#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "lwi/config.h"
#include "lwi/container.h"
#include "lwi/hash.h"
#include "lwi/pe_layout.h"
#include "lwi/win_file.h"
#include "manifest.h"
#include "stamp.h"

namespace fs = std::filesystem;
using namespace lwi;

namespace
{

int fail(const std::string& message)
{
    std::fprintf(stderr, "lwforge: %s\n", message.c_str());
    return 1;
}

int fail(const Status& s)
{
    return fail(s.message());
}

void usage()
{
    std::fputs(
        "lwforge - packages a Locke Werks installer\n"
        "\n"
        "  lwforge build --config <file.toml> --payload <dir> --stub <lwstub.exe>\n"
        "                --out <Setup.exe> [--sign] [--dev]\n"
        "  lwforge inspect <Setup.exe>\n"
        "\n"
        "  --sign  runs scripts/sign.ps1 on the finished installer. Signing is the\n"
        "          last step: everything that rewrites the image must happen first,\n"
        "          because the signature is what makes the bytes immutable.\n"
        "  --dev   marks the container as an unsigned development build, which the\n"
        "          stub displays so it cannot be mistaken for a shipping artifact.\n",
        stderr);
}

/// Flattens a TOML document into dotted keys.
///
/// The stub consumes a flat map, so all structural work happens here. Array
/// indices are kept in the key (preflight.0.type) because ordering matters for
/// actions and hooks and would be lost by a set-like representation.
void flatten(const toml::node& node, const std::string& prefix, Config& out)
{
    if (const auto* table = node.as_table())
    {
        for (const auto& [key, value] : *table)
        {
            const std::string child =
                prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
            flatten(value, child, out);
        }
        return;
    }

    if (const auto* array = node.as_array())
    {
        size_t index = 0;
        for (const auto& element : *array)
        {
            flatten(element, prefix + "." + std::to_string(index), out);
            ++index;
        }
        out.set(prefix + ".count", std::to_string(array->size()));
        return;
    }

    if (const auto* v = node.as_string())
    {
        out.set(prefix, v->get());
    }
    else if (const auto* i = node.as_integer())
    {
        out.set(prefix, std::to_string(i->get()));
    }
    else if (const auto* f = node.as_floating_point())
    {
        out.set(prefix, std::to_string(f->get()));
    }
    else if (const auto* b = node.as_boolean())
    {
        out.set(prefix, b->get() ? "true" : "false");
    }
    else
    {
        // Dates and times have no meaning in an installer config. Stringifying
        // them silently would hide a typo in a key name.
        out.set(prefix, "");
    }
}

struct Args
{
    std::wstring config;
    std::wstring payload;
    std::wstring stub;
    std::wstring out;
    bool sign = false;
    bool dev = false;
};

bool parse_args(int argc, wchar_t** argv, Args& args)
{
    for (int i = 2; i < argc; ++i)
    {
        const std::wstring flag = argv[i];
        const auto next = [&]() -> std::wstring {
            if (i + 1 >= argc)
            {
                return {};
            }
            return argv[++i];
        };

        if (flag == L"--config")
        {
            args.config = next();
        }
        else if (flag == L"--payload")
        {
            args.payload = next();
        }
        else if (flag == L"--stub")
        {
            args.stub = next();
        }
        else if (flag == L"--out")
        {
            args.out = next();
        }
        else if (flag == L"--sign")
        {
            args.sign = true;
        }
        else if (flag == L"--dev")
        {
            args.dev = true;
        }
        else
        {
            std::fprintf(stderr, "lwforge: unknown option %ls\n", flag.c_str());
            return false;
        }
    }
    return true;
}

/// Collects payload files in a stable, locale-independent order.
///
/// Sorted so the same inputs always produce the same container bytes. An
/// ordering that depends on the filesystem's enumeration order would make the
/// pre-signature artifact unreproducible for no reason.
Status collect_payload(const fs::path& root, std::vector<fs::path>& out)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec))
    {
        return Status::error(Code::InvalidArgument,
                             "payload directory does not exist: " + root.string());
    }

    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            return Status::error(Code::IoError, "enumerating payload: " + ec.message());
        }
        if (it->is_regular_file(ec))
        {
            out.push_back(it->path());
        }
    }

    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return a.wstring() < b.wstring();
    });
    return Status::ok();
}

int run_sign(const std::wstring& file)
{
    // Delegates to the PowerShell script rather than reimplementing signtool
    // discovery, so the local and CI paths cannot drift apart.
    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File "
                           L"\"scripts\\sign.ps1\" -FilePath \"" +
                           file + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi))
    {
        return fail(win32_message("CreateProcessW for sign.ps1", GetLastError()));
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code != 0)
    {
        return fail("signing failed");
    }
    return 0;
}

