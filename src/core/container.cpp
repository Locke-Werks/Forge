#include "lwi/container.h"

#include <algorithm>
#include <cstring>

#include "lwi/pe_layout.h"

namespace lwi
{
namespace
{

/// Ceiling on index and string-arena sizes. These live inside the signed
/// region, so exceeding them means the packaging step went wrong, not that
/// someone is attacking us. Refusing still beats trusting a length field.
constexpr uint64_t kMaxIndexBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxStringBytes = 16ull * 1024 * 1024;
constexpr uint64_t kMaxConfigBytes = 64ull * 1024 * 1024;
constexpr uint32_t kMaxFileCount = 1'000'000;

void append_bytes(std::vector<uint8_t>& out, const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

void pad_to(std::vector<uint8_t>& out, size_t alignment)
{
    while (out.size() % alignment != 0)
    {
        out.push_back(0);
    }
}

/// Normalises a payload-relative path and refuses anything that could escape
/// the install directory. Called on the way in, so a bad path is a packaging
/// error the developer sees, not a runtime surprise on a customer machine.
Status normalise_path(std::string_view in, std::string& out)
{
    if (in.empty())
    {
        return Status::error(Code::InvalidArgument, "empty payload path");
    }
    if (in.size() > 32767)
    {
        return Status::error(Code::InvalidArgument, "payload path is too long");
    }

    out.clear();
    out.reserve(in.size());
    for (char c : in)
    {
        out.push_back(c == '/' ? '\\' : c);
    }

    if (out.front() == '\\')
    {
        return Status::error(Code::InvalidArgument, "payload path must be relative: " + out);
    }
    if (out.size() >= 2 && out[1] == ':')
    {
        return Status::error(Code::InvalidArgument, "payload path must not be absolute: " + out);
    }

    // Reject traversal on the normalised form. Checking for the literal ".."
    // substring would also reject a legitimate "..foo" filename, so segments
    // are compared exactly.
    size_t start = 0;
    while (start <= out.size())
    {
        const size_t end = out.find('\\', start);
        const std::string_view seg =
            std::string_view(out).substr(start, (end == std::string::npos ? out.size() : end) - start);
        if (seg == ".." || seg == ".")
        {
            return Status::error(Code::InvalidArgument,
                                 "payload path contains a relative segment: " + out);
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }

    return Status::ok();
}

} // namespace

Status ContainerWriter::add_file(std::string_view relative_path, std::span<const uint8_t> contents,
                                 uint32_t attributes, uint64_t mtime)
{
    std::string path;
    if (Status s = normalise_path(relative_path, path); !s)
    {
        return s;
    }

    if (std::find(paths_.begin(), paths_.end(), path) != paths_.end())
    {
        return Status::error(Code::InvalidArgument, "duplicate payload path: " + path);
    }

    FileEntry entry;
    entry.raw_size = contents.size();
    entry.attributes = attributes;
    entry.mtime = mtime;

    Sha256 digest{};
    if (Status s = sha256(contents, digest); !s)
    {
        return s;
    }
    std::memcpy(entry.sha256, digest.data(), digest.size());

    std::vector<uint8_t> stored;
    if (Status s = compress_buffer(algo_, contents, stored); !s)
    {
        return s;
    }

    // Compression that makes a file larger is not compression. Storing it raw
    // costs one flag and saves the decompressor a pointless pass.
    if (stored.size() >= contents.size())
    {
        stored.assign(contents.begin(), contents.end());
        entry.flags = 1; // stored, not compressed
    }
    entry.stored_size = stored.size();

    paths_.push_back(std::move(path));
    entries_.push_back(entry);
    blobs_.push_back(std::move(stored));

    return Status::ok();
}

Status ContainerWriter::build(uint64_t container_file_offset, std::vector<uint8_t>& out) const
{
    out.clear();

    // The string arena, built first so index entries can reference it.
    std::vector<uint8_t> strings;
    std::vector<FileEntry> entries = entries_;
    for (size_t i = 0; i < paths_.size(); ++i)
    {
        entries[i].path_offset = static_cast<uint32_t>(strings.size());
        entries[i].path_length = static_cast<uint32_t>(paths_[i].size());
        append_bytes(strings, paths_[i].data(), paths_[i].size());
    }

    ContainerHeader header;
    header.header_size = static_cast<uint16_t>(sizeof(ContainerHeader));
    header.flags = flags_;
    header.compression = static_cast<uint32_t>(algo_);
    header.file_count = static_cast<uint32_t>(entries.size());

    std::vector<uint8_t> config_stored;
    if (Status s = compress_buffer(algo_, config_, config_stored); !s)
    {
        return s;
    }
    std::vector<uint8_t> strings_stored;
    if (Status s = compress_buffer(algo_, strings, strings_stored); !s)
    {
        return s;
    }

    // Layout: header, config, strings, data, index.
    //
    // The index goes LAST because every entry carries the container-relative
    // offset of its data. Writing it earlier means either reserving a slot and
    // patching it, or compressing it twice, and the second compression can come
    // out larger than the first: filling in real offsets replaces a run of
    // zeroes with high-entropy values, so the reserved slot overflows. Placing
    // it after the data means the offsets are final before it is compressed,
    // and it is compressed exactly once.
    std::vector<uint8_t> body;
    body.reserve(sizeof(ContainerHeader) + config_stored.size() + strings_stored.size());

    body.resize(sizeof(ContainerHeader), 0); // placeholder, rewritten at the end

    header.config.offset = body.size();
    header.config.size = config_stored.size();
    header.config.raw_size = config_.size();
    append_bytes(body, config_stored.data(), config_stored.size());
    pad_to(body, 8);

    header.strings.offset = body.size();
    header.strings.size = strings_stored.size();
    header.strings.raw_size = strings.size();
    append_bytes(body, strings_stored.data(), strings_stored.size());
    pad_to(body, 8);

    header.data.offset = body.size();
    uint64_t data_bytes = 0;
    for (size_t i = 0; i < blobs_.size(); ++i)
    {
        entries[i].data_offset = body.size();
        append_bytes(body, blobs_[i].data(), blobs_[i].size());
        data_bytes += blobs_[i].size();
        pad_to(body, 8);
    }
    header.data.size = data_bytes;
    header.data.raw_size = data_bytes;

    // Every data_offset is now final, so the index compresses once.
    std::vector<uint8_t> index_raw;
    append_bytes(index_raw, entries.data(), entries.size() * sizeof(FileEntry));
    std::vector<uint8_t> index_stored;
    if (Status s = compress_buffer(algo_, index_raw, index_stored); !s)
    {
        return s;
    }

    header.index.offset = body.size();
    header.index.size = index_stored.size();
    header.index.raw_size = index_raw.size();
    append_bytes(body, index_stored.data(), index_stored.size());
    pad_to(body, 8);

    std::memcpy(body.data(), &header, sizeof(header));

    // Footer, plus whatever padding makes the finished file 8-byte aligned so
    // signtool inserts none of its own.
    Sha256 digest{};
    if (Status s = sha256(body, digest); !s)
    {
        return s;
    }

    Footer footer;
    footer.flags = flags_;
    footer.container_offset = container_file_offset;
    footer.container_size = body.size();
    std::memcpy(footer.container_sha256, digest.data(), digest.size());
    footer.footer_crc32 = crc32(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&footer),
                                                         offsetof(Footer, footer_crc32)));

