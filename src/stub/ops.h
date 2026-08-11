#pragma once

#include <functional>
#include <string>
#include <vector>

#include "lwi/config.h"
#include "lwi/container.h"
#include "lwi/error.h"

namespace lwi::stub
{

/// Where an install lives. Chosen once and then carried, so no code path has to
/// re-derive it and get a different answer.
enum class Scope
{
    Machine,
    User,
};

/// One thing the person installing can say yes or no to.
///
/// Declared in the config, defaulted there, overridable on the command line and
/// then on the options page. Whatever it ends up as travels on the plan, so
/// actions, services, associations and hooks all read the same answer rather
/// than each deciding for themselves what "selected" meant.
struct InstallOption
{
    std::string id;
    std::wstring label;
    std::wstring detail;
    bool selected = false;
};

struct InstallPlan
{
    Scope scope = Scope::Machine;
    std::wstring install_dir;
    std::wstring product;
    std::wstring version;
    std::wstring publisher;
    std::wstring upgrade_code;
    std::wstring aumid;
    std::wstring url_about;
    std::vector<InstallOption> options;
};

InstallPlan plan_from_config(const Config& config, const std::wstring& install_dir_override);

/// True when an option was declared and is selected.
///
/// An id that was never declared reads as not selected. lwforge rejects a `when`
/// naming an undeclared option at build time, so reaching this at runtime means
/// the config was not built by lwforge, and doing less is the safe reading of a
/// condition nobody can evaluate.
[[nodiscard]] bool option_selected(const InstallPlan& plan, std::string_view id);

/// Evaluates the `when` key at "<prefix>when", which gates a declaration on the
/// options that were chosen.
///
/// The grammar is a comma separated list of option ids, each optionally prefixed
/// with '!', all of which must hold. Absent or empty means yes. It is
/// deliberately not an expression language: the stub runs elevated, and a parser
/// is a parser.
[[nodiscard]] bool when_satisfied(const Config& config, const std::string& prefix,
                                  const InstallPlan& plan);

/// Everything an install created, recorded as it happens.
///
/// Uninstall removes exactly what is listed here rather than re-deriving it
/// from the config. Re-deriving is how uninstallers leave files behind when a
/// later version changes a path, and how they delete files they never owned.
struct InstallRecord
{
    std::vector<std::wstring> files;      // relative to install_dir
    std::vector<std::wstring> directories; // relative, deepest first
    std::vector<std::wstring> shortcuts;  // absolute
    std::wstring arp_key;                 // subkey under the Uninstall path
    Scope scope = Scope::Machine;

    /// The uninstall hooks, copied out of the install config.
    ///
    /// They have to travel here because the uninstaller is a payload-free copy
    /// of the stub and has no container to read. That is a real reduction in
    /// guarantee and is worth stating plainly: install hooks are pinned by a
    /// config inside the Authenticode-covered region, whereas uninstall hooks
    /// are pinned by a file in the install directory, protected by that
    /// directory's ACL rather than by the signature. For a per-machine install
    /// under Program Files that means administrator rights; for a per-user
    /// install it means the user's own account, which is the same account the
    /// hook would run as anyway.
    Config hooks;

    /// How to reverse everything that is not a file: registry values and
    /// environment entries, each recorded with what was there before.
    ///
    /// Restoring a captured prior value is the only correct undo. Deleting
    /// unconditionally destroys a setting the machine already had, and leaving
    /// it alone orphans one the install created.
    Config undo;

    /// The options as they were answered, so uninstall gates on the same
    /// answers. Without this an uninstall hook guarded by `when` would re-read
    /// the defaults and run cleanup for something the user never chose, or skip
    /// cleanup for something they did.
    std::vector<InstallOption> options;

    Status save(const std::wstring& install_dir) const;
    Status load(const std::wstring& install_dir);
};

/// Subdirectory holding the uninstaller and the manifest. Leading dot so it
/// sorts out of the way and reads as machinery rather than as product content.
inline constexpr const wchar_t* kMetaDir = L".lw";
inline constexpr const wchar_t* kUninstallerName = L"uninstall.exe";

/// The payload path lwforge stores the uninstaller at. The stub extracts it
/// like any other member, which is what makes the uninstaller a signed binary
/// rather than a copy the installer stamps out at runtime.
inline constexpr const char* kUninstallerPayloadPath = ".lw\\uninstall.exe";

using ProgressFn = std::function<bool(float fraction, const std::wstring& status)>;

/// Deliberate failure injection, for testing rollback.
///
/// Rollback only runs when something has already gone wrong, on a machine
/// nobody is watching, and several of its failure modes look identical to
/// success. It cannot be tested by hoping an install fails, so the engine
/// exposes a way to make it fail on purpose at a numbered step.
///
/// LWI_FAULT_INJECT=<n> returns an error after step n, exercising the ordinary
/// unwind. LWI_FAULT_KILL=<n> terminates the process instead, leaving an
/// uncommitted journal so the NEXT run has to recover it. Both are inert unless
/// the variable is set, and both are compiled in deliberately: a rollback path
/// that only exists in a test build is a rollback path that ships untested.
Status fault_check(uint32_t step);

/// Deletes a directory and everything under it. Missing is success.
void remove_directory_tree(const std::wstring& path);

/// Creates a directory and any missing parents.
Status ensure_directory_exists(const std::wstring& path);

/// Runs an install.
///
/// warnings collects anything that did not stop the install but that the
/// operator should know about, a failed Start Menu shortcut being the usual
/// case. Swallowing those silently is how an installer reports success and
/// leaves the user unable to find the thing it installed.
Status run_install(const ContainerReader& reader, const Config& config, const InstallPlan& plan,
                   const ProgressFn& progress, std::vector<std::string>* warnings = nullptr);

Status run_uninstall(const std::wstring& install_dir);

/// Detects an already-installed version. Returns an empty string when absent.
std::wstring installed_version(const InstallPlan& plan);

} // namespace lwi::stub
