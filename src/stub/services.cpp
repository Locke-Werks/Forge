#include "services.h"

#include <windows.h>

#include <shlobj.h>
#include <winsvc.h>

#include "actions.h"
#include "lwi/win_file.h"

namespace lwi::stub
{
namespace
{

constexpr DWORD kStopTimeoutMs = 30000;

std::wstring expand_tokens(const std::wstring& text, const InstallPlan& plan)
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

DWORD start_type_from_name(std::string_view name)
{
    if (name == "auto")
    {
        return SERVICE_AUTO_START;
    }
    if (name == "disabled")
    {
        return SERVICE_DISABLED;
    }
    return SERVICE_DEMAND_START;
}

/// Stops a service and waits for it to actually stop.
///
/// ControlService returning success only means the stop was accepted. Deleting
/// while it is still running marks it for deletion and leaves it in the
/// database until every handle closes, which is how a reinstall hits
/// ERROR_SERVICE_MARKED_FOR_DELETE and fails for no visible reason.
void stop_and_wait(SC_HANDLE service)
{
    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status))
    {
        return;
    }

    const ULONGLONG deadline = GetTickCount64() + kStopTimeoutMs;
    while (status.dwCurrentState != SERVICE_STOPPED && GetTickCount64() < deadline)
    {
        Sleep(200);
        DWORD needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
        {
            break;
        }
    }
}

std::wstring quote_if_needed(const std::wstring& path)
{
    // An unquoted service path containing a space is the unquoted service path
    // vulnerability: Windows tries "C:\Program.exe" before
    // "C:\Program Files\...". Quoting is not cosmetic.
    if (path.empty() || path.front() == L'"')
    {
        return path;
    }
    return L"\"" + path + L"\"";
}

Status write_string(HKEY root, const std::wstring& subkey, const wchar_t* name,
                    const std::wstring& value)
{
    HKEY key = nullptr;
    const LSTATUS rc = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                       KEY_WRITE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS)
    {
        return Status::error(Code::IoError,
                             win32_message("RegCreateKeyExW", static_cast<DWORD>(rc)));
    }
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return Status::ok();
}

void delete_key_tree(HKEY root, const std::wstring& subkey)
{
    RegDeleteTreeW(root, subkey.c_str());
}

} // namespace

Status apply_services(const Config& config, const InstallPlan& plan, InstallRecord& record,
                      std::vector<std::string>* warnings)
{
    const size_t count = config.array_size("services");
    if (count == 0)
    {
        return Status::ok();
    }

    if (plan.scope != Scope::Machine)
    {
        if (warnings != nullptr)
        {
            warnings->push_back("services declared but the install is per-user; skipped");
        }
        return Status::ok();
    }

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr)
    {
        return Status::error(Code::IoError, win32_message("OpenSCManagerW", GetLastError()));
    }

    size_t undo_index = static_cast<size_t>(record.undo.get_int("count", 0));

    for (size_t i = 0; i < count; ++i)
    {
        const std::string prefix = "services." + std::to_string(i) + ".";
        if (!when_satisfied(config, prefix, plan))
        {
            continue;
        }
        const std::wstring name = to_wide(config.get(prefix + "name"));
        const std::wstring binary = expand_tokens(to_wide(config.get(prefix + "binary")), plan);
        if (name.empty() || binary.empty())
        {
            continue;
        }

        const std::wstring display =
            to_wide(config.get(prefix + "display_name", config.get(prefix + "name")));
        const std::wstring description = to_wide(config.get(prefix + "description"));
        const std::wstring account = to_wide(config.get(prefix + "account"));
        const DWORD start_type = start_type_from_name(config.get(prefix + "start", "demand"));

        std::wstring command = quote_if_needed(binary);
        const size_t arg_count = config.array_size(prefix + "args");
        for (size_t a = 0; a < arg_count; ++a)
        {
            command += L" " + expand_tokens(
                                  to_wide(config.get(prefix + "args." + std::to_string(a))), plan);
        }

        SC_HANDLE service = CreateServiceW(
            manager, name.c_str(), display.c_str(), SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS, start_type, SERVICE_ERROR_NORMAL, command.c_str(), nullptr,
            nullptr, nullptr, account.empty() ? nullptr : account.c_str(), nullptr);

        bool created = service != nullptr;
        if (service == nullptr && GetLastError() == ERROR_SERVICE_EXISTS)
        {
            // An upgrade over an existing install. Reconfigure rather than
            // failing, and do not record it for deletion: it was not ours.
            service = OpenServiceW(manager, name.c_str(), SERVICE_ALL_ACCESS);
            if (service != nullptr)
            {
                stop_and_wait(service);
                ChangeServiceConfigW(service, SERVICE_NO_CHANGE, start_type, SERVICE_NO_CHANGE,
                                     command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr,
                                     display.c_str());
            }
        }

        if (service == nullptr)
        {
            if (warnings != nullptr)
            {
                warnings->push_back("service " + to_utf8(name) + ": " +
                                    win32_message("CreateServiceW", GetLastError()));
            }
            continue;
        }

        if (!description.empty())
        {
            SERVICE_DESCRIPTIONW info{};
            info.lpDescription = const_cast<LPWSTR>(description.c_str());
            ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &info);
        }

        if (created)
        {
            const std::string u = std::to_string(undo_index++) + ".";
            record.undo.set(u + "kind", "service");
            record.undo.set(u + "name", to_utf8(name));
        }

        if (config.get_bool(prefix + "start_now", false) && start_type != SERVICE_DISABLED)
        {
            if (!StartServiceW(service, 0, nullptr) &&
                GetLastError() != ERROR_SERVICE_ALREADY_RUNNING && warnings != nullptr)
            {
                warnings->push_back("service " + to_utf8(name) + ": " +
                                    win32_message("StartServiceW", GetLastError()));
            }
        }

        CloseServiceHandle(service);
    }

    record.undo.set("count", std::to_string(undo_index));
    CloseServiceHandle(manager);
    return Status::ok();
}

