#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lwi/error.h"

namespace lwi
{

/// Owning wrapper for a Win32 HANDLE that uses INVALID_HANDLE_VALUE as its
/// empty state. Declared with void* so this header does not drag in windows.h.
class WinHandle
{
  public:
    WinHandle() = default;
    explicit WinHandle(void* h) : h_(h) {}
    ~WinHandle();

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    WinHandle(WinHandle&& other) noexcept : h_(other.release()) {}
    WinHandle& operator=(WinHandle&& other) noexcept;

    [[nodiscard]] void* get() const { return h_; }
    [[nodiscard]] bool valid() const;
    explicit operator bool() const { return valid(); }

    void* release();
    void reset(void* h = nullptr);

  private:
    void* h_ = nullptr;
};

/// Prefixes a path with \\?\ so it is not subject to MAX_PATH.
///
/// longPathAware in the manifest is deliberately NOT used: it is necessary but
/// not sufficient, because HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\
/// LongPathsEnabled must also be 1 on the target machine, and that is not
/// something an installer can assume about a customer box. The prefix works
/// regardless, at the cost of disabling path normalisation, so the input must
/// already be absolute, backslash-only, and free of . and .. segments.
std::wstring long_path(const std::wstring& path);

WinHandle win_open_read(const std::wstring& path);
WinHandle win_create_new(const std::wstring& path);
WinHandle win_create_always(const std::wstring& path);

Status read_whole_file(const std::wstring& path, std::vector<uint8_t>& out);
Status write_whole_file(const std::wstring& path, std::span<const uint8_t> data);

/// Appends to an existing file and flushes. Used by the forge when writing the
/// container onto the end of a stamped stub.
Status append_to_file(const std::wstring& path, std::span<const uint8_t> data);

Status file_size(const std::wstring& path, uint64_t& out);

std::wstring to_wide(std::string_view utf8);
std::string to_utf8(std::wstring_view wide);

/// Full path of the running executable.
Status self_path(std::wstring& out);

} // namespace lwi
