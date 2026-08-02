#include "lwi/win_file.h"

#include <windows.h>

namespace lwi
{
namespace
{

constexpr DWORD kChunk = 1u << 20;

} // namespace

WinHandle::~WinHandle()
{
    reset();
}

WinHandle& WinHandle::operator=(WinHandle&& other) noexcept
{
    if (this != &other)
    {
        reset(other.release());
    }
    return *this;
}

bool WinHandle::valid() const
{
    return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
}

void* WinHandle::release()
{
    void* h = h_;
    h_ = nullptr;
    return h;
}

void WinHandle::reset(void* h)
{
    if (valid())
    {
        CloseHandle(static_cast<HANDLE>(h_));
    }
    h_ = h;
}

std::wstring long_path(const std::wstring& path)
{
    if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0)
    {
        return path;
    }
    // A UNC path becomes \\?\UNC\server\share, not \\?\\\server\share.
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
    {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    // Relative paths cannot take the prefix, because the prefix disables the
    // normalisation that would resolve them. Leave them alone and let the API
    // fail visibly rather than producing a path that means something else.
    if (path.size() < 3 || path[1] != L':')
    {
        return path;
    }
    return L"\\\\?\\" + path;
}

WinHandle win_open_read(const std::wstring& path)
{
    const std::wstring p = long_path(path);
    return WinHandle(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
}

WinHandle win_create_new(const std::wstring& path)
{
    const std::wstring p = long_path(path);
    return WinHandle(CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
}

WinHandle win_create_always(const std::wstring& path)
{
    const std::wstring p = long_path(path);
    return WinHandle(CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
}

Status read_whole_file(const std::wstring& path, std::vector<uint8_t>& out)
{
    WinHandle h = win_open_read(path);
    if (!h)
    {
        return Status::error(Code::IoError, win32_message("CreateFileW", GetLastError()));
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.get(), &size))
    {
        return Status::error(Code::IoError, win32_message("GetFileSizeEx", GetLastError()));
    }

    out.assign(static_cast<size_t>(size.QuadPart), 0);

    size_t offset = 0;
    while (offset < out.size())
    {
        const DWORD take = static_cast<DWORD>((std::min)(out.size() - offset,
                                                         static_cast<size_t>(kChunk)));
        DWORD read = 0;
        if (!ReadFile(h.get(), out.data() + offset, take, &read, nullptr))
        {
            return Status::error(Code::IoError, win32_message("ReadFile", GetLastError()));
        }
        if (read == 0)
        {
            break;
        }
        offset += read;
    }
    out.resize(offset);
    return Status::ok();
}

namespace
{

Status write_all(HANDLE h, std::span<const uint8_t> data)
{
    size_t offset = 0;
    while (offset < data.size())
    {
        const DWORD take = static_cast<DWORD>((std::min)(data.size() - offset,
                                                         static_cast<size_t>(kChunk)));
        DWORD written = 0;
        if (!WriteFile(h, data.data() + offset, take, &written, nullptr))
        {
            return Status::error(Code::IoError, win32_message("WriteFile", GetLastError()));
        }
        offset += written;
    }
    return Status::ok();
}

} // namespace

Status write_whole_file(const std::wstring& path, std::span<const uint8_t> data)
{
    WinHandle h = win_create_always(path);
    if (!h)
    {
        return Status::error(Code::IoError, win32_message("CreateFileW", GetLastError()));
    }
    if (Status s = write_all(h.get(), data); !s)
    {
        return s;
    }
    // Contents must reach disk before anything renames or signs this file. A
    // rename flushes the rename, not the data written before it.
    if (!FlushFileBuffers(h.get()))
    {
        return Status::error(Code::IoError, win32_message("FlushFileBuffers", GetLastError()));
    }
    return Status::ok();
}

Status append_to_file(const std::wstring& path, std::span<const uint8_t> data)
{
    const std::wstring p = long_path(path);
    WinHandle h(CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h)
    {
        return Status::error(Code::IoError, win32_message("CreateFileW for append", GetLastError()));
    }

    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h.get(), zero, nullptr, FILE_END))
    {
        return Status::error(Code::IoError, win32_message("SetFilePointerEx", GetLastError()));
    }
    if (Status s = write_all(h.get(), data); !s)
    {
        return s;
    }
    if (!FlushFileBuffers(h.get()))
    {
        return Status::error(Code::IoError, win32_message("FlushFileBuffers", GetLastError()));
    }
    return Status::ok();
}

Status file_size(const std::wstring& path, uint64_t& out)
{
    WinHandle h = win_open_read(path);
    if (!h)
    {
        return Status::error(Code::IoError, win32_message("CreateFileW", GetLastError()));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.get(), &size))
    {
        return Status::error(Code::IoError, win32_message("GetFileSizeEx", GetLastError()));
    }
    out = static_cast<uint64_t>(size.QuadPart);
    return Status::ok();
}

std::wstring to_wide(std::string_view utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    if (need <= 0)
    {
        return {};
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), need);
    return out;
}

std::string to_utf8(std::wstring_view wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0)
    {
        return {};
    }
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

Status self_path(std::wstring& out)
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
        {
            return Status::error(Code::IoError,
                                 win32_message("GetModuleFileNameW", GetLastError()));
        }
        if (n < buf.size())
        {
            buf.resize(n);
            out = buf;
            return Status::ok();
        }
        // Truncated. GetModuleFileNameW does not tell you the needed size, so
        // grow and retry rather than guessing.
        buf.resize(buf.size() * 2);
    }
}

} // namespace lwi
