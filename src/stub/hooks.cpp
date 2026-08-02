#include "hooks.h"

#include <windows.h>

#include <algorithm>

#include "lwi/hash.h"
#include "lwi/win_file.h"

namespace lwi::stub
{
namespace
{

constexpr DWORD kDefaultTimeoutMs = 120000;

/// Rejects anything that is not a plain relative path inside the payload.
///
/// The "payload:" prefix is required rather than optional so a config cannot
/// name an arbitrary file on the machine by accident or otherwise. Everything
/// after it is treated as data, never as a command line.
bool resolve_hook_target(const std::string& spec, const std::wstring& install_dir,
                         std::wstring& out, std::string& why)
{
    constexpr std::string_view kPrefix = "payload:";
    if (spec.size() <= kPrefix.size() || spec.compare(0, kPrefix.size(), kPrefix) != 0)
    {
        why = "hook target must start with payload:";
        return false;
    }

    std::string relative = spec.substr(kPrefix.size());
    for (char& c : relative)
    {
        if (c == '/')
        {
            c = '\\';
        }
    }

    if (relative.empty() || relative.front() == '\\' ||
        (relative.size() >= 2 && relative[1] == ':'))
    {
        why = "hook target must be a relative payload path";
        return false;
    }

    size_t start = 0;
    while (start <= relative.size())
    {
        const size_t end = relative.find('\\', start);
        const std::string_view segment = std::string_view(relative).substr(
            start, (end == std::string::npos ? relative.size() : end) - start);
        if (segment == ".." || segment == ".")
        {
            why = "hook target contains a relative segment";
            return false;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }

    out = install_dir + L"\\" + to_wide(relative);
    return true;
}

/// Runs a process and waits, killing it and everything it spawned on timeout.
///
/// The job object is what makes the timeout mean anything: without it, killing
/// the child leaves its children running and the installer proceeds while work
/// it believes finished is still in flight.
bool run_process(const std::wstring& exe, const std::wstring& command_line,
                 const std::wstring& working_dir, DWORD timeout_ms, DWORD& exit_code,
                 std::string& detail)
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    std::wstring mutable_command = command_line;

    // lpApplicationName is set explicitly so the executable is never resolved
    // through the search path, and bInheritHandles is FALSE so the child cannot
    // reach anything the installer has open.
    const BOOL created =
        CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                       working_dir.empty() ? nullptr : working_dir.c_str(), &si, &pi);
    if (!created)
    {
        detail = win32_message("CreateProcessW", GetLastError());
        if (job != nullptr)
        {
            CloseHandle(job);
        }
        return false;
    }

    if (job != nullptr)
    {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);

    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    bool ok = false;

    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 1);
        detail = "timed out after " + std::to_string(timeout_ms) + " ms";
        exit_code = WAIT_TIMEOUT;
    }
    else
    {
        GetExitCodeProcess(pi.hProcess, &exit_code);
        ok = true;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job != nullptr)
    {
        CloseHandle(job);
    }
    return ok;
}

std::wstring quote_argument(const std::wstring& value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos)
    {
        return value;
    }
    std::wstring out = L"\"";
    for (size_t i = 0; i < value.size(); ++i)
    {
        size_t backslashes = 0;
        while (i < value.size() && value[i] == L'\\')
        {
            ++backslashes;
            ++i;
        }
        if (i == value.size())
        {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (value[i] == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
        }
        else
        {
            out.append(backslashes, L'\\');
        }
        out.push_back(value[i]);
    }
    out.push_back(L'"');
    return out;
}

std::wstring expand(const std::wstring& text, const InstallPlan& plan)
{
    struct Token
    {
        const wchar_t* name;
        const std::wstring* value;
    };
    const Token tokens[] = {
        {L"{InstallDir}", &plan.install_dir},
        {L"{Version}", &plan.version},
        {L"{Product}", &plan.product},
    };

    std::wstring out = text;
    for (const Token& token : tokens)
    {
        size_t pos = out.find(token.name);
        while (pos != std::wstring::npos)
        {
            out.replace(pos, wcslen(token.name), *token.value);
            pos = out.find(token.name, pos + token.value->size());
        }
    }
    return out;
}

} // namespace