    out = std::move(body);

    const uint64_t before_pad = container_file_offset + out.size();
    const size_t pad = static_cast<size_t>((8 - (before_pad % 8)) % 8);
    out.insert(out.end(), pad, 0);

    append_bytes(out, &footer, sizeof(footer));
    return Status::ok();
}

Status find_footer(std::span<const uint8_t> image, Footer& out, uint64_t& footer_offset)
{
    PeLayout layout;
    if (Status s = pe_parse(image, layout); !s)
    {
        return s;
    }

    const uint64_t boundary = layout.payload_end;
    if (boundary < sizeof(Footer))
    {
        return Status::error(Code::NoContainer, "no appended container");
    }

    // Scan back up to 8 bytes. Padding ourselves means signtool should insert
    // none, but the PE/COFF spec permits up to 7 zero pad bytes before the
    // certificate table and depending on a tool's current behaviour is how this
    // breaks silently on some future SDK.
    for (uint64_t back = 0; back <= 8; ++back)
    {
        if (boundary < back + sizeof(Footer))
        {
            break;
        }
        const uint64_t candidate = boundary - back - sizeof(Footer);
        if (candidate < layout.payload_start)
        {
            break;
        }

        Footer f{};
        std::memcpy(&f, image.data() + candidate, sizeof(Footer));
        if (f.magic != kFooterMagic)
        {
            continue;
        }

        const uint32_t expect = crc32(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(&f), offsetof(Footer, footer_crc32)));
        if (expect != f.footer_crc32)
        {
            continue;
        }

        out = f;
        footer_offset = candidate;
        return Status::ok();
    }

