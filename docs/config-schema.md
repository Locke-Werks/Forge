# Config schema

`lwforge` reads a TOML file and flattens it to dotted keys. The stub consumes
the flat form, so all structural work happens at build time and the elevated
binary needs no text parser.

Arrays keep their index, so ordering survives: `[[actions]]` becomes
`actions.0.type`, `actions.1.type`, plus `actions.count`.

## Backslashes

The most common way to get this file wrong.

TOML basic strings process escapes, so `"C:\bin"` is `C`, `:`, **backspace**,
`in`. Nothing complains, and the installer then creates a shortcut or a PATH
entry pointing somewhere that cannot exist.

```toml
target = "{InstallDir}\\bin\\app.exe"   # doubled, correct
target = '{InstallDir}\bin\app.exe'     # literal string, also correct
target = "{InstallDir}\bin\app.exe"     # WRONG: \b is a backspace
```

`lwforge` rejects control characters in config values and names the offender,
so this fails the build rather than shipping.

## Tokens

Three sets, and they are **not** interchangeable. Each is expanded by different
code at a different moment, and a token used outside its set is left in the
string verbatim rather than reported. That is how a service ends up with
`{ProgramData}\Thing\svc.exe` as its literal binary path and fails to start with
a message about a missing file.

### In `install.dir`

These five, and nowhere else.

| Token | Meaning |
|---|---|
| `{ProgramFiles}` | `FOLDERID_ProgramFilesX64` |
| `{ProgramData}` | `FOLDERID_ProgramData` |
| `{LocalAppData}` | `FOLDERID_LocalAppData` |
| `{LocalPrograms}` | `FOLDERID_UserProgramFiles` |
| `{Desktop}` | `FOLDERID_Desktop` |

Known folders are resolved through `SHGetKnownFolderPath`, not environment
variables: `%ProgramFiles%` is rewritten by WOW64 and an environment block is
inherited from whoever launched the process.

### In declarations

Registry `key` and `value`, env `value`, service `binary` and `args`, assoc
`open_command` and `icon`, and hook `args`.

| Token | Meaning |
|---|---|
| `{InstallDir}` | the resolved install directory |
| `{Product}` | `product.name` |
| `{Version}` | `product.version` |

A shortcut's `target` is the exception: it expands `{InstallDir}` and nothing
else. Nothing above resolves a known folder, so build those paths out of
`{InstallDir}` rather than reaching for `{ProgramData}` here.

### In hook `args`

Additionally, and only there. The `User*` five are resolved against the account
**that hook runs as** rather than against the installer's own token. See `as`
under `[[hooks.<phase>]]`.

| Token | Meaning |
|---|---|
| `{UserProfile}` | `FOLDERID_Profile` |
| `{UserAppData}` | `FOLDERID_RoamingAppData` |
| `{UserLocalAppData}` | `FOLDERID_LocalAppData` |
| `{UserDesktop}` | `FOLDERID_Desktop` |
| `{UserPrograms}` | `FOLDERID_Programs` |
| `{PriorVersion}` | The version already installed, or empty on a fresh install |

`{PriorVersion}` is what Add/Remove Programs holds at the moment the hook runs,
which on every install phase is the version about to be replaced: the ARP entry
is not rewritten until later. That is how a hook tells an upgrade from a fresh
install without going and reading the registry itself. It is deliberately empty
on `pre_uninstall` and `post_uninstall`, where the same read would answer with
the version being removed before the ARP key is deleted and with nothing after
it, and one token meaning two things inside one uninstall is worse than a token
that means nothing there.

`{LocalAppData}` and `{UserLocalAppData}` name the same known folder and are not
the same path. The first is expanded once, by the installer, for `install.dir`.
The second is expanded per hook, against that hook's token. In an elevated
install those are two different profiles.

## `[product]`

| Key | Required | Notes |
|---|---|---|
| `name` | yes | Also the Add/Remove Programs key name |
| `version` | yes | Dotted numeric, optional `-tag`. Compared numerically |
| `publisher` | | Defaults to `Locke Werks` |
| `description` | | Shown under the title |
| `copyright` | | Goes into the version resource |
| `url` | | `URLInfoAbout` |
| `icon` | | `.ico` path, relative to the config file |
| `upgrade_code` | | Stable across versions. Keys the single-instance mutex |
| `aumid` | | AppUserModelID. **No version component**, or taskbar pins and toast notifications break on upgrade. 128 chars, no spaces |

## `[install]`

