#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lwi/error.h"

namespace lwi
{

/// The compiled configuration, as a flat map of dotted keys to UTF-8 values.
///
/// Flat rather than a tree because the stub is the consumer and a flat map
/// needs no recursive parser inside an elevated binary. The forge does the
/// structural work: TOML tables and arrays are flattened on the way in, so
///
///     [[preflight]]
///     type = "os_build"
///     min  = 17763
///
/// becomes preflight.0.type and preflight.0.min. Arrays keep their index, so
/// ordering, which matters for actions and hooks, survives the flattening.
class Config
{
  public:
    void set(std::string key, std::string value);

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::string_view get(std::string_view key,
                                       std::string_view fallback = {}) const;
    [[nodiscard]] bool get_bool(std::string_view key, bool fallback = false) const;
    [[nodiscard]] int64_t get_int(std::string_view key, int64_t fallback = 0) const;
    [[nodiscard]] uint32_t get_color(std::string_view key, uint32_t fallback) const;

    /// Number of elements in a flattened array, i.e. the count of distinct
    /// indices present under "<prefix>.<n>.".
    [[nodiscard]] size_t array_size(std::string_view prefix) const;

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const
    {
        return entries_;
    }

    Status encode(std::vector<uint8_t>& out) const;
    Status decode(std::span<const uint8_t> in);

  private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

/// Parses "#rrggbb" or "#aarrggbb" into 0xAARRGGBB. Returns false on anything
/// else rather than silently producing black, because a mistyped brand colour
/// that renders as black is a bug someone ships.
bool parse_color(std::string_view text, uint32_t& out);

} // namespace lwi
