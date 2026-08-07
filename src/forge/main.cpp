#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
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

/// Rejects control characters in config values.
///
/// TOML basic strings process backslash escapes, so "C:\bin" is C, colon,
/// BACKSPACE, "in" and "C:\temp" hides a tab. Windows paths are the most common
/// thing anyone puts in this file, which makes it the most common way to author
/// a config that is silently wrong: the installer then creates a shortcut or a
/// PATH entry pointing somewhere that cannot exist, and nothing complains.
///
/// The fix is a literal string ('C:\bin') or a doubled backslash, and the error
/// says so rather than leaving the author to discover it.
int reject_control_characters(const Config& config)
{
    for (const auto& [key, value] : config.entries())
    {
        // License and description text legitimately contain newlines and tabs.
        if (key == "ui.license_text" || key == "ui.warning" || key == "product.description")
        {
            continue;
        }

        for (size_t i = 0; i < value.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c >= 0x20 || c == '\n' || c == '\r')
            {
                continue;
            }

            const char* name = c == '\b'   ? "\\b (backspace)"
                               : c == '\t' ? "\\t (tab)"
                               : c == '\f' ? "\\f (form feed)"
                                           : "a control character";
            return fail(key + " contains " + std::string(name) + " at offset " +
                        std::to_string(i) +
                        ".\n  This is almost always a Windows path in a TOML basic string:"
                        "\n  \"C:\\bin\" is C, colon, backspace, \"in\"."
                        "\n  Use a literal string 'C:\\bin' or double the backslash "
                        "\"C:\\\\bin\".");
        }
    }
    return 0;
}

