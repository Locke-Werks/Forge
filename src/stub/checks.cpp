#include "checks.h"

#include <windows.h>

#include <restartmanager.h>
#include <tlhelp32.h>

#include <algorithm>

#include "lwi/version.h"
#include "lwi/win_file.h"

namespace lwi::stub
{
namespace
{

/// The real OS build number.
///
/// GetVersionEx and VerifyVersionInfo are shimmed: without the right
/// supportedOS GUIDs in the manifest they report 6.2 forever, and even with
/// them they are subject to compatibility shims. RtlGetVersion is not, which is
/// why it is the one Microsoft points at for version detection.
RTL_OSVERSIONINFOEXW real_os_version()
{
    RTL_OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr)
    {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOEXW);
        const auto fn =
            reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn != nullptr)
        {
            fn(&info);
        }
    }
    return info;
}

bool is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool process_running(const std::wstring& name)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, name.c_str()) == 0)
            {
                // Our own process would match if a product were ever named the
                // same as its installer.
                if (entry.th32ProcessID != GetCurrentProcessId())
                {
                    found = true;
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

uint64_t free_space_for(const std::wstring& path)
{
    // Walk up to the nearest existing ancestor: the install directory usually
    // does not exist yet, and GetDiskFreeSpaceEx needs a real one.
    std::wstring probe = path;
    while (!probe.empty())
    {
        if (GetFileAttributesW(long_path(probe).c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            break;
        }
        const size_t slash = probe.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash < 3)
        {
            probe = probe.substr(0, 3); // "C:\"
            break;
        }
        probe = probe.substr(0, slash);
    }

    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(probe.c_str(), &available, nullptr, nullptr))
    {
        return 0;
    }
    return available.QuadPart;
}

uint64_t total_ram()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
    {
        return 0;
    }
    return status.ullTotalPhys;
}

bool registry_key_exists(HKEY root, const std::wstring& subkey)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    RegCloseKey(key);
    return true;
}