int cmd_build(int argc, wchar_t** argv)
{
    Args args;
    if (!parse_args(argc, argv, args))
    {
        return 1;
    }
    if (args.config.empty() || args.payload.empty() || args.stub.empty() || args.out.empty())
    {
        usage();
        return 1;
    }

    // 1. Parse and flatten the config.
    Config config;
    try
    {
        const toml::table doc = toml::parse_file(to_utf8(args.config));
        flatten(doc, "", config);
    }
    catch (const toml::parse_error& e)
    {
        std::string where;
        if (e.source().begin.line != 0)
        {
            where = ":" + std::to_string(e.source().begin.line) + ":" +
                    std::to_string(e.source().begin.column);
        }
        return fail(to_utf8(args.config) + where + ": " + std::string(e.description()));
    }

    const std::string product = std::string(config.get("product.name"));
    const std::string version = std::string(config.get("product.version"));
    if (product.empty() || version.empty())
    {
        return fail("config must set product.name and product.version");
    }

    // 2. Copy the stub, then strip any signature it carries. Re-forging an
    //    already-signed stub has to start from an unsigned image.
    std::vector<uint8_t> image;
    if (Status s = read_whole_file(args.stub, image); !s)
    {
        return fail(s);
    }
    if (Status s = pe_strip_signature(image); !s)
    {
        return fail(s);
    }

    std::error_code ec;
    fs::create_directories(fs::path(args.out).parent_path(), ec);

    if (Status s = write_whole_file(args.out, image); !s)
    {
        return fail(s);
    }

    // 3. Stamp resources. This rewrites the whole image, so nothing computed
    //    from the layout before this point survives it.
    forge::VersionInfo vi;
    vi.company = std::string(config.get("product.publisher", "Locke Werks"));
    vi.file_description = product + " Installer";
    vi.internal_name = product + "Setup";
    vi.original_filename = fs::path(args.out).filename().string();
    vi.product_name = product;
    vi.legal_copyright = std::string(config.get("product.copyright", "Copyright (c) Locke Werks"));

    {
        unsigned a = 0, b = 0, c = 0;
        std::sscanf(version.c_str(), "%u.%u.%u", &a, &b, &c);
        vi.major = static_cast<uint16_t>(a);
        vi.minor = static_cast<uint16_t>(b);
        vi.patch = static_cast<uint16_t>(c);
    }

    std::vector<uint8_t> icon;
    if (const std::string icon_path(config.get("product.icon")); !icon_path.empty())
    {
        const fs::path resolved = fs::path(args.config).parent_path() / icon_path;
        if (Status s = read_whole_file(resolved.wstring(), icon); !s)
        {
            return fail("reading product.icon: " + s.message());
        }
    }

    // Elevation is a config decision, so the manifest is chosen here rather
    // than baked into the stub at link time. asInvoker plus programmatic
    // elevation is the default: elevating before the user has seen the license
    // is bad manners, and an elevated parent cannot reach the invoking user's
    // HKCU or profile, which per-user actions need.
    const bool require_admin = config.get("install.elevation") == "required";
    const std::string manifest =
        require_admin ? forge::kManifestRequireAdministrator : forge::kManifestAsInvoker;

    if (Status s = forge::stamp_resources(args.out, vi, icon, manifest); !s)
    {
        return fail(s);
    }

    // 4. Re-read and re-parse. EndUpdateResource moved things.
    image.clear();
    if (Status s = read_whole_file(args.out, image); !s)
    {
        return fail(s);
    }
    PeLayout layout;
    if (Status s = pe_parse(image, layout); !s)
    {
        return fail("after resource stamping: " + s.message());
    }
    if (layout.is_signed())
    {
        return fail("stamped image unexpectedly carries a signature");
    }

    // 5. Build the container at the offset it will actually occupy.
    std::vector<uint8_t> config_blob;
    if (Status s = config.encode(config_blob); !s)
    {
        return fail(s);
    }

    ContainerWriter writer(CompressAlgo::Lzms);
    writer.set_config(config_blob);
    if (args.dev)
    {
        writer.set_flags(kFlagDevBuild);
    }

    std::vector<fs::path> files;
    if (Status s = collect_payload(args.payload, files); !s)
    {
        return fail(s);
    }
    if (files.empty())
    {
        // DeadLetter shipped signed installers that failed at the copy step
        // because the payload was staged at the wrong point and nothing
        // checked. An empty payload is almost always that bug.
        return fail("payload directory is empty");
    }

    const fs::path payload_root(args.payload);
    uint64_t raw_total = 0;
    for (const fs::path& file : files)
    {
        std::vector<uint8_t> contents;
        if (Status s = read_whole_file(file.wstring(), contents); !s)
        {
            return fail(s);
        }
        raw_total += contents.size();

        const std::string relative = fs::relative(file, payload_root, ec).string();
        if (Status s = writer.add_file(relative, contents); !s)
        {
            return fail(s);
        }
    }

    std::vector<uint8_t> blob;
    if (Status s = writer.build(layout.payload_end, blob); !s)
    {
        return fail(s);
    }

    // 6. Append, then fix the checksum. The checksum is excluded from the
    //    Authenticode digest, so writing it after appending is safe.
    if (Status s = append_to_file(args.out, blob); !s)
    {
        return fail(s);
    }

    image.clear();
    if (Status s = read_whole_file(args.out, image); !s)
    {
        return fail(s);
    }
    if (image.size() % 8 != 0)
    {
        return fail("packaged image is not 8-byte aligned");
    }
    if (Status s = pe_update_checksum(image); !s)
    {
        return fail(s);
    }
    if (Status s = write_whole_file(args.out, image); !s)
    {
        return fail(s);
    }

    // 7. Read it back the way the stub will. A packaging step that cannot
    //    itself open the artifact has produced a broken installer, and finding
    //    that out here is cheaper than finding out on a customer machine.
    {
        ContainerReader check;
        if (Status s = check.open(image); !s)
        {
            return fail("verification after packaging: " + s.message());
        }
        if (check.files().size() != files.size())
        {
            return fail("verification after packaging: payload count mismatch");
        }
    }

    std::printf("%s %s\n", product.c_str(), version.c_str());
    std::printf("  payload   %zu files, %llu bytes raw\n", files.size(),
                static_cast<unsigned long long>(raw_total));
    std::printf("  container %llu bytes at offset %llu\n",
                static_cast<unsigned long long>(blob.size()),
                static_cast<unsigned long long>(layout.payload_end));
    std::printf("  output    %ls (%llu bytes)\n", args.out.c_str(),
                static_cast<unsigned long long>(image.size()));

    if (args.sign)
    {
        if (const int rc = run_sign(args.out); rc != 0)
        {
            return rc;
        }
        std::printf("  signed    yes\n");
    }
    else
    {
        std::printf("  signed    no (pass --sign)\n");
    }

    return 0;
}

