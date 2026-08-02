#include "stamp.h"

#include <windows.h>

#include <cstring>

#include "lwi/win_file.h"

namespace lwi::forge
{
namespace
{

// Resource ids. 1 for the icon group matches what Explorer picks up as the
// application icon, and 1 for RT_MANIFEST is CREATEPROCESS_MANIFEST_RESOURCE_ID.
constexpr WORD kIconGroupId = 1;
constexpr WORD kFirstIconId = 1;
constexpr WORD kVersionId = 1;
constexpr WORD kManifestId = 1;
constexpr WORD kLangNeutral = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
constexpr WORD kLangUsEnglish = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

#pragma pack(push, 2)
struct IconDirHeader
{
    uint16_t reserved;
    uint16_t type;
    uint16_t count;
};
struct IconDirEntryFile
{
    uint8_t width;
    uint8_t height;
    uint8_t color_count;
    uint8_t reserved;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t bytes_in_res;
    uint32_t image_offset;
};
struct IconDirEntryGroup
{
    uint8_t width;
    uint8_t height;
    uint8_t color_count;
    uint8_t reserved;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t bytes_in_res;
    uint16_t id;
};
#pragma pack(pop)

static_assert(sizeof(IconDirHeader) == 6);
static_assert(sizeof(IconDirEntryFile) == 16);
static_assert(sizeof(IconDirEntryGroup) == 14);

/// Builds the nested, length-prefixed, 4-byte-aligned blocks a VS_VERSIONINFO
/// resource is made of. Each block's length covers itself and its children, so
/// lengths are back-patched once the children are written.
class VersionBlockWriter
{
  public:
    [[nodiscard]] std::vector<uint8_t>& bytes() { return buf_; }

    size_t begin(std::wstring_view key, uint16_t value_length, uint16_t type)
    {
        align4();
        const size_t start = buf_.size();
        put16(0); // wLength, patched by end()
        put16(value_length);
        put16(type);
        put_wide(key);
        align4();
        return start;
    }

    void end(size_t start)
    {
        const uint16_t length = static_cast<uint16_t>(buf_.size() - start);
        buf_[start] = static_cast<uint8_t>(length & 0xFF);
        buf_[start + 1] = static_cast<uint8_t>(length >> 8);
    }

    void put16(uint16_t v)
    {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>(v >> 8));
    }

    void put32(uint32_t v)
    {
        put16(static_cast<uint16_t>(v & 0xFFFF));
        put16(static_cast<uint16_t>(v >> 16));
    }

    void put_wide(std::wstring_view s)
    {
        for (wchar_t c : s)
        {
            put16(static_cast<uint16_t>(c));
        }
        put16(0); // NUL
    }

    void align4()
    {
        while (buf_.size() % 4 != 0)
        {
            buf_.push_back(0);
        }
    }

    /// A String entry. wValueLength is counted in WCHARs including the NUL,
    /// which is the one field in this format that is not a byte count.
    void put_string(std::wstring_view key, std::wstring_view value)
    {
        const uint16_t value_words = static_cast<uint16_t>(value.size() + 1);
        const size_t start = begin(key, value_words, 1);
        put_wide(value);
        end(start);
    }

  private:
    std::vector<uint8_t> buf_;
};

} // namespace

Status build_version_resource(const VersionInfo& info, std::vector<uint8_t>& out)
{
    const uint32_t version_ms =
        (static_cast<uint32_t>(info.major) << 16) | static_cast<uint32_t>(info.minor);
    const uint32_t version_ls =
        (static_cast<uint32_t>(info.patch) << 16) | static_cast<uint32_t>(info.build);

    VersionBlockWriter w;

    const size_t root = w.begin(L"VS_VERSION_INFO", 52, 0);

    // VS_FIXEDFILEINFO
    w.put32(0xFEEF04BD); // dwSignature
    w.put32(0x00010000); // dwStrucVersion
    w.put32(version_ms);
    w.put32(version_ls);
    w.put32(version_ms);
    w.put32(version_ls);
    w.put32(0x0000003F); // dwFileFlagsMask
    w.put32(0x00000000); // dwFileFlags
    w.put32(0x00000004); // dwFileOS = VOS_NT_WINDOWS32
    w.put32(0x00000001); // dwFileType = VFT_APP
    w.put32(0x00000000); // dwFileSubtype
    w.put32(0x00000000); // dwFileDateMS
    w.put32(0x00000000); // dwFileDateLS
    w.align4();

    {
        const size_t sfi = w.begin(L"StringFileInfo", 0, 1);
        {
            // 0409 US English, 04B0 Unicode. Matches the block name every
            // version-resource reader looks for first.
            const size_t table = w.begin(L"040904B0", 0, 1);

            const std::wstring version_text = std::to_wstring(info.major) + L"." +
                                              std::to_wstring(info.minor) + L"." +
                                              std::to_wstring(info.patch) + L"." +
                                              std::to_wstring(info.build);

            w.put_string(L"CompanyName", to_wide(info.company));
            w.put_string(L"FileDescription", to_wide(info.file_description));
            w.put_string(L"FileVersion", version_text);
            w.put_string(L"InternalName", to_wide(info.internal_name));
            w.put_string(L"LegalCopyright", to_wide(info.legal_copyright));
            w.put_string(L"OriginalFilename", to_wide(info.original_filename));
            w.put_string(L"ProductName", to_wide(info.product_name));
            w.put_string(L"ProductVersion", version_text);

            w.end(table);
        }
        w.end(sfi);
    }

    {
        const size_t vfi = w.begin(L"VarFileInfo", 0, 1);
        {
            const size_t var = w.begin(L"Translation", 4, 0);
            w.put32(0x04B00409);
            w.end(var);
        }
        w.end(vfi);
    }

    w.end(root);

    out = std::move(w.bytes());
    return Status::ok();
}

