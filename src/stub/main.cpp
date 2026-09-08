#include <windows.h>

#include <shellapi.h> // CommandLineToArgvW
#include <shlobj.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "checks.h"
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

// ERROR_PRODUCT_VERSION. It means "another version is installed" with no
// direction implied, so the message has to say which way, but it is the code
// deployment tools recognise for this situation.
constexpr int kExitOtherVersion = 1638;

// ERROR_INSTALL_PREREQUISITE_FAILED. Distinct from a general failure so an
// operator can tell "this machine does not qualify" from "this install broke".
constexpr int kExitPrerequisite = 1603;

struct Options
{
    bool silent = false;
    bool check_only = false;
    bool uninstall = false;
    std::wstring dir;

    // Option overrides in the order they were given, id to state. Kept as
    // written rather than applied here, because nothing has read the config yet
    // and an id cannot be checked against a list that does not exist.
    std::vector<std::pair<std::string, bool>> option_overrides;
    std::wstring bad_option;

    // Set on the temp-directory copy of the uninstaller. It waits for the
    // original to exit, then removes the directory the original was running
    // from. A process cannot unlink its own running image, so the last step of
    // an uninstall has to be issued from somewhere else.
    std::wstring finish_dir;
    DWORD wait_for_pid = 0;
};

/// Parses "id=value" for /O: and --option.
///
/// The spellings are the ones people already type at other installers, because
/// the alternative is that a deployment engineer reads documentation to say yes
/// to a checkbox.
bool parse_option_override(const std::wstring& text, std::pair<std::string, bool>& out)
{
    const size_t equals = text.find(L'=');
    if (equals == std::wstring::npos || equals == 0)
    {
        return false;
    }

    std::wstring value = text.substr(equals + 1);
    for (wchar_t& c : value)
    {
        c = static_cast<wchar_t>(towlower(c));
    }

    bool state = false;
    if (value == L"1" || value == L"true" || value == L"on" || value == L"yes")
    {
        state = true;
    }
    else if (value == L"0" || value == L"false" || value == L"off" || value == L"no")
    {
        state = false;
    }
    else
    {
        return false;
    }

    out = {to_utf8(text.substr(0, equals)), state};
    return true;
}

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
        else if (upper.rfind(L"/O:", 0) == 0 || upper.rfind(L"-O:", 0) == 0)
        {
            std::pair<std::string, bool> override_value;
            if (parse_option_override(arg.substr(3), override_value))
            {
                options.option_overrides.push_back(std::move(override_value));
            }
            else if (options.bad_option.empty())
            {
                options.bad_option = arg;
            }
        }
        else if (upper == L"--OPTION" && i + 1 < count)
        {
            const std::wstring value = argv[++i];
            std::pair<std::string, bool> override_value;
            if (parse_option_override(value, override_value))
            {
                options.option_overrides.push_back(std::move(override_value));
            }
            else if (options.bad_option.empty())
            {
                options.bad_option = value;
            }
        }
    }

    LocalFree(argv);
    return options;
}

