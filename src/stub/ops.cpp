#include "ops.h"

#include <windows.h>

#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>

#include "actions.h"
#include "journal.h"
#include "services.h"
#include "hooks.h"
#include "lwi/win_file.h"

namespace lwi::stub
{
namespace
{

constexpr const wchar_t* kUninstallPath =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";

constexpr const wchar_t* kManifestName = L"install.manifest";

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

HKEY scope_root(Scope scope)
{
    return scope == Scope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

/// KEY_WOW64_64KEY is passed deliberately and only here.
///
/// The Uninstall key inherits WOW64 redirection from HKLM\SOFTWARE, so a
/// 32-bit view would put the entry somewhere Settings does not look. Most of
/// the other keys an installer touches (App Paths, SOFTWARE\Classes,
/// RegisteredApplications) are Shared on Windows 7 and later, where forcing a
/// view invites the opposite error of assuming two views exist.
Status reg_create(HKEY root, const std::wstring& subkey, HKEY& out)
{
    const LSTATUS rc = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                       KEY_WRITE | KEY_WOW64_64KEY, nullptr, &out, nullptr);
    if (rc != ERROR_SUCCESS)
    {
        return Status::error(Code::IoError,
                             win32_message("RegCreateKeyExW", static_cast<DWORD>(rc)));
    }
    return Status::ok();
}

void reg_set_string(HKEY key, const wchar_t* name, const std::wstring& value)
{
    if (value.empty())
    {
        return;
    }
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void reg_set_dword(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

std::wstring today_stamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16]{};
    swprintf_s(buf, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::wstring quote(const std::wstring& s)
{
    return L"\"" + s + L"\"";
}

Status delete_tree_entry(const std::wstring& path)
{
    const std::wstring full = long_path(path);
    if (DeleteFileW(full.c_str()))
    {
        return Status::ok();
    }
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
    {
        return Status::ok();
    }

    // Read-only files are ours to clear: we wrote them.
    if (err == ERROR_ACCESS_DENIED)
    {
        SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(full.c_str()))
        {
            return Status::ok();
        }
    }

    // Still locked, most often because the file is running or an antivirus
    // scanner holds a handle. Defer rather than fail the whole uninstall.
    MoveFileExW(full.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return Status::ok();
}

} // namespace

Status fault_check(uint32_t step)
{
    const auto read_step = [](const wchar_t* name, uint32_t& out) {
        wchar_t buffer[32]{};
        const DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
        if (n == 0 || n >= std::size(buffer))
        {
            return false;
        }
        out = static_cast<uint32_t>(_wtoi(buffer));
        return true;
    };

    uint32_t target = 0;
    if (read_step(L"LWI_FAULT_KILL", target) && target == step)
    {
        // No unwinding, no destructors, no journal commit. This is what a power
        // loss looks like from the filesystem's point of view.
        TerminateProcess(GetCurrentProcess(), 1);
    }

    if (read_step(L"LWI_FAULT_INJECT", target) && target == step)
    {
        return Status::error(Code::IoError,
                             "deliberate fault injected at step " + std::to_string(step));
    }

    return Status::ok();
}

void remove_directory_tree(const std::wstring& path)
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
                remove_directory_tree(child);
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

Status ensure_directory_exists(const std::wstring& path)
{
    return ensure_directory(path);
}

InstallPlan plan_from_config(const Config& config, const std::wstring& install_dir_override)
{
    InstallPlan plan;
    plan.scope = config.get("install.scope") == "user" ? Scope::User : Scope::Machine;
    plan.product = to_wide(config.get("product.name", "Application"));
    plan.version = to_wide(config.get("product.version"));
    plan.publisher = to_wide(config.get("product.publisher", "Locke Werks"));
    plan.upgrade_code = to_wide(config.get("product.upgrade_code"));
    plan.aumid = to_wide(config.get("product.aumid"));
    plan.url_about = to_wide(config.get("product.url"));
    plan.install_dir = install_dir_override;
    return plan;
}

Status InstallRecord::save(const std::wstring& install_dir) const
{
    // Reuses the config codec rather than inventing a second serialisation.
    // One format means one parser and one set of bounds checks to get right.
    Config manifest;
    manifest.set("scope", scope == Scope::Machine ? "machine" : "user");
    manifest.set("arp_key", to_utf8(arp_key));

    manifest.set("files.count", std::to_string(files.size()));
    for (size_t i = 0; i < files.size(); ++i)
    {
        manifest.set("files." + std::to_string(i), to_utf8(files[i]));
    }
    manifest.set("dirs.count", std::to_string(directories.size()));
    for (size_t i = 0; i < directories.size(); ++i)
    {
        manifest.set("dirs." + std::to_string(i), to_utf8(directories[i]));
    }
    manifest.set("shortcuts.count", std::to_string(shortcuts.size()));
    for (size_t i = 0; i < shortcuts.size(); ++i)
    {
        manifest.set("shortcuts." + std::to_string(i), to_utf8(shortcuts[i]));
    }

    // Uninstall hooks keep their original key names so the loaded manifest can
    // be handed straight to run_hooks without a translation step that could
    // disagree with the installer's own reading of the same config.
    for (const auto& [key, value] : hooks.entries())
    {
        manifest.set(key, value);
    }
    for (const auto& [key, value] : undo.entries())
    {
        manifest.set("undo." + key, value);
    }

    std::vector<uint8_t> blob;
    if (Status s = manifest.encode(blob); !s)
    {
        return s;
    }

    const std::wstring meta = install_dir + L"\\" + kMetaDir;
    if (Status s = ensure_directory(meta); !s)
    {
        return s;
    }
    return write_whole_file(meta + L"\\" + kManifestName, blob);
}

Status InstallRecord::load(const std::wstring& install_dir)
{
    std::vector<uint8_t> blob;
    const std::wstring path = install_dir + L"\\" + kMetaDir + L"\\" + kManifestName;
    if (Status s = read_whole_file(path, blob); !s)
    {
        return Status::error(Code::IoError,
                             "no install manifest at " + to_utf8(path) +
                                 "; refusing to guess what to remove");
    }

    Config manifest;
    if (Status s = manifest.decode(blob); !s)
    {
        return s;
    }

    scope = manifest.get("scope") == "user" ? Scope::User : Scope::Machine;
    arp_key = to_wide(manifest.get("arp_key"));

    const auto read_list = [&](const char* prefix, std::vector<std::wstring>& out) {
        const int64_t count = manifest.get_int(std::string(prefix) + ".count", 0);
        out.clear();
        for (int64_t i = 0; i < count; ++i)
        {
            out.push_back(to_wide(manifest.get(std::string(prefix) + "." + std::to_string(i))));
        }
    };
    read_list("files", files);
    read_list("dirs", directories);
    read_list("shortcuts", shortcuts);

    for (const auto& [key, value] : manifest.entries())
    {
        if (key.rfind("hooks.", 0) == 0)
        {
            hooks.set(key, value);
        }
        else if (key.rfind("undo.", 0) == 0)
        {
            undo.set(key.substr(5), value);
        }
    }

    return Status::ok();
}

namespace
{

Status write_arp(const InstallPlan& plan, uint64_t size_bytes, InstallRecord& record)
{
    // Keyed by product name rather than by a GUID. Settings shows either, and a
    // readable key is one a support engineer can find without a lookup table.
    record.arp_key = plan.product;

    HKEY key = nullptr;
    if (Status s = reg_create(scope_root(plan.scope), kUninstallPath + record.arp_key, key); !s)
    {
        return s;
    }

    const std::wstring uninstaller =
        plan.install_dir + L"\\" + kMetaDir + L"\\" + kUninstallerName;

    reg_set_string(key, L"DisplayName", plan.product);
    reg_set_string(key, L"DisplayVersion", plan.version);
    reg_set_string(key, L"Publisher", plan.publisher);
    reg_set_string(key, L"InstallLocation", plan.install_dir);
    reg_set_string(key, L"InstallDate", today_stamp());
    reg_set_string(key, L"DisplayIcon", uninstaller + L",0");
    reg_set_string(key, L"UninstallString", quote(uninstaller) + L" /uninstall");
    // QuietUninstallString is what Intune, SCCM and winget actually invoke.
    // Without it they fall back to UninstallString and get an interactive
    // prompt in a session with no one to answer it.
    reg_set_string(key, L"QuietUninstallString", quote(uninstaller) + L" /uninstall /S");
    reg_set_string(key, L"URLInfoAbout", plan.url_about);

    // EstimatedSize is in KILOBYTES. Written in bytes it reports a terabyte and
    // looks like a bug in the product rather than in its installer.
    reg_set_dword(key, L"EstimatedSize", static_cast<DWORD>(size_bytes / 1024));
    reg_set_dword(key, L"NoModify", 1);
    reg_set_dword(key, L"NoRepair", 1);

    RegCloseKey(key);
    return Status::ok();
}

Status create_shortcut(const std::wstring& link_path, const std::wstring& target,
                       const std::wstring& working_dir, const std::wstring& aumid)
{
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (FAILED(hr))
    {
        return Status::error(Code::IoError, hresult_message("CoCreateInstance(ShellLink)", hr));
    }

    // SetPath must point at a real, well-formed executable when the target is
    // an .exe. Save resolves the target while writing the link, and a file with
    // an .exe extension that is not a valid PE makes it fail outright with
    // E_FAIL rather than degrade. A target that does not exist at all is fine,
    // which is why this failure mode stays hidden until a payload is present.
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(working_dir.c_str());

    // No SetIconLocation. A shell link with no explicit icon already uses its
    // target's first icon, so pointing it at the target is redundant and only
    // adds another way for Save to fail.

    // The AppUserModelID is what makes a taskbar pin and any toast notification
    // survive an upgrade. It must carry no version component for that reason.
    if (!aumid.empty())
    {
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void**>(&store))))
        {
            PROPVARIANT value{};
            if (SUCCEEDED(InitPropVariantFromString(aumid.c_str(), &value)))
            {
                store->SetValue(PKEY_AppUserModel_ID, value);
                store->Commit();
                PropVariantClear(&value);
            }
            store->Release();
        }
    }

    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    if (SUCCEEDED(hr))
    {
        hr = file->Save(link_path.c_str(), TRUE);
        file->Release();
    }
    link->Release();

