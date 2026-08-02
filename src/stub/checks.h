#pragma once

#include <string>
#include <vector>

#include "lwi/config.h"
#include "ops.h"

namespace lwi::stub
{

enum class CheckState
{
    Pass,
    Warn, // surfaced, does not block
    Fail, // blocks the install
};

struct CheckOutcome
{
    CheckState state = CheckState::Pass;
    std::string type;
    std::wstring message; // shown to the user, from the config's fail text
    std::wstring detail;  // what was actually found, for the log

    /// Set when the check is one the user can clear without leaving the
    /// installer, currently only a running process the Restart Manager can
    /// close on their behalf.
    bool recoverable = false;
};

/// Runs every [[preflight]] entry plus the built-in upgrade rules.
///
/// The vocabulary is fixed and typed. There is no expression language and no
/// scripting: a signed, elevated binary that evaluates arbitrary strings from
/// its own payload is a code-execution proxy, and the whole point of putting
/// the config inside the Authenticode-covered region is to avoid needing one.
std::vector<CheckOutcome> run_preflight(const Config& config, const InstallPlan& plan);

/// True when any outcome blocks.
bool has_blocking_failure(const std::vector<CheckOutcome>& outcomes);

/// Formats outcomes for --check-only and for the silent-mode log.
std::string format_outcomes(const std::vector<CheckOutcome>& outcomes);

/// Asks the Restart Manager which processes hold files under a directory, and
/// optionally shuts them down. Used both by the process_not_running check and
/// before an upgrade replaces files in place.
std::vector<std::wstring> processes_using(const std::wstring& directory,
                                          const std::vector<std::wstring>& files);

} // namespace lwi::stub
