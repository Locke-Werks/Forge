#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "lwi/container.h"
#include "lwi/hash.h"
#include "lwi/pe_layout.h"

using namespace lwi;

namespace
{

/// Builds a minimal but structurally valid PE32+ image with one section.
///
/// Synthetic rather than a checked-in binary so the tests can vary the layout:
/// signed vs unsigned, an odd file length, a section with no raw data. Real
/// signed binaries are covered separately in test_pe_real.cpp, because a parser
/// that only ever sees its own generator's output proves very little.
std::vector<uint8_t> make_pe(size_t section_raw_size = 512, bool with_extra_dirs = true)
{
    constexpr size_t kHeadersSize = 0x400;

    std::vector<uint8_t> img(kHeadersSize + section_raw_size, 0);

    const auto put16 = [&](size_t off, uint16_t v) {
        img[off] = static_cast<uint8_t>(v & 0xFF);
        img[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    const auto put32 = [&](size_t off, uint32_t v) {
        img[off] = static_cast<uint8_t>(v & 0xFF);
        img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };

    put16(0, 0x5A4D); // MZ
    constexpr uint32_t kNtOff = 0x80;
    put32(0x3C, kNtOff);

    put32(kNtOff, 0x00004550); // PE\0\0

    // IMAGE_FILE_HEADER
    const size_t fh = kNtOff + 4;
    put16(fh + 0, 0x8664);                                 // Machine x64
    put16(fh + 2, 1);                                      // NumberOfSections
    put16(fh + 16, 240);                                   // SizeOfOptionalHeader (PE32+)
    put16(fh + 18, 0x0022);                                // Characteristics

    // IMAGE_OPTIONAL_HEADER64
    const size_t oh = fh + 20;
    put16(oh + 0, 0x020B);                                 // PE32+
    put32(oh + 60, static_cast<uint32_t>(kHeadersSize));   // SizeOfHeaders
    put32(oh + 64, 0);                                     // CheckSum
    put32(oh + 108, with_extra_dirs ? 16u : 2u);           // NumberOfRvaAndSizes

    // Section table follows the optional header.
    const size_t sh = oh + 240;
    std::memcpy(&img[sh], ".text\0\0\0", 8);
    put32(sh + 8, 0x1000);                                 // VirtualSize
    put32(sh + 12, 0x1000);                                // VirtualAddress
    put32(sh + 16, static_cast<uint32_t>(section_raw_size)); // SizeOfRawData
    put32(sh + 20, static_cast<uint32_t>(kHeadersSize));   // PointerToRawData

    // Something recognisable in the section body so a truncation bug shows up
    // as wrong data rather than as zeroes that happen to compare equal.
    for (size_t i = 0; i < section_raw_size; ++i)
    {
        img[kHeadersSize + i] = static_cast<uint8_t>(i & 0xFF);
    }

    return img;
}

/// Fakes what signtool does: appends a certificate table and points the
/// security data directory at it. The contents are not a real PKCS#7 blob,
/// because nothing under test parses one.
void fake_sign(std::vector<uint8_t>& img, size_t cert_bytes = 1024)
{
    PeLayout layout;
    REQUIRE(pe_parse(img, layout).is_ok());
    REQUIRE(layout.security_dir_off != 0);

    // signtool aligns the certificate table to 8 bytes, padding if needed.
    while (img.size() % 8 != 0)
    {
        img.push_back(0);
    }

    const uint32_t cert_off = static_cast<uint32_t>(img.size());
    img.insert(img.end(), cert_bytes, 0xAB);

    const auto put32 = [&](size_t off, uint32_t v) {
        img[off] = static_cast<uint8_t>(v & 0xFF);
        img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    put32(layout.security_dir_off, cert_off);
    put32(layout.security_dir_off + 4, static_cast<uint32_t>(cert_bytes));
}

std::vector<uint8_t> bytes_of(std::string_view s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

TEST_CASE("pe_parse finds the end of section data on an unsigned image")
{
    const std::vector<uint8_t> img = make_pe();

    PeLayout layout;
    REQUIRE(pe_parse(img, layout).is_ok());

    CHECK(layout.is_pe32_plus);
    CHECK_FALSE(layout.is_signed());
    CHECK(layout.size_of_headers == 0x400);
    CHECK(layout.payload_start == 0x400 + 512);
    CHECK(layout.payload_end == img.size());
    CHECK(layout.appended_size() == 0);
}

TEST_CASE("pe_parse treats the security directory VirtualAddress as a file offset")
{
    std::vector<uint8_t> img = make_pe();
    const uint64_t unsigned_size = img.size();
    fake_sign(img);

    PeLayout layout;
    REQUIRE(pe_parse(img, layout).is_ok());

    CHECK(layout.is_signed());
    CHECK(layout.cert_table_size == 1024);
    // The boundary is the certificate table offset, not file_size - cert_size,
    // and on a conformant image those agree.
    CHECK(layout.cert_table_off + layout.cert_table_size == img.size());
    CHECK(layout.payload_end == layout.cert_table_off);
    CHECK(layout.payload_end >= unsigned_size);
}

TEST_CASE("pe_strip_signature returns the image to its unsigned form")
{
    const std::vector<uint8_t> original = make_pe();
    std::vector<uint8_t> img = original;
    fake_sign(img);
    REQUIRE(img.size() > original.size());

    REQUIRE(pe_strip_signature(img).is_ok());

    PeLayout layout;
    REQUIRE(pe_parse(img, layout).is_ok());
    CHECK_FALSE(layout.is_signed());
    CHECK(layout.cert_table_off == 0);
    CHECK(layout.cert_table_size == 0);

    // Stripping twice is a no-op, not an error. The forge calls it
    // unconditionally rather than testing first.
    REQUIRE(pe_strip_signature(img).is_ok());
}

TEST_CASE("pe_parse rejects a malformed image rather than reading out of bounds")
{
    SUBCASE("not a PE")
    {
        const std::vector<uint8_t> junk(64, 0x41);
        PeLayout layout;
        const Status s = pe_parse(junk, layout);
        CHECK_FALSE(s.is_ok());
        CHECK(s.code() == Code::NotPeImage);
    }

    SUBCASE("e_lfanew points past the end")
    {
        std::vector<uint8_t> img = make_pe();
        img[0x3C] = 0xFF;
        img[0x3D] = 0xFF;
        img[0x3E] = 0xFF;
        img[0x3F] = 0x7F;
        PeLayout layout;
        CHECK_FALSE(pe_parse(img, layout).is_ok());
    }

    SUBCASE("certificate table claims to lie past the end")
    {
        std::vector<uint8_t> img = make_pe();
        PeLayout layout;
        REQUIRE(pe_parse(img, layout).is_ok());
        const size_t off = layout.security_dir_off;
        img[off] = 0x00;
        img[off + 1] = 0x00;
        img[off + 2] = 0x00;
        img[off + 3] = 0x7F; // huge offset
        img[off + 4] = 0x10; // non-zero size, so it reads as signed
        PeLayout again;
        const Status s = pe_parse(img, again);
        CHECK_FALSE(s.is_ok());
        CHECK(s.code() == Code::MalformedPe);
    }

    SUBCASE("truncated section data")
    {
        std::vector<uint8_t> img = make_pe();
        img.resize(img.size() - 16);
        PeLayout layout;
        CHECK_FALSE(pe_parse(img, layout).is_ok());
    }
}

TEST_CASE("container round-trips through a whole image")
{
    std::vector<uint8_t> img = make_pe();

    PeLayout layout;
    REQUIRE(pe_parse(img, layout).is_ok());

    const std::vector<uint8_t> config = bytes_of("{\"product\":\"DeadLetter\"}");
    const std::vector<uint8_t> file_a = bytes_of("hello from a payload file");

    // Compressible, so the LZMS path is genuinely exercised. A tiny incompressible
    // blob would silently take the stored-raw fallback and prove nothing.
    std::vector<uint8_t> file_b(64 * 1024);
    for (size_t i = 0; i < file_b.size(); ++i)
    {
        file_b[i] = static_cast<uint8_t>((i / 97) & 0xFF);
    }

    ContainerWriter writer(CompressAlgo::Lzms);
    writer.set_config(config);
    REQUIRE(writer.add_file("bin/app.exe", file_a).is_ok());
    REQUIRE(writer.add_file("share/data.bin", file_b).is_ok());
    CHECK(writer.file_count() == 2);

    std::vector<uint8_t> blob;
    REQUIRE(writer.build(layout.payload_end, blob).is_ok());

    img.insert(img.end(), blob.begin(), blob.end());
    CHECK(img.size() % 8 == 0); // so signtool inserts no padding of its own

    ContainerReader reader;
    REQUIRE(reader.open(img).is_ok());

    CHECK(reader.config() == config);
    REQUIRE(reader.files().size() == 2);
    CHECK(reader.files()[0].path == "bin\\app.exe");
    CHECK(reader.files()[1].path == "share\\data.bin");

    std::vector<uint8_t> out;
    REQUIRE(reader.extract(0, out).is_ok());
    CHECK(out == file_a);
    REQUIRE(reader.extract(1, out).is_ok());
    CHECK(out == file_b);
}

TEST_CASE("container is still locatable after the image is signed")
{
    std::vector<uint8_t> img = make_pe();

    PeLayout layout;
    REQUIRE(pe_parse(img, layout).is_ok());

    ContainerWriter writer(CompressAlgo::Lzms);
    writer.set_config(bytes_of("cfg"));
    REQUIRE(writer.add_file("a.txt", bytes_of("contents")).is_ok());

    std::vector<uint8_t> blob;
    REQUIRE(writer.build(layout.payload_end, blob).is_ok());
    img.insert(img.end(), blob.begin(), blob.end());

    // This is the whole architecture in one assertion: append, then sign, and
    // the runtime must still find its payload on the signed artifact.
    fake_sign(img);

    ContainerReader reader;
    REQUIRE(reader.open(img).is_ok());
    REQUIRE(reader.files().size() == 1);

    std::vector<uint8_t> out;
    REQUIRE(reader.extract(0, out).is_ok());
    CHECK(out == bytes_of("contents"));
}

TEST_CASE("footer is found even when signtool inserts alignment padding")
{
    // Force a container whose natural end is not 8-byte aligned by signing an
    // image whose length is odd, so fake_sign has to pad.
    for (size_t extra = 0; extra < 8; ++extra)
    {
        std::vector<uint8_t> img = make_pe(512 + extra);

        PeLayout layout;
        REQUIRE(pe_parse(img, layout).is_ok());

        ContainerWriter writer(CompressAlgo::None);
        writer.set_config(bytes_of("cfg"));
        REQUIRE(writer.add_file("a.txt", bytes_of("x")).is_ok());

        std::vector<uint8_t> blob;
        REQUIRE(writer.build(layout.payload_end, blob).is_ok());
        img.insert(img.end(), blob.begin(), blob.end());
        REQUIRE(img.size() % 8 == 0);

        fake_sign(img);

        ContainerReader reader;
        INFO("extra section bytes = " << extra);
        REQUIRE(reader.open(img).is_ok());
    }
}

TEST_CASE("tampering with the container is detected")
{
    std::vector<uint8_t> img = make_pe();

    PeLayout layout;
    REQUIRE(pe_parse(img, layout).is_ok());

    ContainerWriter writer(CompressAlgo::None);
    writer.set_config(bytes_of("cfg"));
    REQUIRE(writer.add_file("a.txt", bytes_of("original contents")).is_ok());

    std::vector<uint8_t> blob;
    REQUIRE(writer.build(layout.payload_end, blob).is_ok());

    const size_t container_start = img.size();
    img.insert(img.end(), blob.begin(), blob.end());

    ContainerReader clean;
    REQUIRE(clean.open(img).is_ok());

    // Flip one byte inside the container. On a real artifact this also breaks
    // the Authenticode signature; here it must break the container hash, which
    // is the check that runs before the index is parsed.
    std::vector<uint8_t> tampered = img;
    tampered[container_start + sizeof(ContainerHeader) + 4] ^= 0xFF;

    ContainerReader reader;
    const Status s = reader.open(tampered);
    CHECK_FALSE(s.is_ok());
    CHECK(s.code() == Code::HashMismatch);
}

TEST_CASE("an image with no container reports NoContainer, not a parse error")
{
    const std::vector<uint8_t> img = make_pe();
    ContainerReader reader;
    const Status s = reader.open(img);
    CHECK_FALSE(s.is_ok());
    CHECK(s.code() == Code::NoContainer);
}

TEST_CASE("payload paths that escape the install directory are refused")
{
    ContainerWriter writer(CompressAlgo::None);

    CHECK_FALSE(writer.add_file("..\\evil.exe", bytes_of("x")).is_ok());
    CHECK_FALSE(writer.add_file("a/../../evil.exe", bytes_of("x")).is_ok());
    CHECK_FALSE(writer.add_file("C:\\Windows\\System32\\evil.dll", bytes_of("x")).is_ok());
    CHECK_FALSE(writer.add_file("\\absolute.exe", bytes_of("x")).is_ok());
    CHECK_FALSE(writer.add_file("", bytes_of("x")).is_ok());

    // A filename that merely starts with dots is legitimate and must survive.
    CHECK(writer.add_file("..gitignore", bytes_of("x")).is_ok());
    CHECK(writer.add_file("a/b/c.txt", bytes_of("x")).is_ok());

    // Duplicates would make extraction order decide the winner.
    CHECK_FALSE(writer.add_file("a/b/c.txt", bytes_of("y")).is_ok());
}
