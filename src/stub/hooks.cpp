#include "hooks.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>

#include "lwi/hash.h"
#include "lwi/win_file.h"
#include "usercontext.h"

namespace lwi::stub
{
namespace
{

constexpr DWORD kDefaultTimeoutMs = 120000;

/// Rejects anything that is not a plain relative path inside the payload.
///
/// The "payload:" prefix is required rather than optional so a config cannot
/// name an arbitrary file on the machine by accident or otherwise. Everything
/// after it is treated as data, never as a command line.
bool resolve_hook_target(const std::string& spec, const std::wstring& install_dir,
                         std::wstring& out, std::string& why)
{
    constexpr std::string_view kPrefix = "payload:";
    if (spec.size() <= kPrefix.size() || spec.compare(0, kPrefix.size(), kPrefix) != 0)
    {
        why = "hook target must start with payload:";
        return false;
    }

    std::string relative = spec.substr(kPrefix.size());
    for (char& c : relative)
    {
        if (c == '/')
        {
            c = '\\';
        }
    }

    if (relative.empty() || relative.front() == '\\' ||
        (relative.size() >= 2 && relative[1] == ':'))
    {
        why = "hook target must be a relative payload path";
        return false;
    }

    size_t start = 0;
    while (start <= relative.size())
    {
        const size_t end = relative.find('\\', start);
        const std::string_view segment = std::string_view(relative).substr(
            start, (end == std::string::npos ? relative.size() : end) - start);
        if (segment == ".." || segment == ".")
        {
            why = "hook target contains a relative segment";
            return false;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }

    out = install_dir + L"\\" + to_wide(relative);
    return true;
}

std::wstring quote_argument(const std::wstring& value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos)
    {
        return value;
    }
    std::wstring out = L"\"";
    for (size_t i = 0; i < value.size(); ++i)
    {
        size_t backslashes = 0;
        while (i < value.size() && value[i] == L'\\')
        {
            ++backslashes;
            ++i;
        }
        if (i == value.size())
        {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (value[i] == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
        }
        else
        {
            out.append(backslashes, L'\\');
        }
        out.push_back(value[i]);
    }
    out.push_back(L'"');
    return out;
}

/// The values the {Token} placeholders in a hook's arguments stand for.
///
/// Resolved per context, not once per install. The User* folders always name the
/// account the hook itself runs as, so the same argument text means the
/// administrator's profile in an installer-context hook and the person at the
/// keyboard's in a user-context one. That is the honest reading, and it is why
/// writing per-user state needs `as = "user"` rather than a clever path.
struct Expansion
{
    std::wstring install_dir;
    std::wstring version;
    std::wstring product;
    std::wstring user_profile;
    std::wstring user_local_appdata;
    std::wstring user_appdata;
    std::wstring user_desktop;
    std::wstring user_programs;
};

Expansion make_expansion(const InstallPlan& plan, HANDLE token)
{
    Expansion out;
    out.install_dir = plan.install_dir;
    out.version = plan.version;
    out.product = plan.product;
    out.user_profile = known_folder_for(FOLDERID_Profile, token);
    out.user_local_appdata = known_folder_for(FOLDERID_LocalAppData, token);
    out.user_appdata = known_folder_for(FOLDERID_RoamingAppData, token);
    out.user_desktop = known_folder_for(FOLDERID_Desktop, token);
    out.user_programs = known_folder_for(FOLDERID_Programs, token);
    return out;
}

std::wstring expand(const std::wstring& text, const Expansion& values)
{
    struct Token
    {
        const wchar_t* name;
        const std::wstring* value;
    };
    const Token tokens[] = {
        {L"{InstallDir}", &values.install_dir},
        {L"{Version}", &values.version},
        {L"{Product}", &values.product},
        {L"{UserProfile}", &values.user_profile},
        {L"{UserLocalAppData}", &values.user_local_appdata},
        {L"{UserAppData}", &values.user_appdata},
        {L"{UserDesktop}", &values.user_desktop},
        {L"{UserPrograms}", &values.user_programs},
    };

    std::wstring out = text;
    for (const Token& token : tokens)
    {
        size_t pos = out.find(token.name);
        while (pos != std::wstring::npos)
        {
            out.replace(pos, wcslen(token.name), *token.value);
            pos = out.find(token.name, pos + token.value->size());
        }
    }
    return out;
}

} // namespace

const char* phase_key(Phase phase)
{
    switch (phase)
    {
    case Phase::PostExtract:
        return "hooks.post_extract";
    case Phase::PreRegister:
        return "hooks.pre_register";
    case Phase::PostInstall:
        return "hooks.post_install";
    case Phase::PreUninstall:
        return "hooks.pre_uninstall";
    case Phase::PostUninstall:
        return "hooks.post_uninstall";
    }
    return "hooks.unknown";
}

bool run_hooks(Phase phase, const Config& config, const InstallPlan& plan,
               std::vector<HookOutcome>& outcomes)
{
    const std::string base = phase_key(phase);
    const size_t count = config.array_size(base);

    // Both are built on first use. An installer with no per-user hooks never
    // reaches for another token, and one with no hooks at all resolves no known
    // folders.
    Expansion installer_values;
    bool installer_values_built = false;

    UserContext user;
    Status user_status = Status::ok();
    bool user_attempted = false;
    Expansion user_values;
    bool user_values_built = false;

    for (size_t i = 0; i < count; ++i)
    {
        const std::string prefix = base + "." + std::to_string(i) + ".";

        if (!when_satisfied(config, prefix, plan))
        {
            continue;
        }

        HookOutcome outcome;
        outcome.id = std::string(config.get(prefix + "id", prefix));
        outcome.vital = config.get_bool(prefix + "vital", false);
        outcome.context =
            config.get(prefix + "as") == "user" ? HookContext::User : HookContext::Installer;

        // Resolving the context first, because a per-user hook on a machine with
        // nobody logged in cannot run at all and there is no point hashing a
        // binary for it.
        HANDLE token = nullptr;
        if (outcome.context == HookContext::User)
        {
            if (!user_attempted)
            {
                user_attempted = true;
                user_status = UserContext::acquire(user);
            }
            if (!user_status)
            {
                // The usual cause is a deployment tool installing at the login
                // screen. Real state, not a broken config: the product is
                // installed, and the per-user part of it has not happened yet.
                outcome.detail = user_status.message();
                outcomes.push_back(outcome);
                if (outcome.vital)
                {
                    return false;
                }
                continue;
            }
            token = user.token();
            outcome.account = to_utf8(user.account());

            if (!user_values_built)
            {
                user_values = make_expansion(plan, token);
                user_values_built = true;
            }
        }
        else if (!installer_values_built)
        {
            installer_values = make_expansion(plan, nullptr);
            installer_values_built = true;
        }

        const Expansion& values =
            outcome.context == HookContext::User ? user_values : installer_values;

        const std::string spec = std::string(config.get(prefix + "run"));
        std::wstring exe;
        std::string why;
        if (!resolve_hook_target(spec, plan.install_dir, exe, why))
        {
            outcome.detail = why;
            outcomes.push_back(outcome);
            if (outcome.vital)
            {
                return false;
            }
            continue;
        }

        // The digest is not optional. Without it the config could point at any
        // payload member and the signature would still cover the config, so the
        // pinning is what ties a hook to one specific set of bytes.
        //
        // Checked here, by the elevated process, before any token is dropped. A
        // per-user hook clears exactly the same bar as an installer-context one;
        // all that changes is who ends up owning what it writes.
        Sha256 expected{};
        const std::string digest_text = std::string(config.get(prefix + "sha256"));
        if (!from_hex(digest_text, expected))
        {
            outcome.detail = "hook has no valid sha256";
            outcomes.push_back(outcome);
            if (outcome.vital)
            {
                return false;
            }
            continue;
        }

        Sha256 actual{};
        if (Status s = sha256_file(exe, actual); !s)
        {
            outcome.detail = "cannot hash hook binary: " + s.message();
            outcomes.push_back(outcome);
            if (outcome.vital)
            {
                return false;
            }
            continue;
        }

        if (actual != expected)
        {
            outcome.detail = "hook binary does not match its pinned digest";
            outcomes.push_back(outcome);
            // Always fatal, vital or not. A digest mismatch means the file on
            // disk is not the file the signature vouched for.
            return false;
        }

        // Build the command line from an explicit argv. There is no shell
        // anywhere in this path, so nothing in the config can be interpreted as
        // an operator, a redirect, or a second command.
        std::wstring command = quote_argument(exe);
        const size_t arg_count = config.array_size(prefix + "args");
        for (size_t a = 0; a < arg_count; ++a)
        {
            const std::wstring argument =
                expand(to_wide(config.get(prefix + "args." + std::to_string(a))), values);
            command += L" " + quote_argument(argument);
        }

        const DWORD timeout =
            static_cast<DWORD>(config.get_int(prefix + "timeout_ms", kDefaultTimeoutMs));

        const ProcessResult process =
            run_process_as(token, exe, command, plan.install_dir, timeout);
        outcome.ran = process.ran;
        outcome.exit_code = process.exit_code;
        outcome.detail = process.detail;

        if (outcome.ran)
        {
            // An explicit list of acceptable codes, defaulting to zero. Some
            // tools report "done, restart needed" as 3010 and that is a success.
            std::vector<int64_t> accepted;
            const size_t expect_count = config.array_size(prefix + "expect_exit");
            for (size_t e = 0; e < expect_count; ++e)
            {
                accepted.push_back(
                    config.get_int(prefix + "expect_exit." + std::to_string(e), 0));
            }
            if (accepted.empty())
            {
                accepted.push_back(0);
            }

            outcome.ok = std::find(accepted.begin(), accepted.end(),
                                   static_cast<int64_t>(outcome.exit_code)) != accepted.end();
            if (!outcome.ok)
            {
                outcome.detail = "exited with " + std::to_string(outcome.exit_code);
            }
        }

        outcomes.push_back(outcome);

        if (!outcome.ok && outcome.vital)
        {
            return false;
        }
    }

    return true;
}

} // namespace lwi::stub