    if (FAILED(hr))
    {
        return Status::error(Code::IoError, hresult_message("IPersistFile::Save", hr) +
                                                " (target " + to_utf8(target) + ")");
    }
    return Status::ok();
}

} // namespace

std::wstring installed_version(const InstallPlan& plan)
{
    HKEY key = nullptr;
    const std::wstring path = kUninstallPath + plan.product;
    if (RegOpenKeyExW(scope_root(plan.scope), path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS)
    {
        return {};
    }

    wchar_t buffer[128]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::wstring version;
    if (RegQueryValueExW(key, L"DisplayVersion", nullptr, &type,
                         reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS &&
        type == REG_SZ)
    {
        version = buffer;
    }
    RegCloseKey(key);
    return version;
}

Status run_install(const ContainerReader& reader, const Config& config, const InstallPlan& plan,
                   const ProgressFn& progress, std::vector<std::string>* warnings)
{
    InstallRecord record;
    record.scope = plan.scope;

    if (Status s = ensure_directory(plan.install_dir); !s)
    {
        return s;
    }

    // The journal lives in the meta directory, so that has to exist before the
    // first record is written. It used to be created later, by record.save,
    // which meant journal.begin failed instantly on every install and the whole
    // fault matrix passed vacuously.
    if (Status s = ensure_directory(plan.install_dir + L"\\" + kMetaDir); !s)
    {
        return s;
    }

    // An earlier install that crashed leaves an uncommitted journal. Undo it
    // before touching anything, so this install starts from a known state
    // rather than layering onto a half-written one.
    if (const size_t undone = Journal::recover(plan.install_dir); undone != 0 && warnings != nullptr)
    {
        warnings->push_back("rolled back " + std::to_string(undone) +
                            " operations from an interrupted install");
    }

    Journal journal;
    if (Status s = journal.begin(plan.install_dir); !s)
    {
        return s;
    }

    // Every failure past this point unwinds. Declared once here rather than
    // repeated at each return, because the one path that forgets to roll back
    // is the one that leaves a machine broken.
    // One place that knows how to unwind everything, so no failure path can
    // forget a category. Shortcuts were the category it forgot: files rolled
    // back and registry values reverted while a Start Menu entry pointing at a
    // deleted directory survived every failed install.
    const auto abort = [&](Status status) {
        for (const std::wstring& shortcut : record.shortcuts)
        {
            delete_tree_entry(shortcut);
        }
        revert_associations(record);
        revert_services(record);
        revert_actions(record);
        journal.rollback();
        return status;
    };

    const std::vector<FileView>& files = reader.files();
    uint64_t total_bytes = 0;
    uint32_t backup_sequence = 0;
    uint32_t step = 0;

    for (size_t i = 0; i < files.size(); ++i)
    {
        const std::wstring relative = to_wide(files[i].path);
        if (progress && !progress(static_cast<float>(i) / static_cast<float>(files.size()),
                                  relative))
        {
            return abort(Status::error(Code::InvalidArgument, "cancelled"));
        }

        if (Status s = fault_check(step++); !s)
        {
            return abort(s);
        }

        std::vector<uint8_t> contents;
        if (Status s = reader.extract(i, contents); !s)
        {
            return abort(s);
        }

        const std::wstring destination = plan.install_dir + L"\\" + relative;

        const size_t slash = relative.find_last_of(L'\\');
        if (slash != std::wstring::npos)
        {
            const std::wstring relative_dir = relative.substr(0, slash);
            if (GetFileAttributesW(long_path(plan.install_dir + L"\\" + relative_dir).c_str()) ==
                INVALID_FILE_ATTRIBUTES)
            {
                if (Status s = journal.record(JournalOp::DirCreated, relative_dir); !s)
                {
                    return abort(s);
                }
            }
            if (Status s = ensure_directory(plan.install_dir + L"\\" + relative_dir); !s)
            {
                return abort(s);
            }
            if (std::find(record.directories.begin(), record.directories.end(), relative_dir) ==
                record.directories.end())
            {
                record.directories.push_back(relative_dir);
            }
        }

        const bool exists =
            GetFileAttributesW(long_path(destination).c_str()) != INVALID_FILE_ATTRIBUTES;

        if (exists)
        {
            // Rename the original aside rather than overwriting it. This is
            // both the rollback copy and the only way to replace a file that is
            // currently running: Windows permits renaming a mapped image, it
            // only forbids unlinking one. MOVEFILE_REPLACE_EXISTING is
            // deliberately NOT passed, because the destination must not exist.
            const std::wstring backup_relative =
                Journal::backup_path_for(relative, backup_sequence++);
            const std::wstring backup = plan.install_dir + L"\\" + backup_relative;

            const size_t backup_slash = backup.find_last_of(L'\\');
            if (backup_slash != std::wstring::npos)
            {
                if (Status s = ensure_directory(backup.substr(0, backup_slash)); !s)
                {
                    return abort(s);
                }
            }

            if (Status s = journal.record(JournalOp::FileReplaced, relative, backup_relative); !s)
            {
                return abort(s);
            }

            if (!MoveFileExW(long_path(destination).c_str(), long_path(backup).c_str(), 0))
            {
                return abort(Status::error(
                    Code::IoError,
                    win32_message("MoveFileExW (saving the original of " + to_utf8(relative) + ")",
                                  GetLastError())));
            }
        }
        else
        {
            if (Status s = journal.record(JournalOp::FileCreated, relative); !s)
            {
                return abort(s);
            }
        }

        if (Status s = write_whole_file(destination, contents); !s)
        {
            return abort(s);
        }

        record.files.push_back(relative);
        total_bytes += contents.size();
    }

    if (Status s = fault_check(step++); !s)
    {
        return abort(s);
    }

    // Files are on disk. This is the first point a product's own code can run.
    {
        std::vector<HookOutcome> hook_outcomes;
        const bool ok = run_hooks(Phase::PostExtract, config, plan, hook_outcomes);
        for (const HookOutcome& outcome : hook_outcomes)
        {
            if (!outcome.ok && warnings != nullptr)
            {
                warnings->push_back("hook " + outcome.id + ": " + outcome.detail);
            }
        }
        if (!ok)
        {
            return abort(Status::error(Code::IoError, "a required post_extract hook failed"));
        }
    }

    {
        std::vector<HookOutcome> hook_outcomes;
        const bool ok = run_hooks(Phase::PreRegister, config, plan, hook_outcomes);
        for (const HookOutcome& outcome : hook_outcomes)
        {
            if (!outcome.ok && warnings != nullptr)
            {
                warnings->push_back("hook " + outcome.id + ": " + outcome.detail);
            }
        }
        if (!ok)
        {
            return abort(Status::error(Code::IoError, "a required pre_register hook failed"));
        }
    }

    // Shortcuts. Created from the declarative action list, and every one that
    // lands is recorded so uninstall removes exactly these and nothing else.
    const size_t action_count = config.array_size("actions");
    for (size_t i = 0; i < action_count; ++i)
    {
        const std::string prefix = "actions." + std::to_string(i) + ".";
        if (config.get(prefix + "type") != "shortcut")
        {
            continue;
        }

        std::wstring target = to_wide(config.get(prefix + "target"));
        const size_t token = target.find(L"{InstallDir}");
        if (token != std::wstring::npos)
        {
            target.replace(token, wcslen(L"{InstallDir}"), plan.install_dir);
        }

        const std::string where = std::string(config.get(prefix + "where", "common_programs"));
        std::wstring folder;
        if (where == "desktop")
        {
            folder = known_folder(plan.scope == Scope::Machine ? FOLDERID_PublicDesktop
                                                              : FOLDERID_Desktop);
        }
        else
        {
            folder = known_folder(plan.scope == Scope::Machine ? FOLDERID_CommonPrograms
                                                              : FOLDERID_Programs);
        }
        if (folder.empty())
        {
            continue;
        }

        const std::wstring name = to_wide(config.get(prefix + "name", config.get("product.name")));
        const std::wstring link = folder + L"\\" + name + L".lnk";

        // A failed shortcut is not a failed install: the product is on disk and
        // usable. It is still reported, because an install that quietly does
        // not produce the Start Menu entry it was asked for looks to the user
        // like an install that did not happen.
        if (Status s = create_shortcut(link, target, plan.install_dir, plan.aumid); s)
        {
            record.shortcuts.push_back(link);
        }
        else if (warnings != nullptr)
        {
            warnings->push_back("shortcut " + to_utf8(link) + ": " + s.message());
        }
    }

    if (Status s = apply_actions(config, plan, record, warnings); !s)
    {
        return abort(s);
    }

    if (Status s = apply_services(config, plan, record, warnings); !s)
    {
        return abort(s);
    }

    if (Status s = apply_associations(config, plan, record, warnings); !s)
    {
        return abort(s);
    }

    if (Status s = fault_check(step++); !s)
    {
        return abort(s);
    }

    if (Status s = write_arp(plan, total_bytes, record); !s)
    {
        return abort(s);
    }

    for (const auto& [key, value] : config.entries())
    {
        if (key.rfind("hooks.pre_uninstall", 0) == 0 || key.rfind("hooks.post_uninstall", 0) == 0)
        {
            record.hooks.set(key, value);
        }
    }

    if (Status s = record.save(plan.install_dir); !s)
    {
        return abort(s);
    }

    {
        std::vector<HookOutcome> hook_outcomes;
        const bool ok = run_hooks(Phase::PostInstall, config, plan, hook_outcomes);
        for (const HookOutcome& outcome : hook_outcomes)
        {
            if (!outcome.ok && warnings != nullptr)
            {
                warnings->push_back("hook " + outcome.id + ": " + outcome.detail);
            }
        }
        if (!ok)
        {
            return abort(Status::error(Code::IoError, "a required post_install hook failed"));
        }
    }

    // Nothing can unwind past this point: the saved originals are gone and the
    // install is the state of record.
    if (Status s = journal.commit(); !s)
    {
        return s;
    }

    if (progress)
    {
        progress(1.0f, L"Finishing");
    }
    return Status::ok();
}

Status run_uninstall(const std::wstring& install_dir)
{
    InstallRecord record;
    if (Status s = record.load(install_dir); !s)
    {
        return s;
    }

    InstallPlan plan;
    plan.install_dir = install_dir;
    plan.scope = record.scope;

    {
        std::vector<HookOutcome> hook_outcomes;
        // Not fatal. Refusing to uninstall because the product's own cleanup
        // hook failed leaves the user with something they cannot remove, which
        // is worse than removing it with the cleanup half done.
        std::vector<HookOutcome> ignored;
        run_hooks(Phase::PreUninstall, record.hooks, plan, ignored);
    }

    for (const std::wstring& shortcut : record.shortcuts)
    {
        delete_tree_entry(shortcut);
    }

    for (const std::wstring& relative : record.files)
    {
        delete_tree_entry(install_dir + L"\\" + relative);
    }

    // Deepest first, so a parent is only attempted once its children are gone.
    std::vector<std::wstring> dirs = record.directories;
    std::sort(dirs.begin(), dirs.end(),
              [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (const std::wstring& dir : dirs)
    {
        RemoveDirectoryW(long_path(install_dir + L"\\" + dir).c_str());
    }

    revert_associations(record);
    revert_services(record);
    revert_actions(record);

    if (!record.arp_key.empty())
    {
        RegDeleteKeyExW(scope_root(record.scope), (kUninstallPath + record.arp_key).c_str(),
                        KEY_WOW64_64KEY, 0);
    }

    // The manifest, then the meta directory, then the install directory itself.
    // The uninstaller is running from inside the meta directory, so its own
    // image cannot be unlinked yet; that is handled by the caller.
    delete_tree_entry(install_dir + L"\\" + kMetaDir + L"\\" + kManifestName);

    return Status::ok();
}

} // namespace lwi::stub
