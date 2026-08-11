#pragma once

#include <windows.h>

#include <shlobj.h>

#include <string>

#include "lwi/error.h"

namespace lwi::stub
{

/// The account that per-user work should run as.
///
/// An installer that elevated is no longer the person who started it. Over the
/// shoulder UAC is the sharp case: a standard user runs setup, an administrator
/// types credentials, and from that point %USERPROFILE%, HKCU and every per-user
/// known folder belong to the administrator. Anything the installer writes
/// "for the user" lands in the wrong profile. Even when the same person
/// elevated, files created by the elevated token are owned by Administrators
/// rather than by them, so a config file the product expects to rewrite later
/// becomes one it can no longer touch.
///
/// Both are the same bug from two directions, and both are why per-user work has
/// to be handed to a token that is not ours.
class UserContext
{
  public:
    UserContext() = default;
    ~UserContext();

    UserContext(const UserContext&) = delete;
    UserContext& operator=(const UserContext&) = delete;

    /// Resolves the token to run per-user work under.
    ///
    /// Succeeds with a null token() when the installer is already running as the
    /// right person, which is the unelevated case. Callers treat that as "use
    /// the process token" rather than as a second code path.
    ///
    /// Fails when the installer is elevated and there is no interactive user to
    /// hand the work to, which is a deployment tool installing at the login
    /// screen. That is a real state, not an error in the config, so the caller
    /// decides whether it is fatal.
    static Status acquire(UserContext& out);

    [[nodiscard]] HANDLE token() const { return token_; }

    /// Where the token came from: "process", "shell" or "session".
    [[nodiscard]] const char* source() const { return source_; }

    /// DOMAIN\\user for the resolved account, for log lines. Empty if unreadable.
    [[nodiscard]] const std::wstring& account() const { return account_; }

  private:
    HANDLE token_ = nullptr;
    const char* source_ = "process";
    std::wstring account_;
};

/// True when the process token carries the elevation flag. SYSTEM reports true,
/// which is correct: it is not the interactive user either.
bool process_is_elevated();

/// A known folder resolved against a specific user's token, or the current one
/// when token is null.
///
/// SHGetKnownFolderPath takes a token for exactly this reason. Reading
/// %LOCALAPPDATA% out of the environment instead returns whatever the elevated
/// process inherited, which is the wrong profile in the case that matters.
std::wstring known_folder_for(REFKNOWNFOLDERID id, HANDLE token);

struct ProcessResult
{
    bool ran = false;
    DWORD exit_code = 0;
    std::string detail;
};

/// Runs a process to completion, optionally as another user.
///
/// A null token runs it in the current token and environment. Otherwise the
/// child gets that user's environment block, which is the part that actually
/// moves %USERPROFILE% and %APPDATA%. Without it the child runs as the right
/// account with the wrong paths, which is the least useful of the four possible
/// outcomes: it looks like it worked.
///
/// The job object is what makes the timeout mean anything. Killing the child
/// without it leaves its children running, and the installer proceeds while work
/// it believes finished is still in flight.
ProcessResult run_process_as(HANDLE token, const std::wstring& exe,
                             const std::wstring& command_line, const std::wstring& working_dir,
                             DWORD timeout_ms);

} // namespace lwi::stub
