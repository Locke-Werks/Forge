#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#include <shlobj.h>

#include <string>
#include <vector>

#include "lwi/config.h"
#include "lwi/container.h"
#include "lwi/win_file.h"
#include "ops.h"
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
    bool uninstall = false;
    std::wstring dir;

    // Set on the temp-directory copy of the uninstaller. It waits for the
    // original to exit, then removes the directory the original was running
    // from. A process cannot unlink its own running image, so the last step of
    // an uninstall has to be issued from somewhere else.
    std::wstring finish_dir;
    DWORD wait_for_pid = 0;
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
        const std::wstring arg = argv[i];
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
        else if (upper == L"/UNINSTALL" || upper == L"--UNINSTALL")
        {
            options.uninstall = true;
        }
        else if (upper == L"--FINISH-UNINSTALL" && i + 2 < count)
        {
            options.finish_dir = argv[++i];
            options.wait_for_pid = static_cast<DWORD>(_wtoi(argv[++i]));
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

void remove_tree(const std::wstring& path)
{
    WIN32_FIND_DATAW find{};
    const HANDLE handle = FindFirstFileW(long_path(path + L"\\*").c_str(), &find);
    if (handle != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = find.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring child = path + L"\\" + name;
            if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                remove_tree(child);
            }
            else
            {
                SetFileAttributesW(long_path(child).c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(long_path(child).c_str());
            }
        } while (FindNextFileW(handle, &find));
        FindClose(handle);
    }
    RemoveDirectoryW(long_path(path).c_str());
}

/// Relaunches a copy of this executable from the temp directory so it can
/// delete the directory this one is running from.
///
/// A running image cannot be unlinked, so the final removal has to be issued by
/// a process that does not live inside the doomed directory.
/// MOVEFILE_DELAY_UNTIL_REBOOT would also work, but it needs administrator
/// rights and leaves the product looking installed until the machine restarts.
Status relaunch_to_finish(const std::wstring& self, const std::wstring& install_dir)
{
    wchar_t temp_dir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp_dir) == 0)
    {
        return Status::error(Code::IoError, win32_message("GetTempPathW", GetLastError()));
    }

    wchar_t temp_file[MAX_PATH]{};
    if (GetTempFileNameW(temp_dir, L"lwu", 0, temp_file) == 0)
    {
        return Status::error(Code::IoError, win32_message("GetTempFileNameW", GetLastError()));
    }

    std::wstring copy = temp_file;
    copy += L".exe";
    DeleteFileW(temp_file);

    if (!CopyFileW(long_path(self).c_str(), long_path(copy).c_str(), FALSE))
    {
        return Status::error(Code::IoError, win32_message("CopyFileW", GetLastError()));
    }

    std::wstring command = L"\"" + copy + L"\" --finish-uninstall \"" + install_dir + L"\" " +
                           std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, temp_dir, &si, &pi))
    {
        return Status::error(Code::IoError, win32_message("CreateProcessW", GetLastError()));
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return Status::ok();
}

/// The install directory, given that the uninstaller runs from
/// <InstallDir>\.lw\uninstall.exe. Two levels up.
std::wstring install_dir_from_uninstaller(const std::wstring& self)
{
    size_t slash = self.find_last_of(L'\\');
    if (slash == std::wstring::npos)
    {
        return {};
    }
    const std::wstring meta = self.substr(0, slash);
    slash = meta.find_last_of(L'\\');
    if (slash == std::wstring::npos)
    {
        return {};
    }
    return meta.substr(0, slash);
}