| Key | Values | Notes |
|---|---|---|
| `scope` | `machine`, `user` | Decides HKLM vs HKCU and which known folders are used |
| `dir` | path | Supports tokens. `/D=` overrides it |
| `elevation` | `on-demand`, `required` | Defaults to `required`, which stamps a `requireAdministrator` manifest: the installer asks for UAC before any UI appears. `on-demand` stamps `asInvoker` instead, so the license is seen before any prompt, but the stub does not elevate itself, so a machine-scope install choosing it must be launched elevated. Any other value fails the build |

`on-demand` used to be the only way to get per-user work into the invoking
user's token, at the cost of an installer that could not write to Program Files
unless it was already elevated. That trade is gone: a hook declared
`as = "user"` runs as the real user from an installer that elevated normally.
Choose `required` unless the license has to be read before the UAC prompt.

## `[ui]`

| Key | Notes |
|---|---|
| `license_text` | Shown on the first page. Acceptance is required to proceed |
| `warning` | Shown in the error colour above the checkbox |
| `theme.*` | Any of `background`, `surface`, `border`, `border_hover`, `accent`, `accent_soft`, `text`, `text_body`, `text_muted`, `text_faint`, `error`, `success`, `warning` as `#rrggbb` or `#aarrggbb`. Omit the block for the Locke Werks palette |

## `[[options]]`

Things the person installing can say yes or no to. Shown on a page between the
license and the progress bar, settable from the command line, and recorded in
the install manifest so uninstall gates on the same answers.

| Key | Notes |
|---|---|
| `id` | Referenced by `when`, typed as `/O:<id>=off`, and stored as a manifest key. Letters, digits, underscore and hyphen only |
| `label` | **Required.** The sentence the person installing reads |
| `detail` | Second line, smaller. Say what it will do to their machine |
| `default` | Defaults to `true`. An option a product declares is one it wants |

```toml
[[options]]
id      = "user_config"
label   = "Add the MCP server to my Claude Code config"
detail  = "Writes to your user profile. Runs as you, not as the administrator."
default = true
```

Duplicate ids fail the build, because `/O:<id>=` could then mean two things.

An option nothing gates on is not an error: it still travels into the install
manifest, where a hook can read it. `lwforge` prints a note, because the usual
cause is a rename that only got done on one side.

## `when`

Accepted on `[[actions]]`, `[[services]]`, `[[assoc]]` and every
`[[hooks.<phase>]]`. A comma separated list of option ids, each optionally
prefixed with `!`, all of which must hold.

```toml
when = "user_config"              # only when it is on
when = "!user_config"             # only when it is off
when = "user_config, !portable"   # both conditions
```

Deliberately not an expression language. The stub runs elevated and a parser is
a parser.

An id that was never declared **fails the build**. At runtime it would read as
not selected and the declaration would silently never fire, which is the failure
mode this is meant to prevent, not produce.

`when` is rejected on `[[preflight]]`. A preflight decides whether the machine
qualifies, and that cannot be conditional on a checkbox.

## `[[preflight]]`

Runs in every mode, including silent. A blocking failure exits 1603 without
writing anything; a refused downgrade exits 1638.

| `type` | Keys | Meaning |
|---|---|---|
| `os_build` | `min` | Build number from `RtlGetVersion`, which is not shimmed the way `GetVersionEx` is |
| `free_disk_space` | `bytes` | Measured on the install volume |
| `min_ram` | `bytes` | Physical memory |
| `process_not_running` | `name`, `restart_manager` | `restart_manager = true` marks it recoverable so the user gets an option rather than a dead end |
| `elevation` | | Fails when not elevated |
| `no_reboot_pending` | `blocking` | Warns by default. Blocking on a pending reboot strands people who cannot restart |
| `registry_value` | `hive`, `key`, `name`, `expect` | Omit `name` to test the key. `expect = false` asserts absence |
| `file_exists` | `path`, `expect` | |

Every entry takes `fail`, the message shown to the user.

An unrecognised `type` **fails closed**. The author asked for a guarantee this
build cannot make, and passing silently is the wrong reading.

Refusing to replace a newer version is built in and needs no entry.

## `[[actions]]`

| `type` | Keys |
|---|---|
| `shortcut` | `target`, `where` (`desktop`, or Start Menu by default), `name` |
| `registry_write` | `hive`, `key`, `name`, `value`, `value_type` (`sz`, `expand_sz`, `dword`) |
| `env` | `name`, `value`, `value_type` |
| `path_append` | `value` |
| `arp` | Derived from `[product]`; no keys of its own |

Every entry also takes `when`.

