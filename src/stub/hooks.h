#pragma once

#include <string>
#include <vector>

#include "lwi/config.h"
#include "lwi/container.h"
#include "ops.h"

namespace lwi::stub
{

/// Named points where a product can run its own code, in the order they run.
///
/// No phase runs with nothing on disk. pre_install is the earliest, and it works
/// by placing its own binary at its final destination before anything else, so
/// the hook still executes from the install directory that every other phase
/// executes from. The original objection was to a world-writable temp
/// directory, and it still stands: what changes here is the moment, not the
/// location or its ACL.
///
/// pre_install carries one constraint no other phase does. The rest of the
/// payload is not there yet, and on an upgrade the previous version's files
/// still are. A hook is a separate process, so the stub's
/// SetDefaultDllDirectories does not cover it and it resolves imports starting
/// with its own directory: a pre_install binary with a payload-supplied
/// dependency fails to load on a fresh install and binds against the previous
/// version's copy on an upgrade. It has to depend on nothing but the OS and
/// itself.
enum class Phase
{
    PreInstall,   // before any payload file is written; only its own binary is there
    PostExtract,  // files are in place, nothing registered yet
    PreRegister,  // last chance before the registry and shortcuts are touched
    PostInstall,  // everything is done
    PreUninstall, // before anything is removed
    PostUninstall // after removal, before the directory goes
};

const char* phase_key(Phase phase);

/// The payload-relative path a hook's `run` names, empty when the spec is not a
/// well-formed payload reference and `why` says what was wrong with it.
///
/// Exposed so the pre_install stage can find the container member it has to
/// place early. It shares the rejection rules with the rest of hook target
/// resolution rather than restating them: a second copy of the `..` check is a
/// second chance to get wrong the one check that keeps a hook off the rest of
/// the disk.
std::string hook_payload_member(const std::string& spec, std::string& why);

/// Which account a hook runs as.
///
/// Installer is the installer's own token, whatever that is: an elevated
/// administrator, or SYSTEM under a deployment tool. User is the person at the
/// keyboard, at medium integrity, with their own profile.
enum class HookContext
{
    Installer,
    User,
};

struct HookOutcome
{
    std::string id;
    bool ran = false;
    bool ok = false;
    bool vital = false;
    HookContext context = HookContext::Installer;
    std::string account; // who it ran as, for the log line
    uint32_t exit_code = 0;
    std::string detail;
};

/// Runs every hook declared for a phase, in declaration order.
///
/// The security model in one paragraph: a hook may only execute a file that is
/// a member of the payload, whose SHA-256 matches a digest written in the
/// config, and which resolves inside the install directory. The config lives
/// inside the Authenticode-covered region, so changing either the digest or the
/// binary it names invalidates the installer's signature. That is the entire
/// reason hooks are allowed at all, and why there is no interpreter: the
/// signature is what vouches for the code, so the code has to be signed with it.
///
/// `as = "user"` changes who runs it, not what may be run. The digest is checked
/// by the elevated process before the token is dropped, so lowering privilege
/// never lowers the bar the binary had to clear.
///
/// A hook whose `when` is not satisfied is skipped without an outcome. It was
/// not declined, it was not selected, and reporting every unselected option as a
/// non-event buries the outcomes that mean something.
///
/// Returns false when a hook marked vital failed.
bool run_hooks(Phase phase, const Config& config, const InstallPlan& plan,
               std::vector<HookOutcome>& outcomes);

} // namespace lwi::stub
