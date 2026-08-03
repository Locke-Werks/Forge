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

/// The deepest ancestor of a key that already exists.
///
/// Recorded before writing, so uninstall knows how much of the key path this
/// install brought into being. Shared with services.cpp: association keys have
/// exactly the same problem, and the first version of that code left an empty
/// .ext key behind because it removed only the value.
std::wstring registry_existing_ancestor(bool machine, const std::wstring& subkey);

/// Removes keys this install created, leaf first, stopping at stop_at and
/// skipping any key that has since picked up unrelated content.
void registry_prune_created(bool machine, const std::wstring& subkey,
                            const std::wstring& stop_at);

/// Tells the shell and every running process that the environment changed.
///
/// Without it a new PATH entry is invisible until the user logs out, and the
/// usual report is that the installer did not work.
void broadcast_environment_change();

} // namespace lwi::stub
