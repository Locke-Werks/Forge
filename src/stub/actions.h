#pragma once

#include <string>
#include <vector>

#include "lwi/config.h"
#include "ops.h"

namespace lwi::stub
{

/// Applies the declarative [[actions]] that are not shortcuts: registry writes
/// and environment variables.
///
/// Every change captures what was there first, into record.undo, so uninstall
/// restores rather than guesses.
Status apply_actions(const Config& config, const InstallPlan& plan, InstallRecord& record,
                     std::vector<std::string>* warnings);

/// Reverses everything apply_actions recorded.
void revert_actions(const InstallRecord& record);

/// Tells the shell and every running process that the environment changed.
///
/// Without it a new PATH entry is invisible until the user logs out, and the
/// usual report is that the installer did not work.
void broadcast_environment_change();

} // namespace lwi::stub