int cmd_inspect(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        usage();
        return 1;
    }
    const std::wstring path = argv[2];

    std::vector<uint8_t> image;
    if (Status s = read_whole_file(path, image); !s)
    {
        return fail(s);
    }

    PeLayout layout;
    if (Status s = pe_parse(image, layout); !s)
    {
        return fail(s);
    }

    std::printf("image        %ls\n", path.c_str());
    std::printf("  size       %llu bytes\n", static_cast<unsigned long long>(layout.file_size));
    std::printf("  format     %s\n", layout.is_pe32_plus ? "PE32+" : "PE32");
    std::printf("  sections   end at %llu\n",
                static_cast<unsigned long long>(layout.payload_start));
    std::printf("  signature  %s\n", layout.is_signed() ? "present" : "absent");
    if (layout.is_signed())
    {
        std::printf("  cert table %u bytes at %u\n", layout.cert_table_size,
                    layout.cert_table_off);
    }
    std::printf("  appended   %llu bytes\n",
                static_cast<unsigned long long>(layout.appended_size()));

    ContainerReader reader;
    if (Status s = reader.open(image); !s)
    {
        std::printf("container    %s\n", s.message().c_str());
        return s.code() == Code::NoContainer ? 0 : 1;
    }

    Config config;
    if (Status s = config.decode(reader.config()); !s)
    {
        return fail(s);
    }

    std::printf("container    ok%s\n", reader.is_dev_build() ? "  [UNSIGNED DEV BUILD]" : "");
    std::printf("  product    %s %s\n", std::string(config.get("product.name")).c_str(),
                std::string(config.get("product.version")).c_str());
    std::printf("  publisher  %s\n", std::string(config.get("product.publisher")).c_str());
    std::printf("  config     %zu keys\n", config.entries().size());
    std::printf("  payload    %zu files\n", reader.files().size());

    for (const FileView& file : reader.files())
    {
        std::printf("    %-48s %10llu  %s\n", file.path.c_str(),
                    static_cast<unsigned long long>(file.raw_size),
                    to_hex(file.digest).substr(0, 16).c_str());
    }

    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        usage();
        return 1;
    }

    const std::wstring command = argv[1];
    if (command == L"build")
    {
        return cmd_build(argc, argv);
    }
    if (command == L"inspect")
    {
        return cmd_inspect(argc, argv);
    }

    usage();
    return 1;
}
