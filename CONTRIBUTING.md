# Contributing

Forge produces a signed binary that runs elevated on other people's machines and
executes code it extracts from itself. Most of what follows is about the
invariants that make that safe, because they are the parts that look like
ordinary code and are not.

## Building

Needs Visual Studio 2022 with the C++ workload, CMake 3.28 or newer, and
`VCPKG_ROOT` set. `doctest` and `toml++` come from vcpkg in manifest mode.

```powershell
cmake --preset vs
cmake --build build/vs --config Debug
ctest --test-dir build/vs -C Debug --output-on-failure
```

`vs` uses the Visual Studio generator and works from any shell. `dev` and `ci`
use Ninja and **must** run from an x64 Native Tools prompt, because Ninja will
not find `cl.exe` on its own: from an ordinary shell they fail with
`Cannot open include file: 'string'`, which reads like a broken toolchain and is
a missing environment.

`ci` is what CI builds and it turns on `/WX`, so build it before opening a pull
request rather than finding out from the runner.

`release-stage1` and `release-stage2` are the two-pass signing build. Nothing
but `scripts/package.ps1` should drive them; it sets up its own environment and
runs them in order.

`/Qspectre` is applied only when the Spectre-mitigated libraries are installed.
They are not in a default Visual Studio Community installation, and configure
reports whether it found them.

## Layout

| Path | What it is | May depend on |
|---|---|---|
| `src/core` | `lwi_core`: PE layout, container format, SHA-256 over CNG, LZMS, the flat `Config` | Win32 only |
| `src/stub` | `lwstub.exe`: the installer, and the uninstaller, which is the same binary | `lwi_core`, Win32, Direct2D/DirectWrite |
| `src/forge` | `lwforge.exe`: reads TOML and a payload directory, writes an installer | `lwi_core`, `toml++` |
| `tests` | doctest unit tests, plus `hookprobe` and the end-to-end scripts | `lwi_core`, doctest |

**The stub never parses text.** `lwforge` parses TOML and flattens it into a
compact binary blob; the stub decodes that blob. This is the reason the elevated
signed binary links nothing from vcpkg, and it is not a detail to trade away for
convenience. A new config key is a new dotted key in the flat `Config`, not a
new parser in the stub.

## Conventions

- C++20, `/W4 /permissive-`, four spaces, braces on their own line.
- `snake_case` for functions and variables, `PascalCase` for types,
  `kConstantCase` for constants, trailing underscore for private members.
  Everything lives in `namespace lwi` or `namespace lwi::stub`.
- Every call that can fail gets its result checked. `Status` carries the message;
  `win32_message` and `hresult_message` turn a code into something a person can
  act on. An error a user sees should name what was being attempted, not just
  what the API returned.
- Comments explain why, not what. If a line looks wrong until you know a Win32
  rule, write the rule down. If a line exists because two other approaches were
  tried and failed, write that down: several comments in this repository are the
  only record of a full day.
- No build timestamps. `__DATE__`, `__TIME__` and `__TIMESTAMP__` fail CI.

### Commits

Imperative mood, concise subject line, body only when the change needs
explanation. No emoji, no trailers.

### Pull requests

Change code and the strings that code needs. Release notes, changelogs, version
numbers and contributor lists belong to the maintainer at release time.

## Invariants

Each of these has a comment at its site. They are collected here because every
one of them is silent when broken, and several were broken once already.

**Append, then sign. Never sign, then append.** Data appended past the last
section but before the certificate table is inside the Authenticode digest.
Reversing the order leaves the payload outside the hash, which is the
CVE-2013-3900 shape. See `docs/container-format.md`.

**The journal is write-ahead.** `journal.record(...)` is called and flushed
*before* the mutation it describes, never after. A power loss between the record
and the mutation costs one no-op undo; the other order costs untracked state,
which is a half-installed product nobody can remove.

**A hook's digest is checked by the elevated process, before any token is
dropped.** `as = "user"` changes who runs the binary, never what may be run. Do
not move the check to the other side of the de-elevation.

