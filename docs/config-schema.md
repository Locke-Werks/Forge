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
| `elevation` | `on-demand`, `required` | `on-demand` stamps an `asInvoker` manifest so the license is seen before any UAC prompt, and per-user work happens in the invoking user's token. `required` stamps `requireAdministrator` |

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
