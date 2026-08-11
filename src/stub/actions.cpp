#include "actions.h"

#include <windows.h>

#include <algorithm>

#include "lwi/win_file.h"

namespace lwi::stub
{
namespace
{

constexpr const wchar_t* kMachineEnvKey =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
constexpr const wchar_t* kUserEnvKey = L"Environment";

HKEY hive_from_name(std::string_view name, Scope scope)
{
    if (name == "HKCU" || name == "HKEY_CURRENT_USER")
    {
        return HKEY_CURRENT_USER;
    }
    if (name == "HKLM" || name == "HKEY_LOCAL_MACHINE")
    {
        return HKEY_LOCAL_MACHINE;
    }
    // Unspecified follows the install scope, which is almost always what an
    // author means and avoids a per-user install writing machine-wide state.
    return scope == Scope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

const char* hive_name(HKEY hive)
{
    return hive == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU";
}

/// Reads a value without expanding it.
///
/// RegQueryValueEx never expands REG_EXPAND_SZ. RegGetValue does by default and
/// needs RRF_NOEXPAND to stop. Expanding on read and writing the result back is
/// how "%SystemRoot%\system32" becomes a literal path baked for one machine.
bool read_value(HKEY hive, const std::wstring& subkey, const std::wstring& name,
                std::wstring& value, DWORD& type)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD size = 0;
    LSTATUS rc = RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return false;
    }

    std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
    rc = RegQueryValueExW(key, name.c_str(), nullptr, &type,
                          reinterpret_cast<BYTE*>(buffer.data()), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS)
    {
        return false;
    }

    buffer.resize(size / sizeof(wchar_t));
    while (!buffer.empty() && buffer.back() == L'\0')
    {
        buffer.pop_back();
    }
    value = buffer;
    return true;
}

Status write_value(HKEY hive, const std::wstring& subkey, const std::wstring& name,
                   const std::wstring& value, DWORD type)
{
    HKEY key = nullptr;
    const LSTATUS rc = RegCreateKeyExW(hive, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                       KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS)
    {
        return Status::error(Code::IoError,
                             win32_message("RegCreateKeyExW", static_cast<DWORD>(rc)));
    }

    LSTATUS set = ERROR_SUCCESS;
    if (type == REG_DWORD)
    {
        const DWORD number = static_cast<DWORD>(_wtoi64(value.c_str()));
        set = RegSetValueExW(key, name.c_str(), 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&number), sizeof(number));
    }
    else
    {
        set = RegSetValueExW(key, name.c_str(), 0, type,
                             reinterpret_cast<const BYTE*>(value.c_str()),
                             static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(key);
    if (set != ERROR_SUCCESS)
    {
        return Status::error(Code::IoError,
                             win32_message("RegSetValueExW", static_cast<DWORD>(set)));
    }
    return Status::ok();
}

void delete_value(HKEY hive, const std::wstring& subkey, const std::wstring& name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive, subkey.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) ==
        ERROR_SUCCESS)
    {
        RegDeleteValueW(key, name.c_str());
        RegCloseKey(key);
    }
}

bool key_exists(HKEY hive, const std::wstring& subkey)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    RegCloseKey(key);
    return true;
}

bool key_is_empty(HKEY hive, const std::wstring& subkey)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    DWORD subkeys = 0;
    DWORD values = 0;
    const LSTATUS rc = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                                        &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && subkeys == 0 && values == 0;
}

/// The deepest ancestor of subkey that already exists.
///
/// Recorded before writing so uninstall knows exactly how much of the key path
/// this install brought into being. Without it the choice is between leaving
/// empty keys behind forever and deleting keys the machine already had.
std::wstring existing_ancestor(HKEY hive, const std::wstring& subkey)
{
    std::wstring path = subkey;
    while (!path.empty())
    {
        if (key_exists(hive, path))
        {
            return path;
        }
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos)
        {
            return {};
        }
        path = path.substr(0, slash);
    }
    return {};
}

