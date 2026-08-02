#include "lwi/pe_layout.h"

#include <cstring>

namespace lwi
{
namespace
{

// Structure offsets, spelled out rather than pulled from <winnt.h>, so the
// parser is testable off-Windows and so the PE/COFF citations sit next to the
// numbers they justify.
constexpr uint16_t kDosSignature = 0x5A4D; // 'MZ'
constexpr uint32_t kNtSignature = 0x00004550; // 'PE\0\0'
constexpr uint16_t kOptMagicPe32 = 0x010B;
constexpr uint16_t kOptMagicPe32Plus = 0x020B;

constexpr uint32_t kElfanewOff = 0x3C;
constexpr uint32_t kFileHeaderSize = 20;
constexpr uint32_t kSectionHeaderSize = 40;

// Identical in PE32 and PE32+ because everything that differs sits later.
constexpr uint32_t kOptSizeOfHeadersOff = 60;
constexpr uint32_t kOptCheckSumOff = 64;

constexpr uint32_t kOptNumRvaOff32 = 92;
constexpr uint32_t kOptNumRvaOff64 = 108;
constexpr uint32_t kOptDataDirOff32 = 96;
constexpr uint32_t kOptDataDirOff64 = 112;

constexpr uint32_t kDirEntrySecurity = 4;
constexpr uint32_t kDirEntrySize = 8;

// A PE at or above 4 GB cannot be signed: signtool fails with 0x80080057, and
// Microsoft documents that the hash may be wrong above that size even when
// signing appears to succeed. Refusing here turns a confusing signing failure
// into a clear packaging failure.
constexpr uint64_t kMaxSignableImage = 0xFFFFFFFFull;

bool read_u16(std::span<const uint8_t> b, uint64_t off, uint16_t& out)
{
    if (off + 2 > b.size())
    {
        return false;
    }
    out = static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
    return true;
}

bool read_u32(std::span<const uint8_t> b, uint64_t off, uint32_t& out)
{
    if (off + 4 > b.size())
    {
        return false;
    }
    out = static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
          (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
    return true;
}

void write_u32(std::span<uint8_t> b, uint64_t off, uint32_t v)
{
    b[off] = static_cast<uint8_t>(v & 0xFF);
    b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    b[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    b[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

Status malformed(const char* what)
{
    return Status::error(Code::MalformedPe, std::string("malformed PE: ") + what);
}

} // namespace

Status pe_parse(std::span<const uint8_t> image, PeLayout& out)
{
    out = PeLayout{};
    out.file_size = image.size();

    uint16_t dos_magic = 0;
    if (!read_u16(image, 0, dos_magic))
    {
        return Status::error(Code::NotPeImage, "file is too small to be a PE image");
    }
    if (dos_magic != kDosSignature)
    {
        return Status::error(Code::NotPeImage, "missing MZ signature");
    }

    uint32_t e_lfanew = 0;
    if (!read_u32(image, kElfanewOff, e_lfanew))
    {
        return malformed("truncated before e_lfanew");
    }
    // e_lfanew is attacker-influenced on a tampered image. Bound it before it is
    // used as a base for every subsequent offset.
    if (e_lfanew < kElfanewOff + 4 || e_lfanew > image.size())
    {
        return malformed("e_lfanew out of range");
    }
    out.e_lfanew = e_lfanew;

    uint32_t nt_sig = 0;
    if (!read_u32(image, e_lfanew, nt_sig))
    {
        return malformed("truncated at NT headers");
    }
    if (nt_sig != kNtSignature)
    {
        return Status::error(Code::NotPeImage, "missing PE signature");
    }

    const uint64_t file_header_off = static_cast<uint64_t>(e_lfanew) + 4;

    uint16_t num_sections = 0;
    uint16_t size_of_optional = 0;
    if (!read_u16(image, file_header_off + 2, num_sections) ||
        !read_u16(image, file_header_off + 16, size_of_optional))
    {
        return malformed("truncated file header");
    }

    const uint64_t opt_off = file_header_off + kFileHeaderSize;
    if (opt_off + size_of_optional > image.size())
    {
        return malformed("optional header extends past end of file");
    }
    out.opt_header_off = static_cast<uint32_t>(opt_off);

    uint16_t opt_magic = 0;
    if (!read_u16(image, opt_off, opt_magic))
    {
        return malformed("truncated optional header");
    }
    if (opt_magic == kOptMagicPe32Plus)
    {
        out.is_pe32_plus = true;
    }
    else if (opt_magic == kOptMagicPe32)
    {
        out.is_pe32_plus = false;
    }
    else
    {
        return Status::error(Code::Unsupported, "unsupported optional header magic");
    }

    if (!read_u32(image, opt_off + kOptSizeOfHeadersOff, out.size_of_headers))
    {
        return malformed("truncated at SizeOfHeaders");
    }
    if (out.size_of_headers > image.size())
    {
        return malformed("SizeOfHeaders exceeds file size");
    }
    out.checksum_off = static_cast<uint32_t>(opt_off + kOptCheckSumOff);

    const uint32_t num_rva_off = out.is_pe32_plus ? kOptNumRvaOff64 : kOptNumRvaOff32;
    const uint32_t data_dir_off = out.is_pe32_plus ? kOptDataDirOff64 : kOptDataDirOff32;

    uint32_t num_rva = 0;
    if (!read_u32(image, opt_off + num_rva_off, num_rva))
    {
        return malformed("truncated at NumberOfRvaAndSizes");
    }

    // An image with fewer than 5 directory entries simply has no security
    // directory. That is legal and means unsigned, not malformed.
    if (num_rva > kDirEntrySecurity)
    {
        const uint64_t sec_off =
            opt_off + data_dir_off + static_cast<uint64_t>(kDirEntrySecurity) * kDirEntrySize;
        if (sec_off + kDirEntrySize > image.size())
        {
            return malformed("security directory entry past end of file");
        }
        out.security_dir_off = static_cast<uint32_t>(sec_off);

        if (!read_u32(image, sec_off, out.cert_table_off) ||
            !read_u32(image, sec_off + 4, out.cert_table_size))
        {
            return malformed("truncated security directory entry");
        }

        if (out.cert_table_size != 0)
        {
            // cert_table_off is a file offset, not an RVA. Validate it as one.
            const uint64_t cert_end =
                static_cast<uint64_t>(out.cert_table_off) + out.cert_table_size;
            if (out.cert_table_off >= image.size() || cert_end > image.size())
            {
                return malformed("certificate table lies outside the file");
            }
        }
    }

    // SUM_OF_BYTES_HASHED. The whitepaper hashes the headers, then every
    // section's raw data in ascending PointerToRawData order, accumulating
    // SizeOfRawData. The first byte past that total is where appended data
    // begins. Sections with SizeOfRawData 0 (.bss and friends) contribute
    // nothing and must not be skipped in a way that loses the max-offset check.
    uint64_t sum_hashed = out.size_of_headers;
    uint64_t max_section_end = out.size_of_headers;

    const uint64_t sections_off = opt_off + size_of_optional;
    for (uint32_t i = 0; i < num_sections; ++i)
    {
        const uint64_t sh = sections_off + static_cast<uint64_t>(i) * kSectionHeaderSize;
        if (sh + kSectionHeaderSize > image.size())
        {
            return malformed("section table extends past end of file");
        }

        uint32_t size_of_raw = 0;
        uint32_t ptr_to_raw = 0;
        if (!read_u32(image, sh + 16, size_of_raw) || !read_u32(image, sh + 20, ptr_to_raw))
        {
            return malformed("truncated section header");
        }

        if (size_of_raw == 0)
        {
            continue;
        }

        const uint64_t end = static_cast<uint64_t>(ptr_to_raw) + size_of_raw;
        if (end > image.size())
        {
            return malformed("section raw data extends past end of file");
        }

        sum_hashed += size_of_raw;
        if (end > max_section_end)
        {
            max_section_end = end;
        }
    }

    // For a conformant image these agree. They diverge when sections overlap or
    // when there is a gap between them, and in either case SUM_OF_BYTES_HASHED
    // no longer marks the true end of section data, so our appended container
    // would be written into a hole the digest does not cover the way we assume.
    // Refuse rather than produce an installer that fails verification later.
    if (sum_hashed != max_section_end)
    {
        return malformed("section raw data is not contiguous; "
                         "sum of SizeOfRawData does not match the last section end");
    }
    out.payload_start = sum_hashed;

    if (out.payload_start > image.size())
    {
        return malformed("computed payload start is past end of file");
    }

    out.payload_end = out.cert_table_size != 0 ? out.cert_table_off : image.size();

    if (out.payload_end < out.payload_start)
    {
        return malformed("certificate table begins before the end of section data");
    }

    return Status::ok();
}

Status pe_strip_signature(std::vector<uint8_t>& image)
{
    PeLayout layout;
    if (Status s = pe_parse(image, layout); !s)
    {
        return s;
    }

    if (!layout.is_signed())
    {
        return Status::ok();
    }

    // The certificate table is the last thing in a conformant signed image, so
    // truncating at its offset removes it and any alignment padding with it.
    const uint32_t truncate_at = layout.cert_table_off;
    image.resize(truncate_at);

    // Zero {VirtualAddress, Size}. Leaving a stale pointer behind makes the
    // image look signed to anything that reads the directory without checking
    // the file length.
    std::span<uint8_t> buf(image);
    write_u32(buf, layout.security_dir_off, 0);
    write_u32(buf, layout.security_dir_off + 4, 0);

    return Status::ok();
}

Status pe_update_checksum(std::span<uint8_t> image)
{
    PeLayout layout;
    if (Status s = pe_parse(image, layout); !s)
    {
        return s;
    }

    if (image.size() > kMaxSignableImage)
    {
        return Status::error(Code::OutOfRange,
                             "image is 4 GB or larger and cannot be Authenticode signed");
    }

    // Zero the field before summing: the checksum cannot include itself.
    write_u32(image, layout.checksum_off, 0);

    uint32_t sum = 0;
    const uint64_t words = image.size() / 2;
    for (uint64_t i = 0; i < words; ++i)
    {
        const uint32_t w =
            static_cast<uint32_t>(image[i * 2]) | (static_cast<uint32_t>(image[i * 2 + 1]) << 8);
        sum += w;
        sum = (sum >> 16) + (sum & 0xFFFF);
    }
    if (image.size() & 1)
    {
        sum += image[image.size() - 1];
        sum = (sum >> 16) + (sum & 0xFFFF);
    }
    sum = ((sum >> 16) + sum) & 0xFFFF;

    const uint32_t checksum = sum + static_cast<uint32_t>(image.size());
    write_u32(image, layout.checksum_off, checksum);

    return Status::ok();
}

} // namespace lwi
