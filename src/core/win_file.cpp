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

std::wstring normalize_path(const std::wstring& path)
{
    // An empty path stays empty. Turning it into "." would hand CreateFileW the
    // working directory, which succeeds and is never what the caller meant.
    if (path.empty())
    {
        return path;
    }

    std::wstring in = path;
    for (wchar_t& c : in)
    {
        if (c == L'/')
        {
            c = L'\\';
        }
    }

    // How much of the front is the root and therefore never collapsed. A `..`
    // at the top of a drive or a share has nowhere to go, and Win32 leaves it
    // at the root rather than erroring, so this does the same.
    size_t root = 0;
    if (in.size() >= 2 && in[0] == L'\\' && in[1] == L'\\')
    {
        // \\server\share, or the \\?\ and \\.\ device prefixes, which are
        // already literal and must not be rewritten.
        if (in.size() >= 3 && (in[2] == L'?' || in[2] == L'.'))
        {
            return path;
        }
        const size_t server = in.find(L'\\', 2);
        const size_t share = server == std::wstring::npos ? std::wstring::npos
                                                          : in.find(L'\\', server + 1);
        root = share == std::wstring::npos ? in.size() : share + 1;
    }
    else if (in.size() >= 3 && in[1] == L':' && in[2] == L'\\')
    {
        root = 3;
    }
    else if (in.size() >= 2 && in[1] == L':')
    {
        // C:relative, which is drive-relative and not something to collapse.
        root = 2;
    }
    else if (!in.empty() && in[0] == L'\\')
    {
        root = 1;
    }

    std::vector<std::wstring> segments;
    size_t start = root;
    while (start <= in.size())
    {
        const size_t end = in.find(L'\\', start);
        const size_t stop = end == std::wstring::npos ? in.size() : end;
        const std::wstring segment = in.substr(start, stop - start);

        if (segment == L"..")
        {
            // Only inside the path. Popping past the root would turn C:\..\x
            // into a different drive's worth of nonsense.
            if (!segments.empty() && segments.back() != L"..")
            {
                segments.pop_back();
            }
            else if (root == 0)
            {
                segments.push_back(segment);
            }
        }
        else if (!segment.empty() && segment != L".")
        {
            segments.push_back(segment);
        }

        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }

    // Segments are joined to each other, never to the root: a root that needs a
    // separator already carries one. "C:" is the case that makes this matter,
    // because C:relative and C:\relative name different directories.
    std::wstring out = in.substr(0, root);
    for (size_t i = 0; i < segments.size(); ++i)
    {
        if (i != 0)
        {
            out += L'\\';
        }
        out += segments[i];
    }

    // "." and "a\.." both collapse to nothing, which as a path means the
    // working directory and is spelled ".".
    return out.empty() ? L"." : out;
}

std::wstring long_path(const std::wstring& path)
{
    if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0)
    {
        return path;
    }

    // Before anything else, because the prefix below is what disables Win32's
    // own normalisation and a `..` or a forward slash surviving into a
    // \\?\ path is ERROR_INVALID_NAME rather than a path that resolves.
    const std::wstring clean = normalize_path(path);

    // A UNC path becomes \\?\UNC\server\share, not \\?\\\server\share.
    if (clean.size() >= 2 && clean[0] == L'\\' && clean[1] == L'\\')
    {
        return L"\\\\?\\UNC\\" + clean.substr(2);
    }
    // Relative paths cannot take the prefix, because the prefix disables the
    // normalisation that would resolve them. Leave them alone and let the API
    // fail visibly rather than producing a path that means something else.
    if (clean.size() < 3 || clean[1] != L':' || clean[2] != L'\\')
    {
        return clean;
    }
    return L"\\\\?\\" + clean;
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