void revert_services(const InstallRecord& record)
{
    const int64_t count = record.undo.get_int("count", 0);

    SC_HANDLE manager = nullptr;
    for (int64_t i = count - 1; i >= 0; --i)
    {
        const std::string u = std::to_string(i) + ".";
        if (record.undo.get(u + "kind") != "service")
        {
            continue;
        }

        if (manager == nullptr)
        {
            manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
            if (manager == nullptr)
            {
                return;
            }
        }

        const std::wstring name = to_wide(record.undo.get(u + "name"));
        SC_HANDLE service = OpenServiceW(manager, name.c_str(), SERVICE_ALL_ACCESS);
        if (service == nullptr)
        {
            continue;
        }

        stop_and_wait(service);
        DeleteService(service);
        CloseServiceHandle(service);
    }

    if (manager != nullptr)
    {
        CloseServiceHandle(manager);
    }
}

Status apply_associations(const Config& config, const InstallPlan& plan, InstallRecord& record,
                          std::vector<std::string>* warnings)
{
    const size_t count = config.array_size("assoc");
    if (count == 0)
    {
        return Status::ok();
    }

    // HKCR is a merged view, never written directly. Machine scope writes
    // HKLM\Software\Classes, per-user writes HKCU\Software\Classes, and both
    // are Shared on Windows 7 and later so no WOW64 view is forced.
    const HKEY root = plan.scope == Scope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const std::wstring classes = L"SOFTWARE\\Classes\\";

    size_t undo_index = static_cast<size_t>(record.undo.get_int("count", 0));
    bool changed = false;

    for (size_t i = 0; i < count; ++i)
    {
        const std::string prefix = "assoc." + std::to_string(i) + ".";
        if (!when_satisfied(config, prefix, plan))
        {
            continue;
        }
        const std::wstring progid = to_wide(config.get(prefix + "progid"));
        const std::wstring command =
            expand_tokens(to_wide(config.get(prefix + "open_command")), plan);
        if (progid.empty() || command.empty())
        {
            continue;
        }

        const std::wstring friendly =
            to_wide(config.get(prefix + "friendly", config.get(prefix + "progid")));

        if (Status s = write_string(root, classes + progid, nullptr, friendly); !s)
        {
            if (warnings != nullptr)
            {
                warnings->push_back("assoc " + to_utf8(progid) + ": " + s.message());
            }
            continue;
        }
        write_string(root, classes + progid + L"\\shell\\open\\command", nullptr, command);

        if (const std::wstring icon = expand_tokens(to_wide(config.get(prefix + "icon")), plan);
            !icon.empty())
        {
            write_string(root, classes + progid + L"\\DefaultIcon", nullptr, icon);
        }

        {
            const std::string u = std::to_string(undo_index++) + ".";
            record.undo.set(u + "kind", "regtree");
            record.undo.set(u + "hive", plan.scope == Scope::Machine ? "HKLM" : "HKCU");
            record.undo.set(u + "key", to_utf8(classes + progid));
        }

        // The extension points at the ProgID through OpenWithProgids, which is
        // additive. Writing the extension's default value would seize the
        // association from whatever already owns it, and Windows would override
        // that on next launch anyway.
        if (const std::wstring extension = to_wide(config.get(prefix + "extension"));
            !extension.empty())
        {
            HKEY key = nullptr;
            const std::wstring path = classes + extension + L"\\OpenWithProgids";

            // Captured BEFORE the key is created, or there is nothing left to
            // compare against: once RegCreateKeyEx has run, every level of the
            // path exists and the deepest pre-existing ancestor is the leaf.
            const std::wstring ancestor =
                registry_existing_ancestor(plan.scope == Scope::Machine, path);

            if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
                                nullptr, &key, nullptr) == ERROR_SUCCESS)
            {
                RegSetValueExW(key, progid.c_str(), 0, REG_NONE, nullptr, 0);
                RegCloseKey(key);

                const std::string u = std::to_string(undo_index++) + ".";
                record.undo.set(u + "kind", "regvalue");
                record.undo.set(u + "hive", plan.scope == Scope::Machine ? "HKLM" : "HKCU");
                record.undo.set(u + "key", to_utf8(path));
                record.undo.set(u + "name", to_utf8(progid));
                record.undo.set(u + "ancestor", to_utf8(ancestor));
            }
            changed = true;
        }

        // Capabilities, so the product appears in Settings > Default apps.
        //
        // ApplicationDescription is REQUIRED: without it Windows leaves the
        // application out of the defaults UI entirely. Registration is all an
        // installer can do. Windows has blocked programmatic default-handler
        // changes since the UserChoice hash, enforced by UCPD.sys since 2024,
        // so nothing here tries to seize a default.
        const std::wstring app_name = to_wide(config.get(prefix + "app_name", "")) ;
        const std::wstring app_description = to_wide(config.get(prefix + "app_description"));
        if (!app_name.empty() && !app_description.empty())
        {
            const std::wstring caps = L"SOFTWARE\\" + plan.publisher + L"\\" + plan.product +
                                      L"\\Capabilities";
            write_string(root, caps, L"ApplicationName", app_name);
            write_string(root, caps, L"ApplicationDescription", app_description);

            if (const std::wstring extension = to_wide(config.get(prefix + "extension"));
                !extension.empty())
            {
                write_string(root, caps + L"\\FileAssociations", extension.c_str(), progid);
            }

            HKEY key = nullptr;
            if (RegCreateKeyExW(root, L"SOFTWARE\\RegisteredApplications", 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                                nullptr) == ERROR_SUCCESS)
            {
                const std::wstring value = caps;
                RegSetValueExW(key, plan.product.c_str(), 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(value.c_str()),
                               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(key);

                const std::string u = std::to_string(undo_index++) + ".";
                record.undo.set(u + "kind", "regvalue");
                record.undo.set(u + "hive", plan.scope == Scope::Machine ? "HKLM" : "HKCU");
                record.undo.set(u + "key", "SOFTWARE\\RegisteredApplications");
                record.undo.set(u + "name", to_utf8(plan.product));
            }
            changed = true;
        }
    }

    record.undo.set("count", std::to_string(undo_index));

    if (changed)
    {
        // SHCNF_IDLIST is what the reference mandates for SHCNE_ASSOCCHANGED,
        // not SHCNF_DWORD. Its value is 0 and both arguments are null, so the
        // two behave identically here, but the reference is the contract.
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    }
    return Status::ok();
}

