# Forge

LockeWerks Forge. A universal, extensible Windows installer: one signed stub,
compiled once, plus a packaging tool that stamps a product's configuration and
payload into a copy of it. Building an installer needs no C++ toolchain.

## What it is

Three pieces:

- `lwi_core` is a static library with no third-party dependencies: PE layout
  parsing, the container format, SHA-256 over CNG, and LZMS compression through
  the in-box Windows Compression API.
- `lwstub.exe` is the installer. Direct2D and DirectWrite, owner-drawn, x64,
  zero third-party dependencies.
- `lwforge.exe` reads a TOML config and a payload directory and writes a signed,
  single-file installer.

The stub reads a compact binary blob the forge produced. It never parses text,
so the elevated signed binary carries no text parser and links nothing from
vcpkg.

## Per-user work from an elevated installer

Most of an install needs administrator rights. Some of it must not have them: a
config file written into a user's profile by an elevated process ends up owned
by `BUILTIN\Administrators`, and under over-the-shoulder UAC it ends up in the
administrator's profile instead of the user's. Both are the same bug, and both
are silent.

A hook declared `as = "user"` runs unelevated as the person who started the
installer, in their own environment, from the same elevated install. Its binary
is digest-pinned and checked before the token is dropped, so lowering privilege
does not lower the bar. `[[options]]` and `when` decide which of them run.

See `docs/config-schema.md`.

## How packaging works

Order matters, because signing is what makes the bytes immutable:

1. Copy the stub.
2. Replace the icon, version resource and manifest with `UpdateResource`.
3. Append the container.
4. Pad the file to an 8-byte multiple.
5. Sign.

Data appended past the last section but before the certificate table is covered
by the Authenticode digest. That was verified by recomputing the digest of a
real signed binary two ways, not assumed. Signing first and appending second
would leave the payload outside the hash, which is the CVE-2013-3900 shape and
is what this design must not do.

At runtime the stub reads the security data directory to find where the
certificate table begins, and looks for its footer immediately before it. That
directory entry's `VirtualAddress` is a file offset rather than an RVA, which is
the usual bug in self-extracting code.

See `docs/container-format.md` for the byte layout.

## Build

Requires Visual Studio 2022, CMake 3.28 or newer, and `VCPKG_ROOT` set.

```
cmake --preset vs
cmake --build build/vs --config Debug
ctest --test-dir build/vs -C Debug --output-on-failure
```

`--preset vs` uses the Visual Studio generator and works from any shell. The
`dev` and `ci` presets use Ninja and must be run from an x64 Native Tools
prompt, because Ninja needs `cl.exe` already on PATH. The `release-stage1` and
`release-stage2` presets are the two-pass signing build; `scripts/package.ps1`
drives both in order and sets the environment up itself.

`/Qspectre` is applied only when the Spectre-mitigated libraries are installed.
They are not part of a default Visual Studio Community installation; configure
reports whether they were found.

## Signing

Azure Artifact Signing, formerly Trusted Signing. Certificates are valid for
three days, so timestamping is mandatory rather than optional.

```
scripts/sign.ps1 -FilePath dist/Setup.exe
```

Needs `AZURE_TENANT_ID`, `AZURE_CLIENT_ID` and `AZURE_CLIENT_SECRET`, and the
client tools from `winget install -e --id Microsoft.Azure.ArtifactSigningClientTools`.
The script probes both the current and the pre-rename install locations, and
discovers signtool by scanning the Windows Kits directory rather than pinning an
SDK version.

That is the local path. To produce a signed installer from another product's
GitHub Actions workflow, see `docs/using-forge-in-ci.md`, which has the whole
recipe and a list of the things people wrongly conclude are blocking it.

## License

MIT. Deliberately permissive: the stub ships inside installers for products
under several different licenses, and a copyleft stub would constrain that.