const char* phase_key(Phase phase)
{
    switch (phase)
    {
    case Phase::PostExtract:
        return "hooks.post_extract";
    case Phase::PreRegister:
        return "hooks.pre_register";
    case Phase::PostInstall:
        return "hooks.post_install";
    case Phase::PreUninstall:
        return "hooks.pre_uninstall";
    case Phase::PostUninstall:
        return "hooks.post_uninstall";
    }
    return "hooks.unknown";
}

bool run_hooks(Phase phase, const Config& config, const InstallPlan& plan,
               std::vector<HookOutcome>& outcomes)
{
    const std::string base = phase_key(phase);
    const size_t count = config.array_size(base);

    for (size_t i = 0; i < count; ++i)
    {
        const std::string prefix = base + "." + std::to_string(i) + ".";

        HookOutcome outcome;
        outcome.id = std::string(config.get(prefix + "id", prefix));
        outcome.vital = config.get_bool(prefix + "vital", false);

        const std::string spec = std::string(config.get(prefix + "run"));
        std::wstring exe;
        std::string why;
        if (!resolve_hook_target(spec, plan.install_dir, exe, why))
        {
            outcome.detail = why;
            outcomes.push_back(outcome);
            if (outcome.vital)
            {
                return false;
            }
            continue;
        }

        // The digest is not optional. Without it the config could point at any
        // payload member and the signature would still cover the config, so the
        // pinning is what ties a hook to one specific set of bytes.
        Sha256 expected{};
        const std::string digest_text = std::string(config.get(prefix + "sha256"));
        if (!from_hex(digest_text, expected))
        {
            outcome.detail = "hook has no valid sha256";
            outcomes.push_back(outcome);
            if (outcome.vital)
            {
                return false;
            }
            continue;
        }

        Sha256 actual{};
        if (Status s = sha256_file(exe, actual); !s)
        {
            outcome.detail = "cannot hash hook binary: " + s.message();
            outcomes.push_back(outcome);
            if (outcome.vital)
            {
                return false;
            }
            continue;
        }

        if (actual != expected)
        {
            outcome.detail = "hook binary does not match its pinned digest";
            outcomes.push_back(outcome);
            // Always fatal, vital or not. A digest mismatch means the file on
            // disk is not the file the signature vouched for.
            return false;
        }

        // Build the command line from an explicit argv. There is no shell
        // anywhere in this path, so nothing in the config can be interpreted as
        // an operator, a redirect, or a second command.
        std::wstring command = quote_argument(exe);
        const size_t arg_count = config.array_size(prefix + "args");
        for (size_t a = 0; a < arg_count; ++a)
        {
            const std::wstring argument =
                expand(to_wide(config.get(prefix + "args." + std::to_string(a))), plan);
            command += L" " + quote_argument(argument);
        }

        const DWORD timeout =
            static_cast<DWORD>(config.get_int(prefix + "timeout_ms", kDefaultTimeoutMs));

        DWORD exit_code = 0;
        std::string detail;
        outcome.ran = run_process(exe, command, plan.install_dir, timeout, exit_code, detail);
        outcome.exit_code = exit_code;
        outcome.detail = detail;

        if (outcome.ran)
        {
            // An explicit list of acceptable codes, defaulting to zero. Some
            // tools report "done, restart needed" as 3010 and that is a success.
            std::vector<int64_t> accepted;
            const size_t expect_count = config.array_size(prefix + "expect_exit");
            for (size_t e = 0; e < expect_count; ++e)
            {
                accepted.push_back(
                    config.get_int(prefix + "expect_exit." + std::to_string(e), 0));
            }
            if (accepted.empty())
            {
                accepted.push_back(0);
            }

            outcome.ok = std::find(accepted.begin(), accepted.end(),
                                   static_cast<int64_t>(exit_code)) != accepted.end();
            if (!outcome.ok)
            {
                outcome.detail = "exited with " + std::to_string(exit_code);
            }
        }

        outcomes.push_back(outcome);

        if (!outcome.ok && outcome.vital)
        {
            return false;
        }
    }

    return true;
}

} // namespace lwi::stub