void revert_associations(const InstallRecord& record)
{
    const int64_t count = record.undo.get_int("count", 0);
    bool changed = false;

    for (int64_t i = count - 1; i >= 0; --i)
    {
        const std::string u = std::to_string(i) + ".";
        const std::string kind = std::string(record.undo.get(u + "kind"));
        if (kind != "regtree" && kind != "regvalue")
        {
            continue;
        }

        const HKEY root = record.undo.get(u + "hive") == "HKLM" ? HKEY_LOCAL_MACHINE
                                                                : HKEY_CURRENT_USER;
        const std::wstring key = to_wide(record.undo.get(u + "key"));

        if (kind == "regtree")
        {
            delete_key_tree(root, key);
        }
        else
        {
            HKEY handle = nullptr;
            if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE, &handle) == ERROR_SUCCESS)
            {
                RegDeleteValueW(handle, to_wide(record.undo.get(u + "name")).c_str());
                RegCloseKey(handle);
            }

            // Removing the value is not enough: the .ext and OpenWithProgids
            // keys were created by this install too, and leaving them behind
            // puts an empty extension in the registry forever.
            const std::wstring ancestor = to_wide(record.undo.get(u + "ancestor"));
            if (!ancestor.empty() || record.undo.has(u + "ancestor"))
            {
                registry_prune_created(root == HKEY_LOCAL_MACHINE, key, ancestor);
            }
        }
        changed = true;
    }

    if (changed)
    {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    }
}

} // namespace lwi::stub
