#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#include <shlobj.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "lwi/config.h"
#include "lwi/container.h"
#include "lwi/win_file.h"
#include "theme.h"
#include "ui.h"

using namespace lwi;
using namespace lwi::stub;

namespace
{

// Exit codes borrowed from MSI so Intune, SCCM and winget interpret them
// without a custom mapping. 0, 1641 and 3010 are all documented as success.
constexpr int kExitSuccess = 0;
constexpr int kExitUserCancel = 1602;
constexpr int kExitFailure = 1603;
constexpr int kExitAlreadyRunning = 1618;

struct Options
{
    bool silent = false;
    bool check_only = false;
    std::wstring dir;
};

Options parse_command_line()
{
    Options options;

    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr)
    {
        return options;
    }

    for (int i = 1; i < count; ++i)
    {
        std::wstring arg = argv[i];
        std::wstring upper = arg;
        for (wchar_t& c : upper)
        {
            c = static_cast<wchar_t>(towupper(c));
        }

        // /S is FULLY silent, matching NSIS and matching the switch winget
        // supplies for that installer type. Inno's /SILENT, which still shows a
        // progress window, is a different mode and would need its own flag.
        if (upper == L"/S" || upper == L"-S" || upper == L"/VERYSILENT" || upper == L"/QUIET" ||
            upper == L"/Q")
        {
            options.silent = true;
        }
        else if (upper == L"--CHECK-ONLY")
        {
            options.check_only = true;
        }
        else if (upper.rfind(L"/D=", 0) == 0 || upper.rfind(L"/DIR=", 0) == 0)
        {
            const size_t eq = arg.find(L'=');
            options.dir = arg.substr(eq + 1);
        }
    }

    LocalFree(argv);
    return options;
}

std::wstring known_folder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)))
    {
        return {};
    }
    std::wstring out(path);
    CoTaskMemFree(path);
    return out;
}

/// Expands the tokens allowed in install.dir.
///
/// Resolved through SHGetKnownFolderPath rather than by reading environment
/// variables, because %ProgramFiles% is rewritten by WOW64 and an environment
/// block is inherited from whoever launched the process.
std::wstring expand_tokens(const std::wstring& input)
{
    struct Token
    {
        const wchar_t* name;
        const KNOWNFOLDERID* id;
    };
    static const Token tokens[] = {
        {L"{ProgramFiles}", &FOLDERID_ProgramFilesX64},
        {L"{ProgramData}", &FOLDERID_ProgramData},
        {L"{LocalAppData}", &FOLDERID_LocalAppData},
        {L"{LocalPrograms}", &FOLDERID_UserProgramFiles},
        {L"{Desktop}", &FOLDERID_Desktop},
    };

    std::wstring out = input;
    for (const Token& token : tokens)
    {
        const size_t pos = out.find(token.name);
        if (pos != std::wstring::npos)
        {
            out.replace(pos, wcslen(token.name), known_folder(*token.id));
        }
    }
    return out;
}

Status ensure_directory(const std::wstring& path)
{
    const std::wstring full = long_path(path);
    if (CreateDirectoryW(full.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return Status::ok();
    }
    if (GetLastError() != ERROR_PATH_NOT_FOUND)
    {
        return Status::error(Code::IoError, win32_message("CreateDirectoryW", GetLastError()));
    }

    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 3)
    {
        return Status::error(Code::IoError, "cannot create directory: " + to_utf8(path));
    }
    if (Status s = ensure_directory(path.substr(0, slash)); !s)
    {
        return s;
    }
    if (!CreateDirectoryW(full.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return Status::error(Code::IoError, win32_message("CreateDirectoryW", GetLastError()));
    }
    return Status::ok();
}

/// Extracts every payload member into target_dir.
///
/// Each member is verified against the SHA-256 in the signed index before it
/// becomes a file, inside ContainerReader::extract. A successful decompression
/// is never taken as proof of integrity.
Status extract_all(const ContainerReader& reader, const std::wstring& target_dir,
                   const std::function<bool(size_t, size_t, const std::string&)>& progress)
{
    if (Status s = ensure_directory(target_dir); !s)
    {
        return s;
    }

    const std::vector<FileView>& files = reader.files();
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (progress && !progress(i, files.size(), files[i].path))
        {
            return Status::error(Code::Ok, "cancelled");
        }

        std::vector<uint8_t> contents;
        if (Status s = reader.extract(i, contents); !s)
        {
            return s;
        }

        const std::wstring destination = target_dir + L"\\" + to_wide(files[i].path);
        const size_t slash = destination.find_last_of(L'\\');
        if (slash != std::wstring::npos)
        {
            if (Status s = ensure_directory(destination.substr(0, slash)); !s)
            {
                return s;
            }
        }

        if (Status s = write_whole_file(destination, contents); !s)
        {
            return s;
        }
    }
    return Status::ok();
}