/// Maps preflight outcomes onto an exit code.
///
/// Shared by --check-only and the silent path so the two cannot disagree about
/// what a given machine state means. They did: check-only reported a refused
/// downgrade as a generic failure while the installer reported 1638.
int exit_code_for(const std::vector<CheckOutcome>& outcomes)
{
    if (!has_blocking_failure(outcomes))
    {
        return kExitSuccess;
    }
    const bool downgrade = std::any_of(outcomes.begin(), outcomes.end(),
                                       [](const CheckOutcome& outcome) {
                                           return outcome.state == CheckState::Fail &&
                                                  outcome.type == "downgrade";
                                       });
    return downgrade ? kExitOtherVersion : kExitPrerequisite;
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

/// Applies the command line's option overrides onto the plan.
///
/// An id the config never declared is an error rather than something to ignore.
/// Ignoring it is the worst outcome available: the operator believes they turned
/// something off, the default stays on, and the only evidence is on machines
/// nobody is looking at. lwforge cannot catch this one, because the command line
/// is written long after the build.
bool apply_option_overrides(const Options& options, InstallPlan& plan, std::string& error)
{
    if (!options.bad_option.empty())
    {
        error = "not a valid option override: " + to_utf8(options.bad_option) +
                "\nexpected /O:<id>=<on|off>";
        return false;
    }

    for (const auto& [id, state] : options.option_overrides)
    {
        const auto found = std::find_if(plan.options.begin(), plan.options.end(),
                                        [&id](const InstallOption& option) {
                                            return option.id == id;
                                        });
        if (found == plan.options.end())
        {
            error = "there is no option named \"" + id + "\"";
            if (plan.options.empty())
            {
                error += "; this installer declares none";
            }
            else
            {
                error += "; it declares:";
                for (const InstallOption& option : plan.options)
                {
                    error += "\n  " + option.id;
                }
            }
            return false;
        }
        found->selected = state;
    }
    return true;
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
    std::vector<std::string> warnings;
    bool vital_hook_failed = false;
    const Status s = run_uninstall(install_dir, &warnings, &vital_hook_failed);
    CoUninitialize();

    if (options.silent)
    {
        // Same prefix and shape as the install path, so one log format covers
        // both. QuietUninstallString is what Intune, SCCM and winget invoke, and
        // this is the only place a failed cleanup hook can surface for them.
        for (const std::string& warning : warnings)
        {
            write_console("lwi: warning: " + warning + "\n");
        }
    }

    if (!s)
    {
        std::string message = s.message();
        for (const std::string& warning : warnings)
        {
            message += "\n" + warning;
        }
        if (options.silent)
        {
            write_console("lwi: " + s.message() + "\n");
        }
        else
        {
            // One box, not two. The failure is the headline and the warnings are
            // the context for it.
            MessageBoxW(nullptr, to_wide(message).c_str(), L"Uninstall", MB_ICONERROR | MB_OK);
        }
        return kExitFailure;
    }

    if (!warnings.empty() && !options.silent)
    {
        std::string message = "Uninstall finished, but some cleanup steps did not complete:\n";
        for (const std::string& warning : warnings)
        {
            message += "\n" + warning;
        }
        MessageBoxW(nullptr, to_wide(message).c_str(), L"Uninstall", MB_ICONWARNING | MB_OK);
    }

    // Everything is gone except this running image and the directory holding it.
    relaunch_to_finish(self, install_dir);

    // The removal succeeded, so the product is gone either way. 1603 rather than
    // a sixth code, because the five borrowed from MSI are what deployment tools
    // already read without a custom mapping, and a hook the config called vital
    // failing is a failure by the config's own account.
    return vital_hook_failed ? kExitFailure : kExitSuccess;
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
        remove_directory_tree(options.finish_dir);

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

    InstallPlan plan = plan_from_config(config, install_dir);

    // Before anything else looks at the plan, and before the mutex is taken, so
    // a typo on the command line costs nothing and reports itself immediately.
    if (std::string error; !apply_option_overrides(options, plan, error))
    {
        if (options.silent || options.check_only)
        {
            write_console("lwi: " + error + "\n");
        }
        else
        {
            MessageBoxW(nullptr, to_wide(error).c_str(), L"Setup", MB_ICONERROR | MB_OK);
        }
        return kExitFailure;
    }

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
            report += "installed   " + to_utf8(prior) + "\n";
        }
        if (reader.is_dev_build())
        {
            report += "build       UNSIGNED DEV BUILD\n";
        }

        // The effective option set, after the command line. A deployment tool
        // runs --check-only first to decide whether to bother, and this is the
        // only place it can confirm that the /O: switches it passed were spelled
        // the way the installer spells them.
        if (!plan.options.empty())
        {
            report += "\noptions\n";
            for (const InstallOption& option : plan.options)
            {
                report += std::string("  ") + (option.selected ? "[x] " : "[ ] ") + option.id +
                          "   " + to_utf8(option.label) + "\n";
            }
        }

        const std::vector<CheckOutcome> outcomes = run_preflight(config, plan);
        report += "\npreflight\n";
        report += outcomes.empty() ? "  (none declared)\n" : format_outcomes(outcomes);
        write_console(report);

        // --check-only is the flag a deployment tool runs first to decide
        // whether to bother. It must report qualification through its exit
        // code, not only in text nobody parses.
        return exit_code_for(outcomes);
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

    // Preflight runs before anything is written, in every mode. A check that
    // only runs in the interactive path is a check that never runs where it
    // matters, because unattended deployment is exactly where nobody is
    // watching.
    const std::vector<CheckOutcome> outcomes = run_preflight(config, plan);

    if (options.silent)
    {
        if (has_blocking_failure(outcomes))
        {
            write_console("lwi: preflight failed\n" + format_outcomes(outcomes));
            CoUninitialize();
            if (mutex != nullptr)
            {
                ReleaseMutex(mutex);
                CloseHandle(mutex);
            }
            return exit_code_for(outcomes);
        }

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
        // The wizard owns the answers from here. The plan keeps the defaults the
        // config and the command line settled on, and the chosen set is read
        // back once, at the moment the install starts.
        wizard.set_options(plan.options);

        // A machine that does not qualify is told so before it is shown a
        // license to accept. Presenting the license first and failing after
        // wastes the only decision the user was asked to make.
        if (has_blocking_failure(outcomes))
        {
            std::wstring message;
            for (const CheckOutcome& outcome : outcomes)
            {
                if (outcome.state != CheckState::Fail)
                {
                    continue;
                }
                if (!message.empty())
                {
                    message += L"\n\n";
                }
                message += outcome.message;
                if (!outcome.detail.empty())
                {
                    message += L"\n(" + outcome.detail + L")";
                }
            }
            wizard.finish_error(message);
            wizard.run();
            CoUninitialize();
            if (mutex != nullptr)
            {
                ReleaseMutex(mutex);
                CloseHandle(mutex);
            }
            return exit_code_for(outcomes);
        }

        wizard.on_install([&] {
            // COM apartments are per thread, and this thread creates shortcuts.
            const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

            InstallPlan chosen = plan;
            chosen.options = wizard.options();

            // Collected here rather than dropped. Without the argument the
            // parameter defaults to nullptr and report_hooks returns without
            // writing anything, so every hook warning, every failed shortcut and
            // every recovered journal went unreported in the one mode with a
            // person watching.
            std::vector<std::string> warnings;
            const Status s =
                run_install(reader, config, chosen, [&](float fraction, const std::wstring& status) {
                    if (wizard.cancelled())
                    {
                        return false;
                    }
                    wizard.set_progress(fraction, status);
                    return true;
                }, &warnings);

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
                std::vector<std::wstring> wide;
                wide.reserve(warnings.size());
                for (const std::string& warning : warnings)
                {
                    wide.push_back(to_wide(warning));
                }
                wizard.finish_ok(std::move(wide));
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
