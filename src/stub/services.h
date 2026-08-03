#pragma once

#include <string>
#include <vector>

#include "lwi/config.h"
#include "ops.h"

namespace lwi::stub
{

/// Installs services and registers file or protocol associations.
///
/// Both record what they created into record.undo, so uninstall removes exactly
/// that. Split out from actions.cpp because both need a lot of Win32 nothing
/// else does, and because both are machine-scope only.
Status apply_services(const Config& config, const InstallPlan& plan, InstallRecord& record,
                      std::vector<std::string>* warnings);

Status apply_associations(const Config& config, const InstallPlan& plan, InstallRecord& record,
                          std::vector<std::string>* warnings);

/// Stops and deletes every service this install created.
void revert_services(const InstallRecord& record);

/// Removes the ProgIDs and Capabilities this install registered.
void revert_associations(const InstallRecord& record);

} // namespace lwi::stub