bool registry_value_exists(HKEY root, const std::wstring& subkey, const std::wstring& value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    const bool found =
        RegQueryValueExW(key, value.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

/// The three places Windows records that a restart is outstanding.
///
/// Only the first lives under Session Manager, and it is a VALUE rather than a
/// key. The other two are keys under a different hive path entirely. Writing
/// them as "Session Manager\..." is a common and wrong shorthand.
bool reboot_pending()
{
    if (registry_value_exists(HKEY_LOCAL_MACHINE,
                              L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                              L"PendingFileRenameOperations"))
    {
        return true;
    }
    if (registry_key_exists(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based "
                            L"Servicing\\RebootPending"))
    {
        return true;
    }
    return registry_key_exists(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired");
}

HKEY hive_from_name(std::string_view name)
{
    if (name == "HKCU" || name == "HKEY_CURRENT_USER")
    {
        return HKEY_CURRENT_USER;
    }
    if (name == "HKCR" || name == "HKEY_CLASSES_ROOT")
    {
        return HKEY_CLASSES_ROOT;
    }
    return HKEY_LOCAL_MACHINE;
}

std::wstring default_message(const std::string& type)
{
    if (type == "os_build")
    {
        return L"This version of Windows is too old.";
    }
    if (type == "free_disk_space")
    {
        return L"There is not enough free disk space.";
    }
    if (type == "process_not_running")
    {
        return L"An application that must be closed is running.";
    }
    if (type == "min_ram")
    {
        return L"This machine does not have enough memory.";
    }
    if (type == "elevation")
    {
        return L"Administrator rights are required.";
    }
    if (type == "no_reboot_pending")
    {
        return L"A restart is pending. Restart before installing.";
    }
    return L"A requirement was not met.";
}

std::wstring format_bytes(uint64_t bytes)
{
    const wchar_t* units[] = {L"bytes", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, unit == 0 ? L"%.0f %s" : L"%.1f %s", value, units[unit]);
    return buffer;
}

} // namespace

std::vector<std::wstring> processes_using(const std::wstring& directory,
                                          const std::vector<std::wstring>& files)
{
    std::vector<std::wstring> out;

    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS)
    {
        return out;
    }

    std::vector<std::wstring> paths;
    paths.reserve(files.size());
    for (const std::wstring& relative : files)
    {
        paths.push_back(directory + L"\\" + relative);
    }

    std::vector<LPCWSTR> raw;
    raw.reserve(paths.size());
    for (const std::wstring& path : paths)
    {
        raw.push_back(path.c_str());
    }

    if (!raw.empty() &&
        RmRegisterResources(session, static_cast<UINT>(raw.size()), raw.data(), 0, nullptr, 0,
                            nullptr) != ERROR_SUCCESS)
    {
        RmEndSession(session);
        return out;
    }

    UINT needed = 0;
    UINT count = 0;
    DWORD reason = 0;
    // Two-call sizing. The first asks how many entries there are; the array is
    // then sized to that rather than to a guess that goes stale between calls.
    RmGetList(session, &needed, &count, nullptr, &reason);
    if (needed > 0)
    {
        std::vector<RM_PROCESS_INFO> info(needed);
        count = needed;
        if (RmGetList(session, &needed, &count, info.data(), &reason) == ERROR_SUCCESS)
        {
            for (UINT i = 0; i < count; ++i)
            {
                // RmCritical covers more than critical system processes: it also
                // means a permission failure, or our own process. Either way it
                // is not something to offer to close.
                if (info[i].ApplicationType == RmCritical)
                {
                    continue;
                }
                if (info[i].Process.dwProcessId == GetCurrentProcessId())
                {
                    continue;
                }
                out.emplace_back(info[i].strAppName);
            }
        }
    }

    RmEndSession(session);

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<CheckOutcome> run_preflight(const Config& config, const InstallPlan& plan)
{
    std::vector<CheckOutcome> outcomes;

    const auto add = [&](CheckState state, std::string type, std::wstring message,
                         std::wstring detail, bool recoverable = false) {
        CheckOutcome outcome;
        outcome.state = state;
        outcome.type = std::move(type);
        outcome.message = std::move(message);
        outcome.detail = std::move(detail);
        outcome.recoverable = recoverable;
        outcomes.push_back(std::move(outcome));
    };

    const size_t count = config.array_size("preflight");
    for (size_t i = 0; i < count; ++i)
    {
        const std::string prefix = "preflight." + std::to_string(i) + ".";
        const std::string type = std::string(config.get(prefix + "type"));
        if (type.empty())
        {
            continue;
        }

        std::wstring message = to_wide(config.get(prefix + "fail"));
        if (message.empty())
        {
            message = default_message(type);
        }

        if (type == "os_build")
        {
            const RTL_OSVERSIONINFOEXW os = real_os_version();
            const int64_t minimum = config.get_int(prefix + "min", 0);
            const bool ok = static_cast<int64_t>(os.dwBuildNumber) >= minimum;
            add(ok ? CheckState::Pass : CheckState::Fail, type, message,
                L"build " + std::to_wstring(os.dwBuildNumber) + L", requires " +
                    std::to_wstring(minimum));
        }
        else if (type == "free_disk_space")
        {
            const uint64_t required = static_cast<uint64_t>(config.get_int(prefix + "bytes", 0));
            const uint64_t available = free_space_for(plan.install_dir);
            add(available >= required ? CheckState::Pass : CheckState::Fail, type, message,
                format_bytes(available) + L" available, requires " + format_bytes(required));
        }
        else if (type == "min_ram")
        {
            const uint64_t required = static_cast<uint64_t>(config.get_int(prefix + "bytes", 0));
            const uint64_t available = total_ram();
            add(available >= required ? CheckState::Pass : CheckState::Fail, type, message,
                format_bytes(available) + L" installed, requires " + format_bytes(required));
        }
        else if (type == "process_not_running")
        {
            const std::wstring name = to_wide(config.get(prefix + "name"));
            const bool running = !name.empty() && process_running(name);
            // Recoverable when the config allows the Restart Manager to close
            // it, because then the user has a button rather than a dead end.
            const bool rm = config.get_bool(prefix + "restart_manager", false);
            add(running ? CheckState::Fail : CheckState::Pass, type, message,
                running ? name + L" is running" : name + L" is not running", running && rm);
        }
        else if (type == "elevation")
        {
            add(is_elevated() ? CheckState::Pass : CheckState::Fail, type, message,
                is_elevated() ? L"elevated" : L"not elevated");
        }
        else if (type == "no_reboot_pending")
        {
            const bool pending = reboot_pending();
            // A warning by default: a pending reboot rarely stops an install
            // working, and blocking on it strands people who cannot restart.
            const bool blocking = config.get_bool(prefix + "blocking", false);
            add(pending ? (blocking ? CheckState::Fail : CheckState::Warn) : CheckState::Pass,
                type, message, pending ? L"a restart is pending" : L"no restart pending");
        }
        else if (type == "registry_value")
        {
            const HKEY root = hive_from_name(config.get(prefix + "hive", "HKLM"));
            const std::wstring key = to_wide(config.get(prefix + "key"));
            const std::wstring value = to_wide(config.get(prefix + "name"));
            const bool present = value.empty() ? registry_key_exists(root, key)
                                               : registry_value_exists(root, key, value);
            const bool want = config.get_bool(prefix + "expect", true);
            add(present == want ? CheckState::Pass : CheckState::Fail, type, message,
                present ? L"present" : L"absent");
        }
        else if (type == "file_exists")
        {
            const std::wstring path = to_wide(config.get(prefix + "path"));
            const bool present =
                !path.empty() && GetFileAttributesW(long_path(path).c_str()) != INVALID_FILE_ATTRIBUTES;
            const bool want = config.get_bool(prefix + "expect", true);
            add(present == want ? CheckState::Pass : CheckState::Fail, type, message,
                present ? L"present" : L"absent");
        }
        else
        {
            // An unknown check is a config error, and failing closed is the
            // only safe reading: the author asked for a guarantee this build
            // cannot make.
            add(CheckState::Fail, type,
                L"This installer requires a newer version of the installer framework.",
                L"unknown preflight type: " + to_wide(type));
        }
    }

    // Built in, not config driven: refusing to replace a newer build with an
    // older one is behaviour every installer should have and none should have
    // to remember to ask for.
    const std::wstring prior = installed_version(plan);
    if (!prior.empty())
    {
        const int order = compare_versions(to_utf8(plan.version), to_utf8(prior));
        if (order < 0)
        {
            add(CheckState::Fail, "downgrade",
                L"A newer version is already installed. Uninstall it first.",
                prior + L" is installed, this is " + plan.version);
        }
        else if (order == 0)
        {
            add(CheckState::Warn, "reinstall", L"This version is already installed.",
                L"reinstalling " + prior);
        }
        else
        {
            add(CheckState::Pass, "upgrade", L"", prior + L" will be upgraded to " + plan.version);
        }
    }

    return outcomes;
}

bool has_blocking_failure(const std::vector<CheckOutcome>& outcomes)
{
    return std::any_of(outcomes.begin(), outcomes.end(),
                       [](const CheckOutcome& o) { return o.state == CheckState::Fail; });
}

std::string format_outcomes(const std::vector<CheckOutcome>& outcomes)
{
    std::string out;
    for (const CheckOutcome& outcome : outcomes)
    {
        const char* label = outcome.state == CheckState::Pass   ? "pass"
                            : outcome.state == CheckState::Warn ? "warn"
                                                                : "FAIL";
        out += "  [";
        out += label;
        out += "] ";
        out += outcome.type;
        if (!outcome.detail.empty())
        {
            out += ": " + to_utf8(outcome.detail);
        }
        if (outcome.state != CheckState::Pass && !outcome.message.empty())
        {
            out += "\n         " + to_utf8(outcome.message);
        }
        out += "\n";
    }
    return out;
}

} // namespace lwi::stub