int run_uninstall_mode(const Options& options, const std::wstring& self)
{
    const std::wstring install_dir =
        options.dir.empty() ? install_dir_from_uninstaller(self) : options.dir;
    if (install_dir.empty())
    {
        return kExitFailure;
    }

    if (!options.silent)
    {
        const std::wstring prompt = L"Remove " + install_dir + L"?";
        if (MessageBoxW(nullptr, prompt.c_str(), L"Uninstall", MB_ICONQUESTION | MB_OKCANCEL) !=
            IDOK)
        {
            return kExitUserCancel;
        }
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        return kExitFailure;
    }
    const Status s = run_uninstall(install_dir);
    CoUninitialize();

    if (!s)
    {
        if (options.silent)
        {
            write_console("lwi: " + s.message() + "\n");
        }
        else
        {
            MessageBoxW(nullptr, to_wide(s.message()).c_str(), L"Uninstall", MB_ICONERROR | MB_OK);
        }
        return kExitFailure;
    }

    // Everything is gone except this running image and the directory holding it.
    relaunch_to_finish(self, install_dir);
    return kExitSuccess;
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

    // The temp-directory copy finishing an uninstall. It carries no container
    // and touches nothing but the directory it was told to remove.
    if (!options.finish_dir.empty())
    {
        if (options.wait_for_pid != 0)
        {
            const HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, options.wait_for_pid);
            if (parent != nullptr)
            {
                WaitForSingleObject(parent, 30000);
                CloseHandle(parent);
            }
        }
        remove_tree(options.finish_dir);

        // Schedule this copy's own removal. Best effort: MOVEFILE_DELAY_UNTIL_
        // REBOOT needs administrator rights, so an unelevated per-user
        // uninstall leaves one small file in %TEMP% until the directory is
        // cleaned. NSIS has the same residue for the same reason. Leaking a
        // file beats leaving the product's directory behind.
        MoveFileExW(long_path(self).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return kExitSuccess;
    }

    if (options.uninstall)
    {
        return run_uninstall_mode(options, self);
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

    const InstallPlan plan = plan_from_config(config, install_dir);

    if (options.check_only)
    {
        std::string report;
        report += "product     " + std::string(config.get("product.name")) + " " +
                  std::string(config.get("product.version")) + "\n";
        report += "publisher   " + std::string(config.get("product.publisher")) + "\n";
        report += "scope       " +
                  std::string(plan.scope == Scope::Machine ? "machine" : "user") + "\n";
        report += "install dir " + to_utf8(install_dir) + "\n";
        report += "payload     " + std::to_string(reader.files().size()) + " files\n";
        report += "config      " + std::to_string(config.entries().size()) + " keys\n";
        if (const std::wstring prior = installed_version(plan); !prior.empty())
        {
            report += "installed   " + to_utf8(prior) + " (this run would upgrade)\n";
        }
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

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        return kExitFailure;
    }

    int result = kExitSuccess;

    if (options.silent)
    {
        std::vector<std::string> warnings;
        const Status s = run_install(reader, config, plan, nullptr, &warnings);
        for (const std::string& warning : warnings)
        {
            write_console("lwi: warning: " + warning + "\n");
        }
        if (!s)
        {
            write_console("lwi: " + s.message() + "\n");
        }
        result = s.is_ok() ? kExitSuccess : kExitFailure;
    }
    else
    {
        Wizard wizard;
        const Theme theme = Theme::from_config(config);
        if (!wizard.init(instance, config, theme, reader.is_dev_build()))
        {
            MessageBoxW(nullptr, L"Could not create the setup window.", L"Setup",
                        MB_ICONERROR | MB_OK);
            CoUninitialize();
            return kExitFailure;
        }
        wizard.set_install_dir(install_dir);

        wizard.on_install([&] {
            // COM apartments are per thread, and this thread creates shortcuts.
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

            const Status s =
                run_install(reader, config, plan, [&](float fraction, const std::wstring& status) {
                    if (wizard.cancelled())
                    {
                        return false;
                    }
                    wizard.set_progress(fraction, status);
                    return true;
                });

            if (com)
            {
                CoUninitialize();
            }

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

        result = wizard.run() == 0 ? kExitSuccess : kExitUserCancel;
    }

    CoUninitialize();

    if (mutex != nullptr)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return result;
}