**`long_path` normalises before it prefixes.** The `\\?\` prefix disables Win32
path normalisation, so a `..` or a forward slash surviving into a prefixed path
is `ERROR_INVALID_NAME`. `normalize_path` runs first and `tests/test_path.cpp`
pins the cases. This was a real bug twice; do not add a second place that builds
a prefixed path by hand.

**`kHookPhases` in `src/forge/main.cpp` and `phase_key` in `src/stub/hooks.cpp`
must list the same phases.** The forge validates and digest-resolves only what
is in its array, and the stub runs only what its function names. A phase in one
and not the other is a hook that packages clean and never runs, which is exactly
what `validate_hook_phases` now refuses to let happen.

**`backup_sequence` is shared across every caller of `place_member`.**
`Journal::backup_path_for` keys the backup directory on it, and the rename
deliberately does not pass `MOVEFILE_REPLACE_EXISTING`. Two callers counting
separately makes the second rename fail outright.

**The `pre_written` skip list is not an optimisation.** Placing a member twice
journals a `FileReplaced` whose saved original is the copy this same install just
made, so a rollback restores the new file over itself.

**A `when` that cannot be evaluated does less, not more.** The runtime reads an
undeclared option as not selected, and `lwforge` refuses to build a config that
contains one. The first half is only safe because of the second.

## Verifying a change

`ctest` is the cheapest layer and covers the least. Four things run above it,
and all four run in CI. Run the ones your change touches, locally, and say which
you ran.

```powershell
cmake --preset ci; cmake --build build/ci; ctest --preset ci
./scripts/New-DemoPayload.ps1
./build/ci/src/forge/lwforge.exe build `
  --config examples/deadletter/installer.toml `
  --payload build/stage-demo `
  --stub build/ci/src/stub/lwstub.exe `
  --out dist/DeadLetter-Setup.exe --dev
```

**`tests/fault/Run-FaultMatrix.ps1`** sweeps a deliberate failure through every
step of an install, twice: once returning an error so the installer unwinds
itself, and once with `TerminateProcess` so the *next* run has to recover an
uncommitted journal. Rollback runs only when something has already gone wrong, on
a machine nobody is watching, and several of its failure modes look exactly like
success. Any change to `run_install`, the journal, or the order of operations
needs this green.

**`tests/e2e/Run-HookMatrix.ps1`** installs, upgrades over the top and
uninstalls, asserting that each hook phase ran, that its tokens expanded, that
the per-user hook landed in the invoking user's hive with that user's
environment, and that no backup residue survived. Run it elevated as well when
you touch `usercontext.cpp`: unelevated, the installer and the user are the same
account, so the half of `as = "user"` that matters is not exercised.

**`tests/e2e/Test-RejectedConfigs.ps1`** asserts `lwforge` refuses the hook
configs it should. Add a case whenever you add a check.

**Tampering detection** is one step in `.github/workflows/ci.yml`: flip a byte in
a forged installer and it must refuse to run.

A local caveat that will waste your time otherwise: the demo config declares a
`process_not_running` preflight for `deadletter.exe`. On a machine with the real
DeadLetter installed and running, every one of these refuses to install, exactly
as designed. Forge a copy of the config with that preflight pointed at a name
nothing uses rather than closing the product.

"Should work" is not a verification. State what you ran.

## Releasing

1. Bump `cmake/LwiVersion.cmake`, which is the single source of truth: the
   project version, the version resource and the container's META record all
   derive from it.
2. Update the version note in `docs/using-forge-in-ci.md` if the release adds or
   changes a config key. A consuming repo pinned to an older tag does not fail
   the build on a key that release never heard of, it packages it and drops it,
   so that note is the only thing standing between an author and a silently
   inert config.
3. Commit, tag `vX.Y.Z`, push the tag.
4. CI builds, runs everything above, signs `lwforge.exe` and `lwstub.exe` with
   Azure Artifact Signing, verifies both signatures and publishes the release.

`lwstub.exe` ships signed because consuming repositories embed it as their
uninstaller verbatim. That is what saves every consumer a signing pass, and it is
why the release assets are the supported way to get the tools. Building from
source produces an unsigned stub.