void write_console(const std::string& text)
{
    // A GUI-subsystem process has no console of its own, which makes writing
    // diagnostics fiddly in exactly two different ways.
    //
    // If the caller redirected stdout to a pipe or a file, that handle IS
    // inherited and is the only sink that reaches them. Check it first: a shell
    // capturing our output gets nothing if we write to CONOUT$ instead, because
    // CONOUT$ goes to the console buffer and bypasses the pipe entirely.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    bool attached_console = false;

    if (out == nullptr || out == INVALID_HANDLE_VALUE)
    {
        // Not redirected. Attach to the parent's console if it has one.
        // ERROR_ACCESS_DENIED means we are already attached, which is success.
        if (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE ||
            GetLastError() == ERROR_ACCESS_DENIED)
        {
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
            attached_console = true;
        }
    }

    // Neither redirected nor attached, which is a double-click from Explorer.
    // A message box beats appearing to do nothing. Safe for automation because
    // anything scripted redirects stdout and took the branch above.
    if (out == nullptr || out == INVALID_HANDLE_VALUE)
    {
        MessageBoxW(nullptr, to_wide(text).c_str(), L"Setup", MB_ICONINFORMATION | MB_OK);
        return;
    }

    DWORD written = 0;
    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);

    if (attached_console)
    {
        CloseHandle(out);
        FreeConsole();
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    // First statement, before anything can be loaded from a directory the user
    // can write to. /DEPENDENTLOADFLAG covers implicit imports; this covers
    // delay-loads and every explicit LoadLibrary, which matters because the
    // stub pulls in d2d1, dwrite, dwmapi and cabinet.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    const Options options = parse_command_line();

    std::wstring self;
    if (Status s = self_path(self); !s)
    {
        return kExitFailure;
    }

    std::vector<uint8_t> image;
    if (Status s = read_whole_file(self, image); !s)
    {
        return kExitFailure;
    }

    ContainerReader reader;
    if (Status s = reader.open(image); !s)
    {
        const std::string message =
            "This installer is incomplete or has been modified.\n" + s.message();
        if (options.silent || options.check_only)
        {
            write_console("lwi: " + message + "\n");
        }
        else
        {
            MessageBoxW(nullptr, to_wide(message).c_str(), L"Setup", MB_ICONERROR | MB_OK);
        }
        return kExitFailure;
    }

    Config config;
    if (Status s = config.decode(reader.config()); !s)
    {
        return kExitFailure;
    }

    std::wstring install_dir = options.dir;
    if (install_dir.empty())
    {
        install_dir = expand_tokens(to_wide(config.get("install.dir")));
    }
    if (install_dir.empty())
    {
        install_dir = known_folder(FOLDERID_ProgramFilesX64) + L"\\" +
                      to_wide(config.get("product.name", "Application"));
    }

    if (options.check_only)
    {
        std::string report;
        report += "product     " + std::string(config.get("product.name")) + " " +
                  std::string(config.get("product.version")) + "\n";
        report += "publisher   " + std::string(config.get("product.publisher")) + "\n";
        report += "install dir " + to_utf8(install_dir) + "\n";
        report += "payload     " + std::to_string(reader.files().size()) + " files\n";
        report += "config      " + std::to_string(config.entries().size()) + " keys\n";
        if (reader.is_dev_build())
        {
            report += "build       UNSIGNED DEV BUILD\n";
        }
        write_console(report);
        return kExitSuccess;
    }

    // One installer at a time. A second copy racing the first over the same
    // target directory is a corrupted install, not a slow one.
    const std::wstring mutex_name =
        L"Global\\lwi-" + to_wide(config.get("product.upgrade_code", "default"));
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return kExitAlreadyRunning;
    }

    if (options.silent)
    {
        const Status s = extract_all(reader, install_dir, nullptr);
        return s.is_ok() ? kExitSuccess : kExitFailure;
    }

    Wizard wizard;
    const Theme theme = Theme::from_config(config);
    if (!wizard.init(instance, config, theme, reader.is_dev_build()))
    {
        MessageBoxW(nullptr, L"Could not create the setup window.", L"Setup", MB_ICONERROR | MB_OK);
        return kExitFailure;
    }
    wizard.set_install_dir(install_dir);

    wizard.on_install([&] {
        const Status s = extract_all(
            reader, install_dir, [&](size_t index, size_t total, const std::string& path) {
                if (wizard.cancelled())
                {
                    return false;
                }
                wizard.set_progress(static_cast<float>(index) / static_cast<float>(total),
                                    to_wide(path));
                return true;
            });

        if (wizard.cancelled())
        {
            wizard.finish_error(L"Installation was cancelled.");
        }
        else if (!s)
        {
            wizard.finish_error(to_wide(s.message()));
        }
        else
        {
            wizard.finish_ok();
        }
    });

    const int code = wizard.run();
    if (mutex != nullptr)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code == 0 ? kExitSuccess : kExitUserCancel;
}