`where` chooses desktop or Start Menu; `install.scope` chooses whose. `desktop`
resolves to `FOLDERID_PublicDesktop` under machine scope and `FOLDERID_Desktop`
under user scope, and anything else, including the `common_programs` default,
resolves to `FOLDERID_CommonPrograms` or `FOLDERID_Programs` the same way.
Writing `where = "programs"` in a machine-scope install does not get a per-user
Start Menu entry; change `install.scope` for that. `name` defaults to
`product.name`.

Everything writable is captured before it is written and restored on uninstall.
Keys the install created are pruned leaf-first, stopping at the deepest ancestor
that already existed, and only while each key is empty.

`path_append` preserves the existing value type. PATH is normally
`REG_EXPAND_SZ`, and rewriting it as `REG_SZ` freezes every `%VAR%` already in
it. A duplicate entry is not added twice.

The shortcut target must be a real, well-formed executable if it is an `.exe`.
`IPersistFile::Save` resolves the target while writing the link, and a file with
an `.exe` extension that is not a valid PE fails the save outright. A target
that does not exist is fine, which is why this only bites once a payload is
present.

## `[[services]]`

Machine scope only. A per-user install with services declared skips them and
says so in the warnings rather than failing.

| Key | Notes |
|---|---|
| `name` | Service name |
| `display_name` | Defaults to `name` |
| `description` | Shown in services.msc |
| `binary` | Supports tokens. Quoted automatically |
| `args` | Array, appended to the binary path |
| `start` | `demand` (default), `auto`, `disabled` |
| `account` | Defaults to LocalSystem |
| `start_now` | Start it after creating it |
| `when` | Gates the service on an option |

The binary path is quoted whether or not it contains a space. An unquoted
service path is the unquoted service path vulnerability: Windows tries
`C:\Program.exe` before `C:\Program Files\...`.

A service that already exists is reconfigured rather than recreated, and is
**not** recorded for deletion, because this install did not create it. Stopping
waits for the service to actually reach STOPPED: `ControlService` returning
success only means the stop was accepted, and deleting a running service marks
it for deletion until every handle closes, which is how a reinstall hits
`ERROR_SERVICE_MARKED_FOR_DELETE` for no visible reason.

## `[[assoc]]`

| Key | Notes |
|---|---|
| `progid` | The ProgID to create |
| `extension` | e.g. `.eml`. Registered through `OpenWithProgids` |
| `friendly` | Display name for the ProgID |
| `icon` | `path,index` |
| `open_command` | Supports tokens. `%1` is the file |
| `app_name` | Enables the Capabilities registration |
| `app_description` | **Required** for Capabilities |
| `when` | Gates the association on an option |

The extension is registered additively through `OpenWithProgids`. The
extension's default value is deliberately not written: that would seize the
association from whatever already owns it, and Windows overrides it on next
launch regardless.

`ApplicationDescription` is required or the application is left out of the
Default Apps UI entirely. Registration is all an installer can do. Windows has
blocked programmatic default-handler changes since the UserChoice hash, enforced
by `UCPD.sys` since 2024, so nothing here tries to claim a default.

`HKCR` is never written directly. Machine scope writes `HKLM\SOFTWARE\Classes`,
per-user writes `HKCU\SOFTWARE\Classes`.

## `[[hooks.<phase>]]`

| Phase | When |
|---|---|
| `pre_install` | before any payload file is written. See below |
| `post_extract` | files are in place, nothing registered yet |
| `pre_register` | last chance before the registry and shortcuts are touched |
| `post_install` | everything is done |
| `pre_uninstall` | before anything is removed |
| `post_uninstall` | after the registry, services and shortcuts are gone, before the files are deleted |

A phase name that is not one of those six **fails the build**, naming the ones
that exist. A misspelled table would otherwise be packaged into the container,
never validated, never digest-resolved and never run, with nothing said about it
at build time or install time.

| Key | Notes |
|---|---|
| `id` | Name used in warnings. Two hooks in one phase sharing an id fails the build |
| `run` | **Required.** Must start with `payload:` and name a payload member |
| `as` | `installer` (default) or `user`. See below |
| `when` | Gates the hook on an option |
| `args` | Array. Passed as argv, never through a shell. Tokens are expanded |
| `sha256` | `"auto"` makes `lwforge` fill it in. An explicit value is verified and a mismatch fails the build |
| `expect_exit` | Array of acceptable codes. Defaults to `[0]`. Each must be a whole number |
| `timeout_ms` | Defaults to 120000. Must be a positive whole number |
| `vital` | `true` aborts the install on failure. Defaults to `false`. On an uninstall phase it fails the exit code instead, see below |

