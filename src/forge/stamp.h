#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lwi/error.h"

namespace lwi::forge
{

struct VersionInfo
{
    std::string company;
    std::string file_description;
    std::string internal_name;
    std::string original_filename;
    std::string product_name;
    std::string legal_copyright;
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    uint16_t build = 0;
};

/// Rewrites the icon, version resource and manifest of a PE on disk.
///
/// Must run BEFORE the container is appended and before signing.
/// EndUpdateResource rewrites the whole image and can move section raw offsets,
/// so anything computed from the layout beforehand is stale afterwards. The
/// caller re-reads and re-parses the file before deciding where to append.
Status stamp_resources(const std::wstring& exe_path, const VersionInfo& version,
                       const std::vector<uint8_t>& icon_file, const std::string& manifest_xml);

/// Splits a .ico file into the RT_ICON images and the RT_GROUP_ICON directory
/// that Windows expects. An .ico on disk and an icon in a PE are different
/// layouts: the directory entry holds a resource id in the PE, where the file
/// holds a byte offset.
Status build_icon_resources(const std::vector<uint8_t>& ico,
                            std::vector<std::vector<uint8_t>>& images,
                            std::vector<uint8_t>& group);

/// Serialises a VS_VERSIONINFO resource. Written by hand because there is no
/// API to produce one, and shelling out to rc.exe would make packaging depend
/// on a Windows SDK being installed on the packaging machine.
Status build_version_resource(const VersionInfo& info, std::vector<uint8_t>& out);

} // namespace lwi::forge
