#include "lwi/hash.h"

#include <windows.h>

#include <bcrypt.h>

#include "lwi/win_file.h"

namespace lwi
{
namespace
{

constexpr size_t kFileChunk = 1u << 20; // 1 MiB

bool nt_ok(NTSTATUS s)
{
    return s >= 0;
}

const char kHexDigits[] = "0123456789abcdef";

int hex_value(char c)
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

Sha256Hasher::~Sha256Hasher()
{
    if (handle_ != nullptr)
    {
        BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle_));
        handle_ = nullptr;
    }
}

Status Sha256Hasher::init()
{
    if (handle_ != nullptr)
    {
        return Status::error(Code::InvalidArgument, "hasher already initialised");
    }

    DWORD object_size = 0;
    DWORD written = 0;
    NTSTATUS st = BCryptGetProperty(BCRYPT_SHA256_ALG_HANDLE, BCRYPT_OBJECT_LENGTH,
                                    reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                    &written, 0);
    if (!nt_ok(st))
    {
        return Status::error(Code::CryptoError, "BCryptGetProperty(OBJECT_LENGTH) failed");
    }

    object_.assign(object_size, 0);

    BCRYPT_HASH_HANDLE h = nullptr;
    st = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &h, object_.data(),
                          static_cast<ULONG>(object_.size()), nullptr, 0, 0);
    if (!nt_ok(st))
    {
        return Status::error(Code::CryptoError, "BCryptCreateHash failed");
    }

    handle_ = h;
    return Status::ok();
}

Status Sha256Hasher::update(std::span<const uint8_t> data)
{
    if (handle_ == nullptr)
    {
        return Status::error(Code::InvalidArgument, "hasher not initialised");
    }

    // cbInput is a ULONG, so a span larger than 4 GB has to be fed in pieces.
    size_t offset = 0;
    while (offset < data.size())
    {
        const size_t take = (std::min)(data.size() - offset, static_cast<size_t>(1u << 30));
        const NTSTATUS st =
            BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(handle_),
                           const_cast<PUCHAR>(data.data() + offset), static_cast<ULONG>(take), 0);
        if (!nt_ok(st))
        {
            return Status::error(Code::CryptoError, "BCryptHashData failed");
        }
        offset += take;
    }
    return Status::ok();
}

Status Sha256Hasher::finish(Sha256& out)
{
    if (handle_ == nullptr)
    {
        return Status::error(Code::InvalidArgument, "hasher not initialised");
    }

    const NTSTATUS st = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(handle_), out.data(),
                                         static_cast<ULONG>(out.size()), 0);

    // The handle cannot be reused after finishing, so it is destroyed here
    // rather than left in a state a caller might mistake for usable.
    BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle_));
    handle_ = nullptr;

    if (!nt_ok(st))
    {
        return Status::error(Code::CryptoError, "BCryptFinishHash failed");
    }
    return Status::ok();
}

Status sha256(std::span<const uint8_t> data, Sha256& out)
{
    Sha256Hasher h;
    if (Status s = h.init(); !s)
    {
        return s;
    }
    if (Status s = h.update(data); !s)
    {
        return s;
    }
    return h.finish(out);
}

Status sha256_file(const std::wstring& path, Sha256& out)
{
    WinHandle file = win_open_read(path);
    if (!file)
    {
        return Status::error(Code::IoError,
                             win32_message("CreateFileW for hashing", GetLastError()));
    }

    Sha256Hasher h;
    if (Status s = h.init(); !s)
    {
        return s;
    }

    std::vector<uint8_t> buf(kFileChunk);
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(file.get(), buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr))
        {
            return Status::error(Code::IoError, win32_message("ReadFile", GetLastError()));
        }
        if (read == 0)
        {
            break;
        }
        if (Status s = h.update(std::span<const uint8_t>(buf.data(), read)); !s)
        {
            return s;
        }
    }

    return h.finish(out);
}

std::string to_hex(const Sha256& digest)
{
    std::string out;
    out.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i)
    {
        out[i * 2] = kHexDigits[digest[i] >> 4];
        out[i * 2 + 1] = kHexDigits[digest[i] & 0x0F];
    }
    return out;
}

bool from_hex(std::string_view hex, Sha256& out)
{
    if (hex.size() != out.size() * 2)
    {
        return false;
    }
    for (size_t i = 0; i < out.size(); ++i)
    {
        const int hi = hex_value(hex[i * 2]);
        const int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
        {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

uint32_t crc32(std::span<const uint8_t> data)
{
    static uint32_t table[256];
    static bool built = false;
    if (!built)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        built = true;
    }

    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data)
    {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace lwi