Status build_icon_resources(const std::vector<uint8_t>& ico,
                            std::vector<std::vector<uint8_t>>& images, std::vector<uint8_t>& group)
{
    images.clear();
    group.clear();

    if (ico.size() < sizeof(IconDirHeader))
    {
        return Status::error(Code::InvalidArgument, "icon file is too small");
    }

    IconDirHeader header{};
    std::memcpy(&header, ico.data(), sizeof(header));
    if (header.reserved != 0 || header.type != 1 || header.count == 0)
    {
        return Status::error(Code::InvalidArgument, "not a valid .ico file");
    }

    const size_t entries_size = static_cast<size_t>(header.count) * sizeof(IconDirEntryFile);
    if (ico.size() < sizeof(IconDirHeader) + entries_size)
    {
        return Status::error(Code::InvalidArgument, "icon directory is truncated");
    }

    // The group directory shares the header but its entries carry a resource id
    // where the file's carry a byte offset, so it is 2 bytes shorter per entry.
    group.resize(sizeof(IconDirHeader) +
                 static_cast<size_t>(header.count) * sizeof(IconDirEntryGroup));
    std::memcpy(group.data(), &header, sizeof(header));

    for (uint16_t i = 0; i < header.count; ++i)
    {
        IconDirEntryFile entry{};
        std::memcpy(&entry, ico.data() + sizeof(IconDirHeader) + i * sizeof(IconDirEntryFile),
                    sizeof(entry));

        if (static_cast<uint64_t>(entry.image_offset) + entry.bytes_in_res > ico.size())
        {
            return Status::error(Code::InvalidArgument, "icon image lies outside the file");
        }

        images.emplace_back(ico.begin() + entry.image_offset,
                            ico.begin() + entry.image_offset + entry.bytes_in_res);

        IconDirEntryGroup g{};
        g.width = entry.width;
        g.height = entry.height;
        g.color_count = entry.color_count;
        g.reserved = entry.reserved;
        g.planes = entry.planes;
        g.bit_count = entry.bit_count;
        g.bytes_in_res = entry.bytes_in_res;
        g.id = static_cast<uint16_t>(kFirstIconId + i);

        std::memcpy(group.data() + sizeof(IconDirHeader) + i * sizeof(IconDirEntryGroup), &g,
                    sizeof(g));
    }

    return Status::ok();
}

Status stamp_resources(const std::wstring& exe_path, const VersionInfo& version,
                       const std::vector<uint8_t>& icon_file, const std::string& manifest_xml)
{
    std::vector<std::vector<uint8_t>> icon_images;
    std::vector<uint8_t> icon_group;
    if (!icon_file.empty())
    {
        if (Status s = build_icon_resources(icon_file, icon_images, icon_group); !s)
        {
            return s;
        }
    }

    std::vector<uint8_t> version_blob;
    if (Status s = build_version_resource(version, version_blob); !s)
    {
        return s;
    }

    // bDeleteExistingResources = TRUE. The stub ships with its own placeholder
    // icon and version resource; leaving them behind would mean a product's
    // installer carried two icon groups and Explorer picking whichever sorts
    // first.
    HANDLE update = BeginUpdateResourceW(exe_path.c_str(), TRUE);
    if (update == nullptr)
    {
        return Status::error(Code::IoError,
                             win32_message("BeginUpdateResourceW", GetLastError()));
    }

    const auto fail = [&](const char* what) {
        const DWORD err = GetLastError();
        EndUpdateResourceW(update, TRUE); // discard
        return Status::error(Code::IoError, win32_message(what, err));
    };

    if (!UpdateResourceW(update, RT_VERSION, MAKEINTRESOURCEW(kVersionId), kLangUsEnglish,
                         version_blob.data(), static_cast<DWORD>(version_blob.size())))
    {
        return fail("UpdateResourceW(RT_VERSION)");
    }

    if (!manifest_xml.empty())
    {
        if (!UpdateResourceW(update, RT_MANIFEST, MAKEINTRESOURCEW(kManifestId), kLangUsEnglish,
                             const_cast<char*>(manifest_xml.data()),
                             static_cast<DWORD>(manifest_xml.size())))
        {
            return fail("UpdateResourceW(RT_MANIFEST)");
        }
    }

    for (size_t i = 0; i < icon_images.size(); ++i)
    {
        if (!UpdateResourceW(update, RT_ICON,
                             MAKEINTRESOURCEW(static_cast<WORD>(kFirstIconId + i)), kLangNeutral,
                             icon_images[i].data(), static_cast<DWORD>(icon_images[i].size())))
        {
            return fail("UpdateResourceW(RT_ICON)");
        }
    }

    if (!icon_group.empty())
    {
        if (!UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(kIconGroupId), kLangNeutral,
                             icon_group.data(), static_cast<DWORD>(icon_group.size())))
        {
            return fail("UpdateResourceW(RT_GROUP_ICON)");
        }
    }

    if (!EndUpdateResourceW(update, FALSE))
    {
        return Status::error(Code::IoError, win32_message("EndUpdateResourceW", GetLastError()));
    }

    return Status::ok();
}

} // namespace lwi::forge