/// Removes keys this install created, leaf first, stopping at the ancestor that
/// was already there. Each is deleted only when empty, so a key that picked up
/// unrelated content in the meantime survives.
void prune_created_keys(HKEY hive, const std::wstring& subkey, const std::wstring& stop_at)
{
    std::wstring path = subkey;
    while (!path.empty() && path.size() > stop_at.size())
    {
        if (!key_is_empty(hive, path))
        {
            return;
        }
        if (RegDeleteKeyExW(hive, path.c_str(), KEY_WOW64_64KEY, 0) != ERROR_SUCCESS)
        {
            return;
        }
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos)
        {
            return;
        }
        path = path.substr(0, slash);
    }
}

DWORD type_from_name(std::string_view name)
{
    if (name == "dword")
    {
        return REG_DWORD;
    }
    if (name == "expand_sz")
    {
        return REG_EXPAND_SZ;
    }
    return REG_SZ;
}

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

/// Splits a PATH-style value, dropping empty segments.
std::vector<std::wstring> split_path(const std::wstring& value)
{
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= value.size())
    {
        const size_t end = value.find(L';', start);
        const std::wstring piece =
            value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!piece.empty())
        {
            out.push_back(piece);
        }
        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }
    return out;
}

bool same_path(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

} // namespace

std::wstring registry_existing_ancestor(bool machine, const std::wstring& subkey)
{
    return existing_ancestor(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, subkey);
}

void registry_prune_created(bool machine, const std::wstring& subkey, const std::wstring& stop_at)
{
    prune_created_keys(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, subkey, stop_at);
}

void broadcast_environment_change()
{
    // SendMessageTimeout takes SEVEN parameters. The trailing lpdwResult is
    // optional in the documentation's sense, not in C's: it still has to be
    // passed. A five-second timeout keeps one wedged top-level window from
    // hanging the installer.
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000,
                        &result);
}

