#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lwi
{

/// A dotted numeric version, with anything after the numbers kept as a
/// pre-release tag.
///
/// Comparison is numeric per component, never lexicographic. String comparison
/// gets "10.0.2" versus "9.0.1" backwards, and an installer that gets that
/// backwards offers a downgrade as an upgrade.
struct Version
{
    std::vector<uint64_t> parts;
    std::string tag; // "beta.1" in "1.2.0-beta.1", empty for a release

    [[nodiscard]] bool empty() const { return parts.empty(); }
    [[nodiscard]] std::string to_string() const;
};

Version parse_version(std::string_view text);

/// Returns <0, 0 or >0.
///
/// Missing trailing components are zero, so 1.2 and 1.2.0 are equal. A version
/// carrying a pre-release tag sorts BEFORE the same version without one, which
/// is the semver rule and the one users expect: 1.0.0-rc1 precedes 1.0.0.
int compare_versions(const Version& a, const Version& b);
int compare_versions(std::string_view a, std::string_view b);

} // namespace lwi
