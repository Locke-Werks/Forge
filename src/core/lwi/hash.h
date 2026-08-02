#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lwi/error.h"

namespace lwi
{

using Sha256 = std::array<uint8_t, 32>;

/// Incremental SHA-256 over CNG.
///
/// Uses the BCRYPT_SHA256_ALG_HANDLE pseudo-handle rather than
/// BCryptOpenAlgorithmProvider. Microsoft's own documentation steers Windows 10
/// and later callers to the pseudo-handles and notes that opening a provider is
/// expensive; on a Win10-minimum target the open and close pair is dead weight.
class Sha256Hasher
{
  public:
    Sha256Hasher() = default;
    ~Sha256Hasher();

    Sha256Hasher(const Sha256Hasher&) = delete;
    Sha256Hasher& operator=(const Sha256Hasher&) = delete;

    Status init();
    Status update(std::span<const uint8_t> data);
    Status finish(Sha256& out);

  private:
    void* handle_ = nullptr; // BCRYPT_HASH_HANDLE
    std::vector<uint8_t> object_;
};

Status sha256(std::span<const uint8_t> data, Sha256& out);

/// Hashes a file in chunks. Used for payload verification, where the file is
/// too large to justify holding twice in memory.
Status sha256_file(const std::wstring& path, Sha256& out);

std::string to_hex(const Sha256& digest);
bool from_hex(std::string_view hex, Sha256& out);

/// CRC-32 (IEEE, reflected, the zlib polynomial). Used only as a cheap
/// structural check on the footer, never as an integrity guarantee. Integrity
/// is SHA-256 plus the Authenticode signature over the whole appended region.
uint32_t crc32(std::span<const uint8_t> data);

} // namespace lwi
