#include "usercontext.h"

#include <userenv.h>  // CreateEnvironmentBlock
#include <wtsapi32.h> // WTSQueryUserToken

#include <vector>

#include "lwi/win_file.h"

namespace lwi::stub
{
namespace
{

std::wstring token_account(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0)
    {
        return {};
    }

    std::vector<uint8_t> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
    {
        return {};
    }

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t name[256]{};
    wchar_t domain[256]{};
    DWORD name_length = static_cast<DWORD>(std::size(name));
    DWORD domain_length = static_cast<DWORD>(std::size(domain));
    SID_NAME_USE use{};
    if (!LookupAccountSidW(nullptr, user->User.Sid, name, &name_length, domain, &domain_length,
                           &use))
    {
        return {};
    }

    std::wstring out;
    if (domain[0] != L'\0')
    {
        out = domain;
        out += L'\\';
    }
    out += name;
    return out;
}

/// The token of whoever owns the desktop in this session.
///
/// GetShellWindow finds Explorer, and Explorer runs as the person who double
/// clicked setup even when an administrator answered the UAC prompt on their
/// behalf. It is also their FILTERED token, which is the point: the work is
/// meant to happen at medium integrity so the files it writes are ordinary
/// user-owned files.
HANDLE shell_token(std::string& why)
{
    const HWND shell = GetShellWindow();
    if (shell == nullptr)
    {
        why = "no shell window in this session";
        return nullptr;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &pid);
    if (pid == 0)
    {
        why = "the shell window has no process";
        return nullptr;
    }

    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
    {
        why = win32_message("OpenProcess(shell)", GetLastError());
        return nullptr;
    }

    HANDLE opened = nullptr;
    if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, &opened))
    {
        why = win32_message("OpenProcessToken(shell)", GetLastError());
        CloseHandle(process);
        return nullptr;
    }
    CloseHandle(process);

    // A primary token, because it is going to be the token of a new process
    // rather than one this thread wears.
    HANDLE primary = nullptr;
    if (!DuplicateTokenEx(opened, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary,
                          &primary))
    {
        why = win32_message("DuplicateTokenEx(shell)", GetLastError());
        CloseHandle(opened);
        return nullptr;
    }
    CloseHandle(opened);
    return primary;
}

/// The console session's user, asked for directly.
///
/// For a deployment tool running as SYSTEM in session 0 there is no shell to
/// borrow from, so the session has to be named. This needs SE_TCB_NAME, which
/// SYSTEM holds and an elevated administrator does not, so it is not a general
/// fallback for the path above: it is the other half of a pair.
HANDLE console_session_token(std::string& why)
{
    const DWORD session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFFu)
    {
        why = "no console session is attached";
        return nullptr;
    }

    HANDLE user = nullptr;
    if (!WTSQueryUserToken(session, &user))
    {
        // ERROR_NO_TOKEN here means the console session exists but nobody is
        // logged into it, which is the login screen. That is the common state
        // for an Intune or SCCM install and is not a misconfiguration.
        why = win32_message("WTSQueryUserToken", GetLastError());
        return nullptr;
    }

    HANDLE primary = nullptr;
    if (!DuplicateTokenEx(user, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary,
                          &primary))
    {
        why = win32_message("DuplicateTokenEx(session)", GetLastError());
        CloseHandle(user);
        return nullptr;
    }
    CloseHandle(user);
    return primary;
}

} // namespace

UserContext::~UserContext()
{
    if (token_ != nullptr)
    {
        CloseHandle(token_);
    }
}

bool process_is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

Status UserContext::acquire(UserContext& out)
{
    // Not elevated, so this process already is the user. Per-user work runs
    // in-process rather than through a second token, which keeps the unelevated
    // per-user install on one code path instead of two.
    if (!process_is_elevated())
    {
        out.token_ = nullptr;
        out.source_ = "process";

        HANDLE self = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self))
        {
            out.account_ = token_account(self);
            CloseHandle(self);
        }
        return Status::ok();
    }

    std::string why_shell;
    if (HANDLE token = shell_token(why_shell); token != nullptr)
    {
        out.token_ = token;
        out.source_ = "shell";
        out.account_ = token_account(token);
        return Status::ok();
    }

    std::string why_session;
    if (HANDLE token = console_session_token(why_session); token != nullptr)
    {
        out.token_ = token;
        out.source_ = "session";
        out.account_ = token_account(token);
        return Status::ok();
    }

    return Status::error(Code::IoError,
                         "no interactive user to run per-user work as (" + why_shell + "; " +
                             why_session + ")");
}

