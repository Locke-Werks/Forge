#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <windows.h>

#include <string>
#include <vector>

#include "lwi/container.h"
#include "lwi/pe_layout.h"
#include "lwi/win_file.h"

using namespace lwi;

// A parser that has only ever seen its own generator's output proves very
// little. These cases run against real, Microsoft-signed binaries already on
// the machine, which is where layouts the synthetic fixture never produces show
// up: many sections, PE32 and PE32+ side by side, catalog-signed files with no
// embedded certificate table, and files whose section data is not where a
// naive reading of the headers would put it.

namespace
{

std::wstring system_dir()
{
    std::wstring buf(MAX_PATH, L'\0');
    const UINT n = GetSystemDirectoryW(buf.data(), static_cast<UINT>(buf.size()));
    buf.resize(n);
    return buf;
}

std::vector<std::wstring> sample_binaries()
{
    const std::wstring sys = system_dir();
    std::vector<std::wstring> out;
    for (const wchar_t* name : {L"\\notepad.exe", L"\\kernel32.dll", L"\\advapi32.dll",
                                L"\\shell32.dll", L"\\user32.dll", L"\\cmd.exe", L"\\ntdll.dll",
                                L"\\bcrypt.dll", L"\\cabinet.dll", L"\\d2d1.dll"})
    {
        std::wstring path = sys + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            out.push_back(std::move(path));
        }
    }
    return out;
}

} // namespace

TEST_CASE("pe_parse handles every system binary we can find")
{
    const std::vector<std::wstring> paths = sample_binaries();
    REQUIRE_MESSAGE(!paths.empty(), "no system binaries found to test against");

    int embedded_signed = 0;
    int catalog_signed = 0;

    for (const std::wstring& path : paths)
    {
        std::vector<uint8_t> image;
        const Status read = read_whole_file(path, image);
        if (!read)
        {
            // Some system files are not readable even from an elevated prompt.
            // Skipping is correct; failing would make the suite depend on which
            // machine it runs on.
            continue;
        }

        PeLayout layout;
        const Status s = pe_parse(image, layout);
        INFO("path = " << to_utf8(path));
        REQUIRE(s.is_ok());

        CHECK(layout.file_size == image.size());
        CHECK(layout.size_of_headers > 0);
        CHECK(layout.payload_start >= layout.size_of_headers);
        CHECK(layout.payload_start <= image.size());
        CHECK(layout.payload_end >= layout.payload_start);
        CHECK(layout.payload_end <= image.size());

        if (layout.is_signed())
        {
            ++embedded_signed;

            // The security directory's VirtualAddress is a file offset. If it
            // were being read as an RVA these bounds would be nonsense.
            CHECK(layout.cert_table_off >= layout.payload_start);
            CHECK(static_cast<uint64_t>(layout.cert_table_off) + layout.cert_table_size <=
                  image.size());

            // On conformant signtool output the certificate table is the last
            // thing in the file, so its end is the file end. This is the
            // property that lets the locator use the certificate table offset
            // as the boundary for finding an appended footer.
            CHECK(static_cast<uint64_t>(layout.cert_table_off) + layout.cert_table_size ==
                  image.size());

            // Between the last section and the certificate table there should be
            // nothing but up to 7 bytes of alignment padding. Anything more
            // would mean this binary already carries appended data, which would
            // make it a poor stub candidate.
            CHECK(layout.appended_size() <= 7);
        }
        else
        {
            ++catalog_signed;
            CHECK(layout.payload_end == image.size());
        }

        // No system binary carries our container, and asking must produce a
        // clean "not present" rather than a parse error.
        ContainerReader reader;
        const Status open = reader.open(image);
        CHECK_FALSE(open.is_ok());
        CHECK(open.code() == Code::NoContainer);
    }

    MESSAGE("embedded-signed: " << embedded_signed << "  catalog-signed: " << catalog_signed);
    CHECK(embedded_signed > 0);
}

TEST_CASE("stripping a real signature leaves a parseable image")
{
    const std::wstring sys = system_dir();
    std::vector<uint8_t> image;

    // Find one that actually carries an embedded signature. Catalog-signed
    // files have nothing to strip and would make this vacuous.
    bool found = false;
    for (const wchar_t* name : {L"\\notepad.exe", L"\\cmd.exe", L"\\shell32.dll", L"\\d2d1.dll"})
    {
        std::vector<uint8_t> candidate;
        if (!read_whole_file(sys + name, candidate))
        {
            continue;
        }
        PeLayout layout;
        if (pe_parse(candidate, layout) && layout.is_signed())
        {
            image = std::move(candidate);
            found = true;
            break;
        }
    }

    if (!found)
    {
        MESSAGE("no embedded-signed system binary available; skipping");
        return;
    }

    PeLayout before;
    REQUIRE(pe_parse(image, before).is_ok());
    REQUIRE(before.is_signed());
    const uint64_t original_size = image.size();
    const uint64_t payload_start = before.payload_start;

    REQUIRE(pe_strip_signature(image).is_ok());

    PeLayout after;
    REQUIRE(pe_parse(image, after).is_ok());
    CHECK_FALSE(after.is_signed());
    CHECK(image.size() < original_size);

    // Stripping must not disturb where section data ends. If it did, the
    // container would be appended into the wrong place on a re-forged stub.
    CHECK(after.payload_start == payload_start);
    CHECK(after.payload_end == image.size());
}

TEST_CASE("a real binary survives strip, append, and locate")
{
    const std::wstring sys = system_dir();
    std::vector<uint8_t> image;
    if (!read_whole_file(sys + L"\\notepad.exe", image))
    {
        MESSAGE("notepad.exe unavailable; skipping");
        return;
    }

    REQUIRE(pe_strip_signature(image).is_ok());

    PeLayout layout;
    REQUIRE(pe_parse(image, layout).is_ok());

    ContainerWriter writer(CompressAlgo::Lzms);
    writer.set_config(std::vector<uint8_t>{'c', 'f', 'g'});
    const std::vector<uint8_t> payload(4096, 0x5A);
    REQUIRE(writer.add_file("payload.bin", payload).is_ok());

    std::vector<uint8_t> blob;
    REQUIRE(writer.build(layout.payload_end, blob).is_ok());
    image.insert(image.end(), blob.begin(), blob.end());

    CHECK(image.size() % 8 == 0);

    ContainerReader reader;
    REQUIRE(reader.open(image).is_ok());
    REQUIRE(reader.files().size() == 1);

    std::vector<uint8_t> out;
    REQUIRE(reader.extract(0, out).is_ok());
    CHECK(out == payload);

    // The checksum is excluded from the Authenticode digest, so updating it
    // after appending is safe and must not disturb the container.
    REQUIRE(pe_update_checksum(image).is_ok());
    ContainerReader again;
    REQUIRE(again.open(image).is_ok());
    CHECK(again.files().size() == 1);
}