`expect_exit` and `timeout_ms` are checked at build time because both are read
at install time by a parser that falls back to its default on anything it cannot
make sense of. `timeout_ms = "30s"` would quietly have become 120000 and a
non-numeric `expect_exit` entry would quietly have become `0`, which is the one
value that means success.

A hook may only run a payload member whose digest matches. The config is inside
the Authenticode-covered region, so changing either the digest or the binary it
names invalidates the signature. That is the whole reason hooks exist without an
interpreter: the signature vouches for the code, so the code has to be covered
by it.

A digest mismatch at runtime is always fatal, `vital` or not, because it means
the file on disk is not the file the signature vouched for.

`post_uninstall` runs before the payload files are deleted, because the hook is
one of them. Everything registered is already gone by then, which is what the
phase name promises.

### `pre_install`

The only phase that runs before the payload replaces what is already installed,
which makes it the only place a product can stop its own running copy before the
files under it are swapped. Without it an upgrade leaves every client talking to
the old image out of its renamed backup until something restarts it, and the
running image holds that backup open so it cannot be cleaned up either.

It works by placing the hook's own binary at its final path first, journaled like
any other write, and then running it before anything else is extracted. So the
hook still executes from the install directory that every other phase executes
from: the objection that ruled out running before extraction was to a
world-writable temp directory, and that still stands. What changes is the
moment, not the location or its ACL. A rollback removes the early copy, or
restores the original it displaced, exactly as it does for every other file.

**A `pre_install` hook must depend on nothing but the operating system and
itself.** The rest of the payload is not on disk yet, and on an upgrade the copy
that is there belongs to the version being replaced. A hook is a separate
process, so the stub's own DLL search hardening does not cover it and it resolves
imports starting with its own directory: a payload-supplied dependency is missing
on a fresh install and is the previous version's on an upgrade. A single
self-contained executable qualifies. Anything that ships its runtime beside it
does not, and belongs in `post_extract`.

`{PriorVersion}` is empty on a fresh install, so a hook can do nothing when
there is nothing to stop.

One interaction to know about: a `process_not_running` preflight blocks the
install before any hook runs. If a `pre_install` hook exists to stop the same
process, the preflight will refuse the install first and the hook will never get
the chance. Pick one.

### `as = "user"`

Runs the hook as the person who started the installer, unelevated, in their own
environment. Use it for anything that writes into a user profile.

An installer that elevated is no longer the person who started it, in two ways
that produce the same bug:

- Over-the-shoulder UAC. A standard user runs setup, an administrator types
  credentials, and from that point `%USERPROFILE%`, `HKCU` and every per-user
  known folder belong to the administrator. Work meant for the user lands in the
  wrong profile entirely.
- Ownership. Even when the same person elevated, a file created by the elevated
  token is owned by `BUILTIN\Administrators`, not by them. A config file the
  product expects to rewrite later is one it can no longer touch.

`as = "user"` fixes both. The token comes from the shell, so it is the desktop
owner's and it is their filtered, medium-integrity token. The child gets that
user's environment block, which is what actually moves `%USERPROFILE%` and
`%APPDATA%`; without it a hook runs as the right account with the wrong paths,
which is the worst of the four outcomes because it looks like it worked.

Under a deployment tool running as SYSTEM there is no shell to borrow from, so
the console session's token is asked for directly. If nobody is logged in, there
is no user to hand the work to: the hook is skipped and reported as a warning,
or fails the install if it is `vital`. That is a real machine state, not a broken
config, and an installer at the login screen has to decide which it wants.

The digest is checked by the elevated process before the token is dropped. A
per-user hook clears exactly the same bar as any other; all that changes is who
ends up owning what it writes.

Anything else fails the build. A mistyped `as` that fell back to `installer`
would silently run elevated, which is the outcome the key exists to avoid.

### How a hook is run

The working directory is the install directory, in every phase.

The environment is the installer's own for `as = "installer"`, and the account's
own for `as = "user"`, built from that user's token. That second part is what
actually moves `%USERPROFILE%` and `%APPDATA%`; without it a hook runs as the
right account with the wrong paths.

There is no shell anywhere in the path. `run` names the executable and `args` is
passed as argv, so nothing in the config can be read as an operator, a redirect
or a second command.

The hook and everything it starts run inside a job object. On `timeout_ms` the
whole tree is terminated and the hook is reported as failed with code 258
(`WAIT_TIMEOUT`); that is not an exit code the hook chose, so listing 258 in
`expect_exit` does not make a timeout succeed.

