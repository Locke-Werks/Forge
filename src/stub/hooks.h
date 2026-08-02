#pragma once

#include <string>
#include <vector>

#include "lwi/config.h"
#include "lwi/container.h"
#include "ops.h"

namespace lwi::stub
{

/// Named points where a product can run its own code.
///
/// Only phases where the payload is already on disk. A pre-extract hook would
/// have to be unpacked somewhere first, and the only place available before the
/// install directory exists is a world-writable temp directory, which is the
/// one place a signed elevated process should not be executing from.
enum class Phase
{
    PostExtract,  // files are in place, nothing registered yet
    PreRegister,  // last chance before the registry and shortcuts are touched
    PostInstall,  // everything is done
    PreUninstall, // before anything is removed
    PostUninstall // after removal, before the directory goes
};

const char* phase_key(Phase phase);

struct HookOutcome
{
    std::string id;
    bool ran = false;
    bool ok = false;
    bool vital = false;
    uint32_t exit_code = 0;
    std::string detail;
};

/// Runs every hook declared for a phase.
///
/// The security model in one paragraph: a hook may only execute a file that is
/// a member of the payload, whose SHA-256 matches a digest written in the
/// config, and which resolves inside the install directory. The config lives
/// inside the Authenticode-covered region, so changing either the digest or the
/// binary it names invalidates the installer's signature. That is the entire
/// reason hooks are allowed at all, and why there is no interpreter: the
/// signature is what vouches for the code, so the code has to be signed with it.
///
/// Returns false when a hook marked vital failed.
bool run_hooks(Phase phase, const Config& config, const InstallPlan& plan,
               std::vector<HookOutcome>& outcomes);

} // namespace lwi::stub
