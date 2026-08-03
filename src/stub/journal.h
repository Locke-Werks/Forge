#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lwi/error.h"

namespace lwi::stub
{

enum class JournalOp : uint32_t
{
    None = 0,
    FileCreated = 1,  // a: path relative to the install directory
    FileReplaced = 2, // a: relative path, b: relative path of the saved original
    DirCreated = 3,   // a: relative path
};

struct JournalRecord
{
    JournalOp op = JournalOp::None;
    std::wstring a;
    std::wstring b;
};

/// A write-ahead log of file mutations, so a failed install can be undone.
///
/// Records are appended AND FLUSHED BEFORE the mutation they describe, never
/// after. A power loss between the record and the mutation costs one no-op
/// undo; the reverse ordering costs untracked state, which is the failure that
/// leaves a half-installed product nobody can remove.
///
/// Rollback runs from the in-memory copy, because the common case is an error
/// partway through an install that is still running. The on-disk copy is for
/// the uncommon case: a crash, after which the next run finds a journal that
/// was never committed and undoes it before starting.
class Journal
{
  public:
    Status begin(const std::wstring& install_dir);

    /// Appends and flushes. Call before performing the operation.
    Status record(JournalOp op, const std::wstring& a, const std::wstring& b = {});

    /// Undoes every recorded operation in reverse, best effort. A rollback that
    /// hits an error keeps going: leaving nine of ten changes reverted beats
    /// stopping at the first one.
    void rollback();

    /// Discards the saved originals and removes the journal. After this the
    /// install is the state of record.
    Status commit();

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] size_t size() const { return records_.size(); }

    /// Rolls back a journal left behind by a crashed install.
    ///
    /// Returns the number of records undone, or zero when there was nothing to
    /// recover, which is the normal case.
    static size_t recover(const std::wstring& install_dir);

    /// Where the saved original of a replaced file goes. Kept inside the
    /// install directory so the rename that creates it is same-volume, which
    /// is what makes it atomic and what preserves the security descriptor.
    static std::wstring backup_path_for(const std::wstring& relative, uint32_t sequence);

  private:
    std::wstring install_dir_;
    std::wstring journal_path_;
    std::vector<JournalRecord> records_;
    bool active_ = false;
};

} // namespace lwi::stub
