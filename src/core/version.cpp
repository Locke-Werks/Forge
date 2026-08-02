#include "lwi/version.h"

#include <algorithm>
#include <charconv>

namespace lwi
{

Version parse_version(std::string_view text)
{
    Version out;

    // Split off a pre-release tag at the first '-' or '+'.
    const size_t tag_start = text.find_first_of("-+");
    std::string_view numeric = text;
    if (tag_start != std::string_view::npos)
    {
        out.tag = std::string(text.substr(tag_start + 1));
        numeric = text.substr(0, tag_start);
    }

    size_t pos = 0;
    while (pos <= numeric.size())
    {
        const size_t dot = numeric.find('.', pos);
        const std::string_view piece =
            numeric.substr(pos, dot == std::string_view::npos ? numeric.size() - pos : dot - pos);

        uint64_t value = 0;
        const auto result =
            std::from_chars(piece.data(), piece.data() + piece.size(), value);
        if (result.ec != std::errc{} || result.ptr != piece.data() + piece.size())
        {
            // A non-numeric component ends the version rather than being
            // silently treated as zero, which would make "1.x" equal "1.0".
            break;
        }
        out.parts.push_back(value);

        if (dot == std::string_view::npos)
        {
            break;
        }
        pos = dot + 1;
    }

    return out;
}

int compare_versions(const Version& a, const Version& b)
{
    const size_t count = (std::max)(a.parts.size(), b.parts.size());
    for (size_t i = 0; i < count; ++i)
    {
        const uint64_t left = i < a.parts.size() ? a.parts[i] : 0;
        const uint64_t right = i < b.parts.size() ? b.parts[i] : 0;
        if (left != right)
        {
            return left < right ? -1 : 1;
        }
    }

    // Equal numerically. A pre-release sorts before the release it precedes.
    if (a.tag.empty() != b.tag.empty())
    {
        return a.tag.empty() ? 1 : -1;
    }
    if (a.tag != b.tag)
    {
        return a.tag < b.tag ? -1 : 1;
    }
    return 0;
}

int compare_versions(std::string_view a, std::string_view b)
{
    return compare_versions(parse_version(a), parse_version(b));
}

std::string Version::to_string() const
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
        {
            out += '.';
        }
        out += std::to_string(parts[i]);
    }
    if (!tag.empty())
    {
        out += '-';
        out += tag;
    }
    return out;
}

} // namespace lwi