/// Fills in or verifies the sha256 of every hook against the payload.
///
/// `sha256 = "auto"` is substituted with the real digest. An explicit digest is
/// compared and a mismatch fails the build. Hand-maintaining these is how a
/// pinned hash quietly stops matching the binary it names, at which point the
/// stub refuses to run the hook on a machine where nobody can see why.
int resolve_hook_digests(Config& config, const std::map<std::string, Sha256>& digests)
{
    static const char* kPhases[] = {"hooks.post_extract", "hooks.pre_register",
                                    "hooks.post_install", "hooks.pre_uninstall",
                                    "hooks.post_uninstall"};

    for (const char* phase : kPhases)
    {
        const size_t count = config.array_size(phase);
        for (size_t i = 0; i < count; ++i)
        {
            const std::string prefix = std::string(phase) + "." + std::to_string(i) + ".";
            const std::string run = std::string(config.get(prefix + "run"));
            if (run.empty())
            {
                continue;
            }

            constexpr std::string_view kPayload = "payload:";
            if (run.compare(0, kPayload.size(), kPayload) != 0)
            {
                return fail(prefix + "run must start with payload: (got \"" + run + "\")");
            }

            std::string relative = run.substr(kPayload.size());
            for (char& c : relative)
            {
                if (c == '/')
                {
                    c = '\\';
                }
            }

            const auto found = digests.find(relative);
            if (found == digests.end())
            {
                return fail(prefix + "run names \"" + relative +
                            "\", which is not in the payload");
            }

            const std::string actual = to_hex(found->second);
            const std::string declared = std::string(config.get(prefix + "sha256"));

            if (declared.empty() || declared == "auto")
            {
                config.set(prefix + "sha256", actual);
                std::printf("  hook      %s -> %s\n", relative.c_str(),
                            actual.substr(0, 16).c_str());
            }
            else if (declared != actual)
            {
                return fail(prefix + "sha256 does not match " + relative + "\n" +
                            "  declared " + declared + "\n" + "  actual   " + actual +
                            "\n  set it to \"auto\" to have lwforge fill it in");
            }
        }
    }
    return 0;
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

    if (const int rc = reject_control_characters(config); rc != 0)
    {
        return rc;
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
    // than baked into the stub at link time. requireAdministrator is the
    // default because the stub does not elevate itself: a machine-scope install
    // that starts unelevated dies at the first write to Program Files or HKLM,
    // and machine scope is the common case. on-demand opts out and buys a
    // license page before the UAC prompt plus a token that can still reach the
    // invoking user's HKCU and profile, which per-user actions need.
    //
    // A value that is neither fails the build. Silently falling back would now
    // hand an installer more privilege than its author asked for.
    const std::string_view elevation = config.get("install.elevation", "required");
    if (elevation != "required" && elevation != "on-demand")
    {
        return fail("install.elevation must be \"required\" or \"on-demand\", not \"" +
                    std::string(elevation) + "\"");
    }
    const std::string manifest = elevation == "on-demand" ? forge::kManifestAsInvoker
                                                          : forge::kManifestRequireAdministrator;

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
    ContainerWriter writer(CompressAlgo::Lzms);
    if (args.dev)
    {
        writer.set_flags(kFlagDevBuild);
    }

    // Payload digests, keyed by normalised relative path. Hook pinning is
    // resolved against these before the config is encoded, so a hook whose
    // digest does not match the file it names fails the BUILD rather than
    // failing on a customer machine where nobody can read the message.
    std::map<std::string, Sha256> digests;

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

    // Embed the stub as the uninstaller.
    //
    // It is added as an ordinary payload member so it is extracted, hashed and
    // verified like everything else, and so the uninstaller left behind on a
    // customer machine is the SAME signed binary rather than something the
    // installer stamps out at runtime. An unsigned uninstaller sitting in
    // Program Files and running elevated is exactly what SmartScreen and Smart
    // App Control are there to complain about.
    {
        std::vector<uint8_t> uninstaller;
        if (Status s = read_whole_file(args.stub, uninstaller); !s)
        {
            return fail("reading stub for the embedded uninstaller: " + s.message());
        }

        // The uninstaller is whatever stub was handed in, so it is signed only
        // if that stub was already signed. Signing the finished installer does
        // nothing for the copy inside it: that copy is payload, hashed and
        // extracted, not re-signed on the way out.
        //
        // An unsigned uninstaller sitting permanently in Program Files and
        // invoked elevated by Settings is exactly what the embedding was meant
        // to avoid, so a release build refuses rather than shipping one.
        PeLayout stub_layout;
        if (Status s = pe_parse(uninstaller, stub_layout); !s)
        {
            return fail("stub is not a valid PE: " + s.message());
        }
        if (!stub_layout.is_signed() && !args.dev)
        {
            return fail(
                "the stub is unsigned, so the embedded uninstaller would be too.\n"
                "  Sign the stub before forging, or pass --dev to accept it.\n"
                "  The release path is two stages: build and sign lwstub.exe with the\n"
                "  release-stage1 preset, then forge against the signed copy.");
        }
        if (Status s = writer.add_file(".lw\\uninstall.exe", uninstaller); !s)
        {
            return fail(s);
        }
        Sha256 digest{};
        if (Status s = sha256(uninstaller, digest); !s)
        {
            return fail(s);
        }
        digests[".lw\\uninstall.exe"] = digest;
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

        std::string relative = fs::relative(file, payload_root, ec).string();
        if (Status s = writer.add_file(relative, contents); !s)
        {
            return fail(s);
        }

        Sha256 digest{};
        if (Status s = sha256(contents, digest); !s)
        {
            return fail(s);
        }
        for (char& c : relative)
        {
            if (c == '/')
            {
                c = '\\';
            }
        }
        digests[relative] = digest;
    }

    // Resolve hook pinning now that every payload digest is known.
    if (const int rc = resolve_hook_digests(config, digests); rc != 0)
    {
        return rc;
    }

    std::vector<uint8_t> config_blob;
    if (Status s = config.encode(config_blob); !s)
    {
        return fail(s);
    }
    writer.set_config(config_blob);

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
        // files.size() + 1 for the embedded uninstaller.
        if (check.files().size() != files.size() + 1)
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
    for (const auto& [key, value] : config.entries())
    {
        // Truncated because a license body is thousands of characters and would
        // bury everything else. The point of inspect is to see the structure.
        std::string shown = value;
        if (shown.size() > 60)
        {
            shown = shown.substr(0, 57) + "...";
        }
        for (char& c : shown)
        {
            if (c == '\n' || c == '\r')
            {
                c = ' ';
            }
        }
        std::printf("    %-40s %s\n", key.c_str(), shown.c_str());
    }
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