Status apply_actions(const Config& config, const InstallPlan& plan, InstallRecord& record,
                     std::vector<std::string>* warnings)
{
    const size_t count = config.array_size("actions");
    size_t undo_index = static_cast<size_t>(record.undo.get_int("count", 0));
    bool environment_changed = false;

    for (size_t i = 0; i < count; ++i)
    {
        const std::string prefix = "actions." + std::to_string(i) + ".";
        if (!when_satisfied(config, prefix, plan))
        {
            continue;
        }
        const std::string type = std::string(config.get(prefix + "type"));

        if (type == "registry_write")
        {
            const HKEY hive = hive_from_name(config.get(prefix + "hive"), plan.scope);
            const std::wstring key = expand_tokens(to_wide(config.get(prefix + "key")), plan);
            const std::wstring name = to_wide(config.get(prefix + "name"));
            const std::wstring value = expand_tokens(to_wide(config.get(prefix + "value")), plan);
            const DWORD value_type = type_from_name(config.get(prefix + "value_type", "sz"));

            if (key.empty())
            {
                continue;
            }

            std::wstring prior;
            DWORD prior_type = REG_SZ;
            const bool existed = read_value(hive, key, name, prior, prior_type);
            const std::wstring ancestor = existing_ancestor(hive, key);

            if (Status s = write_value(hive, key, name, value, value_type); !s)
            {
                if (warnings != nullptr)
                {
                    warnings->push_back("registry " + to_utf8(key) + ": " + s.message());
                }
                continue;
            }

            const std::string u = std::to_string(undo_index++) + ".";
            record.undo.set(u + "kind", "registry");
            record.undo.set(u + "hive", hive_name(hive));
            record.undo.set(u + "key", to_utf8(key));
            record.undo.set(u + "name", to_utf8(name));
            record.undo.set(u + "existed", existed ? "true" : "false");
            record.undo.set(u + "prior", to_utf8(prior));
            record.undo.set(u + "prior_type", std::to_string(prior_type));
            record.undo.set(u + "ancestor", to_utf8(ancestor));
        }
        else if (type == "env" || type == "path_append")
        {
            const bool machine = plan.scope == Scope::Machine;
            const HKEY hive = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
            const std::wstring key = machine ? kMachineEnvKey : kUserEnvKey;

            const std::wstring name =
                type == "path_append" ? L"Path" : to_wide(config.get(prefix + "name"));
            const std::wstring value = expand_tokens(to_wide(config.get(prefix + "value")), plan);
            if (name.empty() || value.empty())
            {
                continue;
            }

            std::wstring prior;
            DWORD prior_type = REG_SZ;
            const bool existed = read_value(hive, key, name, prior, prior_type);

            std::wstring next = value;
            DWORD next_type = type_from_name(config.get(prefix + "value_type", "expand_sz"));

            if (type == "path_append")
            {
                // Preserve the existing type. PATH is normally REG_EXPAND_SZ,
                // and rewriting it as REG_SZ silently freezes every %VAR% the
                // machine already had in it.
                next_type = existed ? prior_type : REG_EXPAND_SZ;

                std::vector<std::wstring> parts = split_path(prior);
                const bool already = std::any_of(
                    parts.begin(), parts.end(),
                    [&](const std::wstring& part) { return same_path(part, value); });
                if (already)
                {
                    continue;
                }
                parts.push_back(value);

                next.clear();
                for (size_t p = 0; p < parts.size(); ++p)
                {
                    if (p != 0)
                    {
                        next += L';';
                    }
                    next += parts[p];
                }
            }

            if (Status s = write_value(hive, key, name, next, next_type); !s)
            {
                if (warnings != nullptr)
                {
                    warnings->push_back("environment " + to_utf8(name) + ": " + s.message());
                }
                continue;
            }

            environment_changed = true;

            const std::string u = std::to_string(undo_index++) + ".";
            record.undo.set(u + "kind", "registry");
            record.undo.set(u + "hive", hive_name(hive));
            record.undo.set(u + "key", to_utf8(key));
            record.undo.set(u + "name", to_utf8(name));
            record.undo.set(u + "existed", existed ? "true" : "false");
            record.undo.set(u + "prior", to_utf8(prior));
            record.undo.set(u + "prior_type", std::to_string(prior_type));
        }
    }

    record.undo.set("count", std::to_string(undo_index));

    if (environment_changed)
    {
        broadcast_environment_change();
    }
    return Status::ok();
}

void revert_actions(const InstallRecord& record)
{
    const int64_t count = record.undo.get_int("count", 0);
    bool environment_changed = false;

    // Reverse order, so a value written twice ends up back at its original
    // rather than at whatever the first write captured.
    for (int64_t i = count - 1; i >= 0; --i)
    {
        const std::string u = std::to_string(i) + ".";
        if (record.undo.get(u + "kind") != "registry")
        {
            continue;
        }

        const std::string hive_text = std::string(record.undo.get(u + "hive"));
        const HKEY hive = hive_text == "HKLM" ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        const std::wstring key = to_wide(record.undo.get(u + "key"));
        const std::wstring name = to_wide(record.undo.get(u + "name"));

        if (record.undo.get_bool(u + "existed"))
        {
            const std::wstring prior = to_wide(record.undo.get(u + "prior"));
            const DWORD type =
                static_cast<DWORD>(record.undo.get_int(u + "prior_type", REG_SZ));
            write_value(hive, key, name, prior, type);
        }
        else
        {
            delete_value(hive, key, name);

            // Only prune below the ancestor that predated this install. The
            // environment keys always predate it, so their recorded ancestor
            // equals the key itself and nothing is pruned.
            const std::wstring ancestor = to_wide(record.undo.get(u + "ancestor"));
            prune_created_keys(hive, key, ancestor);
        }

        if (key == kMachineEnvKey || key == kUserEnvKey)
        {
            environment_changed = true;
        }
    }

    if (environment_changed)
    {
        broadcast_environment_change();
    }
}

} // namespace lwi::stub