std::wstring known_folder_for(REFKNOWNFOLDERID id, HANDLE token)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, token, &path)))
    {
        return {};
    }
    std::wstring out(path);
    CoTaskMemFree(path);
    return out;
}

ProcessResult run_process_as(HANDLE token, const std::wstring& exe,
                             const std::wstring& command_line, const std::wstring& working_dir,
                             DWORD timeout_ms)
{
    ProcessResult result;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    wchar_t desktop[] = L"winsta0\\default";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (token != nullptr)
    {
        // Named explicitly when the child may land in another session. A process
        // created with no desktop gets none, and everything it does that touches
        // USER32 then fails in ways that read as the hook being broken.
        si.lpDesktop = desktop;
    }

    PROCESS_INFORMATION pi{};
    std::wstring mutable_command = command_line;
    const wchar_t* directory = working_dir.empty() ? nullptr : working_dir.c_str();

    void* environment = nullptr;
    BOOL created = FALSE;

    if (token == nullptr)
    {
        // lpApplicationName is set explicitly so the executable is never resolved
        // through the search path, and bInheritHandles is FALSE so the child
        // cannot reach anything the installer has open.
        created = CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory, &si, &pi);
        if (created == FALSE)
        {
            result.detail = win32_message("CreateProcessW", GetLastError());
        }
    }
    else
    {
        // The environment block is the whole point of the exercise. Without it
        // the child runs as the right account and still sees the installer's
        // %USERPROFILE% and %APPDATA%, so it writes into the wrong profile with
        // the right credentials and reports success.
        DWORD flags = CREATE_SUSPENDED;
        if (CreateEnvironmentBlock(&environment, token, FALSE))
        {
            flags |= CREATE_UNICODE_ENVIRONMENT;
        }
        else
        {
            environment = nullptr;
        }

        // CREATE_NO_WINDOW is deliberately absent here. CreateProcessWithTokenW
        // documents the flags it accepts and that is not one of them, so a
        // console hook would fail to start rather than start quietly. SW_HIDE in
        // STARTUPINFO hides the console it does get, which is what the flag was
        // there for.
        //
        // Two calls because the privilege needed differs by caller.
        // CreateProcessWithTokenW needs SE_IMPERSONATE_NAME, which an elevated
        // administrator has. CreateProcessAsUserW needs SE_ASSIGNPRIMARYTOKEN,
        // which only SYSTEM has. Trying both covers an elevated interactive
        // install and a deployment tool's SYSTEM install without having to work
        // out from the token which one we are.
        created = CreateProcessWithTokenW(token, 0, exe.c_str(), mutable_command.data(), flags,
                                          environment, directory, &si, &pi);
        const DWORD with_token_error = GetLastError();

        if (created == FALSE)
        {
            created = CreateProcessAsUserW(token, exe.c_str(), mutable_command.data(), nullptr,
                                           nullptr, FALSE, flags, environment, directory, &si, &pi);
            if (created == FALSE)
            {
                result.detail = win32_message("CreateProcessWithTokenW", with_token_error) + "; " +
                                win32_message("CreateProcessAsUserW", GetLastError());
            }
        }
    }

    if (created == FALSE)
    {
        if (environment != nullptr)
        {
            DestroyEnvironmentBlock(environment);
        }
        if (job != nullptr)
        {
            CloseHandle(job);
        }
        return result;
    }

    if (job != nullptr)
    {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    if (pi.hThread != nullptr)
    {
        ResumeThread(pi.hThread);
    }

    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 1);
        result.detail = "timed out after " + std::to_string(timeout_ms) + " ms";
        result.exit_code = WAIT_TIMEOUT;
    }
    else
    {
        GetExitCodeProcess(pi.hProcess, &result.exit_code);
        result.ran = true;
    }

    if (pi.hThread != nullptr)
    {
        CloseHandle(pi.hThread);
    }
    CloseHandle(pi.hProcess);
    if (environment != nullptr)
    {
        DestroyEnvironmentBlock(environment);
    }
    if (job != nullptr)
    {
        CloseHandle(job);
    }
    return result;
}

} // namespace lwi::stub
