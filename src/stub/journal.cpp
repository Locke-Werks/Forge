#include "journal.h"

#include <windows.h>

#include <cstring>

#include "lwi/win_file.h"
#include "ops.h"

namespace lwi::stub
{
namespace
{

constexpr uint32_t kJournalMagic = 0x314A5749; // "IWJ1"
constexpr const wchar_t* kJournalName = L"install.journal";
constexpr const wchar_t* kBackupSuffix = L".lw-old";

std::wstring journal_file(const std::wstring& install_dir)
{
    return install_dir + L"\\" + kMetaDir + L"\\" + kJournalName;
}

void append_u32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void append_string(std::vector<uint8_t>& out, const std::wstring& value)
{
    const std::string utf8 = to_utf8(value);
    append_u32(out, static_cast<uint32_t>(utf8.size()));
    out.insert(out.end(), utf8.begin(), utf8.end());
}

bool take_u32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& out)
{
    if (pos + 4 > in.size())
    {
        return false;
    }
    out = static_cast<uint32_t>(in[pos]) | (static_cast<uint32_t>(in[pos + 1]) << 8) |
          (static_cast<uint32_t>(in[pos + 2]) << 16) | (static_cast<uint32_t>(in[pos + 3]) << 24);
    pos += 4;
    return true;
}

bool take_string(const std::vector<uint8_t>& in, size_t& pos, std::wstring& out)
{
    uint32_t length = 0;
    if (!take_u32(in, pos, length) || pos + length > in.size())
    {
        return false;
    }
    out = to_wide(std::string(reinterpret_cast<const char*>(in.data()) + pos, length));
    pos += length;
    return true;
}

/// Appends to a file and flushes before returning.
///
/// Opened and closed per record rather than held open. An installer writes a
/// few thousand records at most, and a handle held across the whole install is
/// a handle that is open when the process dies, with the last records still in
/// the cache.
Status append_flushed(const std::wstring& path, const std::vector<uint8_t>& data)
{
    const std::wstring full = long_path(path);
    HANDLE file = CreateFileW(full.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return Status::error(Code::IoError, win32_message("CreateFileW (journal)", GetLastError()));
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    if (ok)
    {
        FlushFileBuffers(file);
    }
    CloseHandle(file);

    if (!ok)
    {
        return Status::error(Code::IoError, win32_message("WriteFile (journal)", GetLastError()));
    }
    return Status::ok();
}

void force_delete(const std::wstring& path)
{
    const std::wstring full = long_path(path);
    SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(full.c_str()) && GetLastError() == ERROR_ACCESS_DENIED)
    {
        // Locked, most often by the process being replaced. Defer rather than
        // abandon the rollback.
        MoveFileExW(full.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

void undo(const std::wstring& install_dir, const JournalRecord& record)
{
    switch (record.op)
    {
    case JournalOp::FileCreated:
        force_delete(install_dir + L"\\" + record.a);
        break;

    case JournalOp::FileReplaced:
    {
        const std::wstring target = long_path(install_dir + L"\\" + record.a);
        const std::wstring backup = long_path(install_dir + L"\\" + record.b);
        force_delete(install_dir + L"\\" + record.a);
        // MOVEFILE_REPLACE_EXISTING as well: the delete above may have been
        // deferred to reboot, leaving the target still present.
        MoveFileExW(backup.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        break;
    }

    case JournalOp::DirCreated:
        // Only if empty. A directory that has picked up anything else is not
        // ours to remove.
        RemoveDirectoryW(long_path(install_dir + L"\\" + record.a).c_str());
        break;

    case JournalOp::None:
        break;
    }
}

std::vector<JournalRecord> parse(const std::vector<uint8_t>& blob)
{
    std::vector<JournalRecord> out;
    size_t pos = 0;

    uint32_t magic = 0;
    if (!take_u32(blob, pos, magic) || magic != kJournalMagic)
    {
        return out;
    }

    for (;;)
    {
        const size_t start = pos;
        uint32_t op = 0;
        JournalRecord record;
        if (!take_u32(blob, pos, op) || !take_string(blob, pos, record.a) ||
            !take_string(blob, pos, record.b))
        {
            // A truncated tail is expected after a crash: the last record was
            // being written when the power went. Everything before it is valid.
            pos = start;
            break;
        }
        record.op = static_cast<JournalOp>(op);
        out.push_back(std::move(record));
    }

    return out;
}

} // namespace

std::wstring Journal::backup_path_for(const std::wstring& relative, uint32_t sequence)
{
    return std::wstring(kMetaDir) + L"\\backup\\" + std::to_wstring(sequence) + kBackupSuffix +
           L"\\" + relative;
}

Status Journal::begin(const std::wstring& install_dir)
{
    install_dir_ = install_dir;
    journal_path_ = journal_file(install_dir);
    records_.clear();

    std::vector<uint8_t> header;
    append_u32(header, kJournalMagic);
    if (Status s = append_flushed(journal_path_, header); !s)
    {
        return s;
    }

    active_ = true;
    return Status::ok();
}

Status Journal::record(JournalOp op, const std::wstring& a, const std::wstring& b)
{
    if (!active_)
    {
        return Status::error(Code::InvalidArgument, "journal is not open");
    }

    std::vector<uint8_t> blob;
    append_u32(blob, static_cast<uint32_t>(op));
    append_string(blob, a);
    append_string(blob, b);

    if (Status s = append_flushed(journal_path_, blob); !s)
    {
        return s;
    }

    records_.push_back(JournalRecord{op, a, b});
    return Status::ok();
}

void Journal::rollback()
{
    if (!active_)
    {
        return;
    }

    for (size_t i = records_.size(); i-- > 0;)
    {
        undo(install_dir_, records_[i]);
    }

    records_.clear();
    force_delete(journal_path_);
    RemoveDirectoryW(long_path(install_dir_ + L"\\" + kMetaDir + L"\\backup").c_str());
    active_ = false;
}

Status Journal::commit()
{
    if (!active_)
    {
        return Status::ok();
    }

    // Saved originals are only useful until the install succeeds. Keeping them
    // would double the footprint of every upgrade.
    for (const JournalRecord& entry : records_)
    {
        if (entry.op == JournalOp::FileReplaced)
        {
            force_delete(install_dir_ + L"\\" + entry.b);
        }
    }

    const std::wstring backup_root = install_dir_ + L"\\" + kMetaDir + L"\\backup";
    remove_directory_tree(backup_root);

    force_delete(journal_path_);
    records_.clear();
    active_ = false;
    return Status::ok();
}

size_t Journal::recover(const std::wstring& install_dir)
{
    const std::wstring path = journal_file(install_dir);
    if (GetFileAttributesW(long_path(path).c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return 0;
    }

    std::vector<uint8_t> blob;
    if (Status s = read_whole_file(path, blob); !s)
    {
        return 0;
    }

    const std::vector<JournalRecord> records = parse(blob);
    for (size_t i = records.size(); i-- > 0;)
    {
        undo(install_dir, records[i]);
    }

    force_delete(path);
    remove_directory_tree(install_dir + L"\\" + kMetaDir + L"\\backup");
    return records.size();
}

} // namespace lwi::stub