### What a failed hook does, and does not, undo

A rollback reverses files, registry values, environment entries, services,
shortcuts and file associations, because the installer recorded each of those as
it made them. It cannot reverse what a hook did: the hook is an opaque signed
binary and the engine has no idea what it touched. A `vital` hook failing after
an earlier hook has already changed the machine leaves that earlier change in
place.

That is worth knowing when deciding what belongs in a hook and what belongs in a
declarative `[[actions]]` entry, which does have an exact undo.

### Where a failure is reported

An install prints every failed hook as a warning: to the console in silent mode,
and on the final page of the wizard, which says the install completed with
warnings rather than reporting a clean finish. A `vital` failure is a failed
install and gets the error page and exit code 1603 instead.

An uninstall reports the same way, to the console in silent mode and in one
dialog interactively.

### Uninstall hooks

They travel in the install manifest, since the uninstaller has no container.
They are protected by the install directory's ACL rather than by the signature,
which is a real reduction and is stated here rather than glossed.

The options as they were answered travel with them, so a `when` on an uninstall
hook is evaluated against what the user actually chose rather than against the
defaults. `{Product}` and `{Version}` are the ones recorded at install time, so
they name what is being removed. Both are empty for an install performed by
v0.3.0 or earlier, which did not record them.

**An uninstall never stops for a failed hook, `vital` or not.** Leaving somebody
unable to remove a product because the product's own cleanup code is broken is
worse than removing it with the cleanup half done. What `vital` does on these two
phases is fail the uninstaller's exit code, 1603 after the removal has finished,
so a deployment tool can tell a clean removal from a dirty one. Everything is
still gone.

A digest mismatch stops the rest of that phase, as it does on an install. That is
reachable here without any tampering: the manifest pins the bytes as they were at
install time, so a product that replaces its own binaries in place, or an
antivirus that quarantines and restores one, invalidates the pin. The uninstall
reports both the mismatch and that the hooks after it did not run.

## Command line and exit codes

Switches, not config: there is no `[cli]` table. They are listed here because the
option ids in `/O:` come from `[[options]]` above.

| Switch | Meaning |
|---|---|
| `/S`, `/VERYSILENT`, `/quiet`, `/q` | Fully silent, matching NSIS and what winget supplies |
| `/D=`, `/DIR=` | Install directory, overriding `install.dir` |
| `/O:<id>=<value>` | Set an option. `1/0`, `true/false`, `on/off`, `yes/no` |
| `--option <id>=<value>` | The same thing, spelled out |
| `/uninstall` | Uninstall |
| `--check-only` | Report and exit without writing anything |

`--check-only` lists the effective option set after the command line, which is
the only place a deployment tool can confirm that the `/O:` switches it passed
were spelled the way this version of the installer spells them.

An option id the installer does not declare **fails with 1603 before anything is
written**, and the message lists the ids it does declare. Ignoring a typo is the
worst outcome available: the operator believes they turned something off, the
default stays on, and the only evidence is on machines nobody is looking at.

The stub returns exactly these five, borrowed from MSI so Intune, SCCM and winget
read them without a custom mapping:

| Code | Meaning |
|---|---|
| 0 | Success |
| 1602 | User cancelled, including a declined UAC prompt |
| 1603 | Failure, a preflight requirement not met, or an uninstall whose `vital` hook failed |
| 1618 | Another instance is already running |
| 1638 | Another version is installed; used for a refused downgrade |

1603 covers a failed install, a machine that does not qualify, and an uninstall
that removed everything but could not finish its cleanup, so a caller that needs
to tell them apart has to read the message. The one split worth having is carved
out already: a refused downgrade gets 1638 rather than being buried in 1603. A
sixth code was not invented for the uninstall case, because these five are what
Intune, SCCM and winget already read without a custom mapping, and a hook the
config itself called vital failing is a failure by that config's own account.

That last case is the one to know about when something retries on failure: the
product is gone, so a retried uninstall finds nothing to remove.

There is no reboot-required path yet, so 3010 is never returned. It is still
worth knowing, because MSI documents 0, 1641 and 3010 all as success and a hook
may well exit 3010 to mean "done, restart needed": list it in that hook's
`expect_exit` or the hook is treated as failed.

Note that the stub is a GUI-subsystem binary. A shell that does not wait for it
sees a stale exit code; deployment tools always wait, but a hand-run
`installer.exe && echo done` will not.
