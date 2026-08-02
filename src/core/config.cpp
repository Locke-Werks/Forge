#include "lwi/config.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <set>

namespace lwi
{
namespace
{

constexpr uint32_t kConfigMagic = 0x30474643; // "CFG0"
constexpr uint64_t kMaxEntries = 100'000;
constexpr uint64_t kMaxStringLen = 1u << 20;

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

bool take_u32(std::span<const uint8_t> in, size_t& pos, uint32_t& out)
{
    if (pos + 4 > in.size())
    {
        return false;
    }
    out = static_cast<uint32_t>(in[pos]) | (static_cast<uint32_t>(in[pos + 1]) << 8) |
          (static_cast<uint32_t>(in[pos + 2]) << 16) | (static_cast<uint32_t>(in[pos + 3]) << 24);
    pos += 4;
    return true;
}

int hex_val(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

void Config::set(std::string key, std::string value)
{
    for (auto& e : entries_)
    {
        if (e.first == key)
        {
            e.second = std::move(value);
            return;
        }
    }
    entries_.emplace_back(std::move(key), std::move(value));
}

bool Config::has(std::string_view key) const
{
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const auto& e) { return e.first == key; });
}

std::string_view Config::get(std::string_view key, std::string_view fallback) const
{
    for (const auto& e : entries_)
    {
        if (e.first == key)
        {
            return e.second;
        }
    }
    return fallback;
}

bool Config::get_bool(std::string_view key, bool fallback) const
{
    const std::string_view v = get(key);
    if (v.empty())
    {
        return fallback;
    }
    return v == "true" || v == "1" || v == "yes";
}

int64_t Config::get_int(std::string_view key, int64_t fallback) const
{
    const std::string_view v = get(key);
    if (v.empty())
    {
        return fallback;
    }
    int64_t out = 0;
    const auto* first = v.data();
    const auto* last = v.data() + v.size();
    const auto result = std::from_chars(first, last, out);
    if (result.ec != std::errc{} || result.ptr != last)
    {
        return fallback;
    }
    return out;
}

uint32_t Config::get_color(std::string_view key, uint32_t fallback) const
{
    uint32_t out = 0;
    if (parse_color(get(key), out))
    {
        return out;
    }
    return fallback;
}

size_t Config::array_size(std::string_view prefix) const
{
    std::set<uint64_t> indices;
    std::string needle(prefix);
    needle.push_back('.');

    for (const auto& e : entries_)
    {
        if (e.first.size() <= needle.size() || e.first.compare(0, needle.size(), needle) != 0)
        {
            continue;
        }
        const size_t start = needle.size();
        const size_t dot = e.first.find('.', start);
        if (dot == std::string::npos)
        {
            continue;
        }
        uint64_t idx = 0;
        const auto result =
            std::from_chars(e.first.data() + start, e.first.data() + dot, idx);
        if (result.ec == std::errc{} && result.ptr == e.first.data() + dot)
        {
            indices.insert(idx);
        }
    }
    return indices.size();
}

Status Config::encode(std::vector<uint8_t>& out) const
{
    out.clear();
    put_u32(out, kConfigMagic);
    put_u32(out, static_cast<uint32_t>(entries_.size()));

    for (const auto& e : entries_)
    {
        if (e.first.size() > kMaxStringLen || e.second.size() > kMaxStringLen)
        {
            return Status::error(Code::OutOfRange, "config entry is too large: " + e.first);
        }
        put_u32(out, static_cast<uint32_t>(e.first.size()));
        out.insert(out.end(), e.first.begin(), e.first.end());
        put_u32(out, static_cast<uint32_t>(e.second.size()));
        out.insert(out.end(), e.second.begin(), e.second.end());
    }
    return Status::ok();
}

Status Config::decode(std::span<const uint8_t> in)
{
    entries_.clear();

    size_t pos = 0;
    uint32_t magic = 0;
    uint32_t count = 0;
    if (!take_u32(in, pos, magic) || magic != kConfigMagic)
    {
        return Status::error(Code::MalformedContainer, "config blob has bad magic");
    }
    if (!take_u32(in, pos, count))
    {
        return Status::error(Code::MalformedContainer, "config blob is truncated");
    }
    if (count > kMaxEntries)
    {
        return Status::error(Code::OutOfRange, "config declares an implausible entry count");
    }

    entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t key_len = 0;
        uint32_t val_len = 0;

        if (!take_u32(in, pos, key_len) || key_len > kMaxStringLen ||
            pos + key_len > in.size())
        {
            return Status::error(Code::MalformedContainer, "config key is out of bounds");
        }
        std::string key(reinterpret_cast<const char*>(in.data()) + pos, key_len);
        pos += key_len;

        if (!take_u32(in, pos, val_len) || val_len > kMaxStringLen ||
            pos + val_len > in.size())
        {
            return Status::error(Code::MalformedContainer, "config value is out of bounds");
        }
        std::string value(reinterpret_cast<const char*>(in.data()) + pos, val_len);
        pos += val_len;

        entries_.emplace_back(std::move(key), std::move(value));
    }

    return Status::ok();
}

bool parse_color(std::string_view text, uint32_t& out)
{
    if (text.empty() || text.front() != '#')
    {
        return false;
    }
    const std::string_view digits = text.substr(1);
    if (digits.size() != 6 && digits.size() != 8)
    {
        return false;
    }

    uint32_t value = 0;
    for (char c : digits)
    {
        const int v = hex_val(c);
        if (v < 0)
        {
            return false;
        }
        value = (value << 4) | static_cast<uint32_t>(v);
    }

    // #rrggbb is opaque. Only #aarrggbb carries its own alpha.
    out = digits.size() == 6 ? (0xFF000000u | value) : value;
    return true;
}

} // namespace lwi
