#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "lwi/error.h"

namespace lwi
{

/// Where the Authenticode-relevant boundaries of a PE image sit.
///
/// The whole packaging design rests on one property, which was verified
/// empirically rather than assumed: bytes appended past the last section, but
/// before the attribute certificate table, ARE covered by the Authenticode
/// digest. The signing whitepaper's step 14 states the extra-data region as
/// beginning at SUM_OF_BYTES_HASHED and running for
///
///     file_size - (cert_table_size + SUM_OF_BYTES_HASHED)
///
/// so appending a payload and THEN signing puts the payload inside the hash.
/// Signing first and appending second would leave it outside, which is the
/// CVE-2013-3900 shape and is exactly what this design must not do.
struct PeLayout
{
    bool is_pe32_plus = false;
    uint64_t file_size = 0;

    uint32_t e_lfanew = 0;
    uint32_t opt_header_off = 0;
    uint32_t size_of_headers = 0;

    /// File offset of OptionalHeader.CheckSum. Excluded from the digest, which
    /// is why it can be rewritten after appending without breaking a signature.
    uint32_t checksum_off = 0;

    /// File offset of the DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] entry
    /// itself, i.e. where the 8 bytes {VirtualAddress, Size} live.
    uint32_t security_dir_off = 0;

    /// DataDirectory[4].VirtualAddress. Despite the name this is a FILE OFFSET,
    /// not an RVA. PE/COFF says so verbatim: "These certificates are not loaded
    /// into memory as part of the image. As such, the first field of this entry,
    /// which is normally an RVA, is a file pointer instead." Reading it as an
    /// RVA is the single most common bug in self-extracting code.
    uint32_t cert_table_off = 0;
    uint32_t cert_table_size = 0;

    /// SUM_OF_BYTES_HASHED: size_of_headers plus every section's SizeOfRawData.
    /// The first byte past the last section's raw data.
    uint64_t payload_start = 0;

    /// One past the last byte available to us: the certificate table offset on
    /// a signed image, the file size on an unsigned one.
    uint64_t payload_end = 0;

    [[nodiscard]] bool is_signed() const { return cert_table_size != 0; }

    /// Bytes between the last section and the certificate table. This is our
    /// appended container plus any alignment padding signtool inserted.
    [[nodiscard]] uint64_t appended_size() const
    {
        return payload_end > payload_start ? payload_end - payload_start : 0;
    }
};

/// Parses just enough of a PE image to find the appended-data region.
///
/// Every field read is bounds-checked against the buffer. The stub runs this on
/// its own image, which an attacker with write access controls, so a malformed
/// header must produce an error rather than an out-of-bounds read.
Status pe_parse(std::span<const uint8_t> image, PeLayout& out);

/// Removes an existing Authenticode signature: truncates the image at the
/// certificate table and zeroes the data directory entry.
///
/// Needed because re-forging an already-signed stub, or re-signing after
/// stamping, must start from an unsigned image. signtool will not append a
/// second certificate table over an existing one in the layout we rely on.
Status pe_strip_signature(std::vector<uint8_t>& image);

/// Recomputes and writes OptionalHeader.CheckSum.
///
/// The checksum is excluded from the Authenticode digest, so this is safe to do
/// at any point. It is not required for a user-mode executable to run, but a
/// wrong checksum is a thing some security tooling flags, and fixing it costs
/// one call.
Status pe_update_checksum(std::span<uint8_t> image);

} // namespace lwi
