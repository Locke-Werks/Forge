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

Expanded wherever a path or argument is accepted.

| Token | Meaning |
|---|---|
| `{InstallDir}` | the resolved install directory |
| `{Product}` | `product.name` |
| `{Version}` | `product.version` |
| `{ProgramFiles}` | `FOLDERID_ProgramFilesX64` |
| `{ProgramData}` | `FOLDERID_ProgramData` |
| `{LocalAppData}` | `FOLDERID_LocalAppData` |
| `{LocalPrograms}` | `FOLDERID_UserProgramFiles` |
| `{Desktop}` | `FOLDERID_Desktop` |

Known folders are resolved through `SHGetKnownFolderPath`, not environment
variables: `%ProgramFiles%` is rewritten by WOW64 and an environment block is
inherited from whoever launched the process.

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
| `elevation` | `on-demand`, `required` | Defaults to `required`, which stamps a `requireAdministrator` manifest: the installer asks for UAC before any UI appears. `on-demand` stamps `asInvoker` instead, so the license is seen before any prompt and per-user work happens in the invoking user's token, but the stub does not elevate itself, so a machine-scope install choosing it must be launched elevated. Any other value fails the build |

## `[ui]`

| Key | Notes |
|---|---|
| `license_text` | Shown on the first page. Acceptance is required to proceed |
| `warning` | Shown in the error colour above the checkbox |
| `theme.*` | Any of `background`, `surface`, `border`, `border_hover`, `accent`, `accent_soft`, `text`, `text_body`, `text_muted`, `text_faint`, `error`, `success` as `#rrggbb` or `#aarrggbb`. Omit the block for the Locke Werks palette |

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
| `shortcut` | `target`, `where` (`common_programs`, `programs`, `desktop`), `name` |
| `registry_write` | `hive`, `key`, `name`, `value`, `value_type` (`sz`, `expand_sz`, `dword`) |
| `env` | `name`, `value`, `value_type` |
| `path_append` | `value` |
| `arp` | Derived from `[product]`; no keys of its own |

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

Phases: `post_extract`, `pre_register`, `post_install`, `pre_uninstall`,
`post_uninstall`.

| Key | Notes |
|---|---|
| `id` | Name used in logs |
| `run` | Must start with `payload:` and name a payload member |
| `args` | Array. Passed as argv, never through a shell |
| `sha256` | `"auto"` makes `lwforge` fill it in. An explicit value is verified and a mismatch fails the build |
| `expect_exit` | Array of acceptable codes. Defaults to `[0]` |
| `timeout_ms` | Defaults to 120000 |
| `vital` | `true` aborts the install on failure. Defaults to `false` |

A hook may only run a payload member whose digest matches. The config is inside
the Authenticode-covered region, so changing either the digest or the binary it
names invalidates the signature. That is the whole reason hooks exist without an
interpreter: the signature vouches for the code, so the code has to be covered
by it.

A digest mismatch at runtime is always fatal, `vital` or not, because it means
the file on disk is not the file the signature vouched for.

Uninstall hooks travel in the install manifest, since the uninstaller has no
container. They are protected by the install directory's ACL rather than by the
signature, which is a real reduction and is stated here rather than glossed.

## `[cli]` and exit codes

Accepted switches: `/S`, `/VERYSILENT`, `/quiet`, `/q` (all fully silent,
matching NSIS and what winget supplies), `/D=`, `/DIR=`, `/uninstall`,
`--check-only`.

| Code | Meaning |
|---|---|
| 0 | Success |
| 1602 | User cancelled, including a declined UAC prompt |
| 1603 | Failure, or a preflight requirement not met |
| 1618 | Another instance is already running |
| 1638 | Another version is installed; used for a refused downgrade |
| 3010 | Success, restart required |

0, 1641 and 3010 are all documented by MSI as success. Deployment tools treat
them that way, so anything reporting otherwise will be misread.

Note that the stub is a GUI-subsystem binary. A shell that does not wait for it
sees a stale exit code; deployment tools always wait, but a hand-run
`installer.exe && echo done` will not.