    return Status::error(Code::NoContainer, "no appended container");
}

Status ContainerReader::open(std::span<const uint8_t> image)
{
    Footer footer{};
    uint64_t footer_offset = 0;
    if (Status s = find_footer(image, footer, footer_offset); !s)
    {
        return s;
    }

    if (footer.footer_version != kFooterVersion)
    {
        return Status::error(Code::UnsupportedVersion,
                             "container footer version is newer than this build understands");
    }

    if (footer.container_offset > image.size() ||
        footer.container_size > image.size() - footer.container_offset)
    {
        return Status::error(Code::MalformedContainer, "container extends past end of file");
    }
    if (footer.container_offset + footer.container_size > footer_offset)
    {
        return Status::error(Code::MalformedContainer, "container overlaps its own footer");
    }

    container_.assign(image.begin() + static_cast<ptrdiff_t>(footer.container_offset),
                      image.begin() + static_cast<ptrdiff_t>(footer.container_offset +
                                                             footer.container_size));

    // Whole-container integrity BEFORE any structural field is believed.
    Sha256 actual{};
    if (Status s = sha256(container_, actual); !s)
    {
        return s;
    }
    if (std::memcmp(actual.data(), footer.container_sha256, actual.size()) != 0)
    {
        return Status::error(Code::HashMismatch, "container hash does not match its footer");
    }

    if (container_.size() < sizeof(ContainerHeader))
    {
        return Status::error(Code::MalformedContainer, "container is smaller than its header");
    }
    std::memcpy(&header_, container_.data(), sizeof(ContainerHeader));

    if (header_.magic[0] != 'L' || header_.magic[1] != 'W' || header_.magic[2] != 'I' ||
        header_.magic[3] != 'C')
    {
        return Status::error(Code::MalformedContainer, "bad container magic");
    }
    if (header_.format_version != kContainerVersion)
    {
        return Status::error(Code::UnsupportedVersion,
                             "container format version is newer than this build understands");
    }
    if (header_.file_count > kMaxFileCount)
    {
        return Status::error(Code::OutOfRange, "container declares an implausible file count");
    }

    const auto region_ok = [&](const Region& r, uint64_t max_raw) {
        return r.offset <= container_.size() && r.size <= container_.size() - r.offset &&
               r.raw_size <= max_raw;
    };

    if (!region_ok(header_.config, kMaxConfigBytes) || !region_ok(header_.index, kMaxIndexBytes) ||
        !region_ok(header_.strings, kMaxStringBytes))
    {
        return Status::error(Code::MalformedContainer, "container region is out of bounds");
    }

    const auto algo = static_cast<CompressAlgo>(header_.compression);

    if (Status s = decompress_buffer(algo,
                                     std::span<const uint8_t>(container_.data() +
                                                                  header_.config.offset,
                                                              static_cast<size_t>(header_.config.size)),
                                     header_.config.raw_size, config_);
        !s)
    {
        return s;
    }

    std::vector<uint8_t> index_raw;
    if (Status s = decompress_buffer(algo,
                                     std::span<const uint8_t>(container_.data() +
                                                                  header_.index.offset,
                                                              static_cast<size_t>(header_.index.size)),
                                     header_.index.raw_size, index_raw);
        !s)
    {
        return s;
    }

    std::vector<uint8_t> strings;
    if (Status s =
            decompress_buffer(algo,
                              std::span<const uint8_t>(container_.data() + header_.strings.offset,
                                                       static_cast<size_t>(header_.strings.size)),
                              header_.strings.raw_size, strings);
        !s)
    {
        return s;
    }

    if (index_raw.size() != static_cast<uint64_t>(header_.file_count) * sizeof(FileEntry))
    {
        return Status::error(Code::MalformedContainer,
                             "index size does not match the declared file count");
    }

    files_.clear();
    files_.reserve(header_.file_count);
    for (uint32_t i = 0; i < header_.file_count; ++i)
    {
        FileEntry entry{};
        std::memcpy(&entry, index_raw.data() + static_cast<size_t>(i) * sizeof(FileEntry),
                    sizeof(FileEntry));

        if (static_cast<uint64_t>(entry.path_offset) + entry.path_length > strings.size())
        {
            return Status::error(Code::MalformedContainer, "index entry path is out of bounds");
        }
        if (entry.data_offset > container_.size() ||
            entry.stored_size > container_.size() - entry.data_offset)
        {
            return Status::error(Code::MalformedContainer, "index entry data is out of bounds");
        }

        FileView view;
        view.path.assign(reinterpret_cast<const char*>(strings.data()) + entry.path_offset,
                         entry.path_length);
        view.raw_size = entry.raw_size;
        view.stored_size = entry.stored_size;
        view.data_offset = entry.data_offset;
        view.mtime = entry.mtime;
        view.attributes = entry.attributes;
        std::memcpy(view.digest.data(), entry.sha256, view.digest.size());

        // Re-check the path on the way out. The writer validated it, but the
        // reader is what runs elevated on a machine we do not control.
        std::string normalised;
        if (Status s = normalise_path(view.path, normalised); !s)
        {
            return Status::error(Code::MalformedContainer,
                                 "container holds an unsafe payload path: " + view.path);
        }
        view.path = normalised;

        // entry.flags bit 0 means stored rather than compressed.
        if ((entry.flags & 1u) != 0)
        {
            view.stored_size = entry.stored_size;
        }

        files_.push_back(std::move(view));
        stored_flags_.push_back(entry.flags);
    }

    return Status::ok();
}

Status ContainerReader::extract(size_t index, std::vector<uint8_t>& out) const
{
    if (index >= files_.size())
    {
        return Status::error(Code::OutOfRange, "payload index out of range");
    }

    const FileView& view = files_[index];
    const auto algo = (stored_flags_[index] & 1u) != 0
                          ? CompressAlgo::None
                          : static_cast<CompressAlgo>(header_.compression);

    const std::span<const uint8_t> stored(container_.data() + view.data_offset,
                                          static_cast<size_t>(view.stored_size));

    if (Status s = decompress_buffer(algo, stored, view.raw_size, out); !s)
    {
        return s;
    }

    // Decompress returning TRUE is not integrity. Verify the digest recorded in
    // the signed index before this data becomes a file on disk.
    Sha256 actual{};
    if (Status s = sha256(out, actual); !s)
    {
        return s;
    }
    if (actual != view.digest)
    {
        out.clear();
        return Status::error(Code::HashMismatch,
                             "payload member failed verification: " + view.path);
    }

    return Status::ok();
}

} // namespace lwi
