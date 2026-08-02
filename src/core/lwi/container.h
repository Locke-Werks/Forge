#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lwi/compress.h"
#include "lwi/error.h"
#include "lwi/hash.h"

namespace lwi
{

inline constexpr uint64_t kFooterMagic = 0x544F4F4649574Cull; // "LWIFOOT" little-endian
inline constexpr uint32_t kFooterVersion = 1;
inline constexpr uint32_t kContainerVersion = 1;

/// Marks a container built from unsigned, locally-built stubs. The stub shows
/// "UNSIGNED BUILD" in its footer when this is set, so a development artifact
/// can never be mistaken for a shipping one.
inline constexpr uint32_t kFlagDevBuild = 1u << 0;

#pragma pack(push, 1)

struct Region
{
    uint64_t offset = 0;   // relative to the start of the container
    uint64_t size = 0;     // stored (compressed) size
    uint64_t raw_size = 0; // size after decompression
};
static_assert(sizeof(Region) == 24, "Region layout is on-disk format");

struct ContainerHeader
{
    uint8_t magic[4] = {'L', 'W', 'I', 'C'};
    uint16_t format_version = kContainerVersion;
    uint16_t header_size = 0;
    uint32_t flags = 0;
    uint32_t compression = 0; // CompressAlgo
    uint32_t file_count = 0;
    uint32_t reserved0 = 0;
    Region config;
    Region index;
    Region strings;
    Region data;
    uint8_t reserved1[8] = {};
};
static_assert(sizeof(ContainerHeader) == 128, "ContainerHeader layout is on-disk format");

struct FileEntry
{
    uint32_t path_offset = 0; // byte offset into the string arena
    uint32_t path_length = 0; // bytes, UTF-8, not NUL terminated
    uint64_t raw_size = 0;
    uint64_t stored_size = 0;
    uint64_t data_offset = 0; // relative to the start of the container
    uint8_t sha256[32] = {};  // of the UNCOMPRESSED contents
    uint64_t mtime = 0;       // FILETIME
    uint32_t attributes = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(FileEntry) == 80, "FileEntry layout is on-disk format");

/// Sits at the very end of the appended region, immediately before the
/// attribute certificate table on a signed image.
///
/// The size is a multiple of 8 so that a container padded to 8-byte alignment
/// leaves the whole file 8-byte aligned. signtool aligns the start of the
/// certificate table to 8 bytes and inserts up to 7 zero pad bytes if the file
/// does not already end there; padding ourselves means it inserts none. The
/// locator still scans backwards defensively, because relying on signtool never
/// padding is relying on an implementation detail.
struct Footer
{
    uint64_t magic = kFooterMagic;
    uint32_t footer_version = kFooterVersion;
    uint32_t flags = 0;
    uint64_t container_offset = 0; // absolute file offset
    uint64_t container_size = 0;
    uint8_t container_sha256[32] = {};
    uint32_t footer_crc32 = 0; // over every preceding byte of this struct
    uint32_t reserved = 0;
};
static_assert(sizeof(Footer) == 72, "Footer layout is on-disk format");

#pragma pack(pop)

/// One payload member, as seen by a reader.
struct FileView
{
    std::string path; // relative, forward or back slashes normalised to '\'
    uint64_t raw_size = 0;
    uint64_t stored_size = 0;
    uint64_t data_offset = 0;
    Sha256 digest{};
    uint64_t mtime = 0;
    uint32_t attributes = 0;
};

class ContainerWriter
{
  public:
    explicit ContainerWriter(CompressAlgo algo = CompressAlgo::Lzms) : algo_(algo) {}

    void set_flags(uint32_t flags) { flags_ = flags; }

    /// The compiled config blob. Opaque here: the container does not care what
    /// is inside it, only that it is covered by the hash and the signature.
    void set_config(std::span<const uint8_t> config) { config_.assign(config.begin(), config.end()); }

    Status add_file(std::string_view relative_path, std::span<const uint8_t> contents,
                    uint32_t attributes = 0, uint64_t mtime = 0);

    /// Serialises the container plus alignment padding plus the footer.
    ///
    /// container_file_offset is where the first byte will land in the finished
    /// executable, which the caller knows because it is appending to a file
    /// whose current length it just measured. The footer records it absolutely
    /// so the runtime locator never has to guess.
    Status build(uint64_t container_file_offset, std::vector<uint8_t>& out) const;

    [[nodiscard]] size_t file_count() const { return entries_.size(); }

  private:
    CompressAlgo algo_;
    uint32_t flags_ = 0;
    std::vector<uint8_t> config_;
    std::vector<FileEntry> entries_;
    std::vector<std::string> paths_;
    std::vector<std::vector<uint8_t>> blobs_; // stored (compressed) contents
};

class ContainerReader
{
  public:
    /// Locates and validates the container inside a whole PE image.
    ///
    /// Verifies the footer CRC and then the container's SHA-256 BEFORE any
    /// field of the header or index is trusted. The index is attacker-reachable
    /// data being parsed inside an elevated process, so it is checked as a
    /// whole first rather than field by field as it is consumed.
    Status open(std::span<const uint8_t> image);

    [[nodiscard]] const ContainerHeader& header() const { return header_; }
    [[nodiscard]] const std::vector<uint8_t>& config() const { return config_; }
    [[nodiscard]] const std::vector<FileView>& files() const { return files_; }
    [[nodiscard]] uint32_t flags() const { return header_.flags; }
    [[nodiscard]] bool is_dev_build() const { return (header_.flags & kFlagDevBuild) != 0; }

    /// Decompresses one member and verifies its SHA-256 against the index.
    /// A successful Decompress return is never taken as proof of integrity.
    Status extract(size_t index, std::vector<uint8_t>& out) const;

  private:
    std::vector<uint8_t> container_; // the whole container, copied out of the image
    ContainerHeader header_{};
    std::vector<uint8_t> config_;
    std::vector<FileView> files_;

    /// Per-entry FileEntry::flags. Bit 0 means the member was stored rather
    /// than compressed, which happens whenever compression made it larger.
    std::vector<uint32_t> stored_flags_;
};

/// Finds the footer in a whole-image buffer without parsing the container.
/// Exposed for tooling (lwforge inspect) and for tests.
Status find_footer(std::span<const uint8_t> image, Footer& out, uint64_t& footer_offset);

} // namespace lwi
