# Signing

Azure Artifact Signing, formerly Trusted Signing. The certificate resolves to
**Specter Point Intelligence, LLC**, the parent organisation, which is correct
for a Locke Werks product.

| | |
|---|---|
| Endpoint | `https://eus.codesigning.azure.net` |
| Account | `specterpoint` |
| Certificate profile | `specterpoint` |

These live in `signing/signing.env` and nowhere else. `signing/metadata.json` is
generated from it and is gitignored, and CI fails if it is ever committed.

That is not tidiness. The profile name drifted into three different values
across four repositories that each kept their own copy:

| Repository | Profile |
|---|---|
| MCPmcp, MTGSim | `specterpoint` |
| DeadLetter | `lockewerks-public` (stale) |
| specterpoint | `specterworks-public` |

Consolidating also matters for SmartScreen. EV certificates no longer bypass it,
and publisher reputation accumulates across files signed by the same
certificate, so splitting releases across three profiles splits the reputation
three ways for nothing.

## Order of operations

Signing is always last, because signing is what makes the bytes immutable.
Anything that rewrites the image happens first: resource stamping, then the
container append, then the 8-byte alignment pad, then `signtool`.

For a release there are **two** signing passes, and the order is load-bearing:

1. Build and sign `lwstub.exe`.
2. Build `lwforge`, forge the installer against the **signed** stub, sign the
   result.

`lwforge` embeds the stub it is given as the uninstaller, and that copy is
payload: hashed and extracted, never re-signed on the way out. Signing only the
finished installer therefore leaves an unsigned uninstaller sitting permanently
in Program Files, invoked elevated by Settings.

`lwforge` refuses an unsigned stub unless `--dev` is passed, and
`scripts/package.ps1` runs both stages in order and verifies both artifacts.

```
scripts/package.ps1 -Config examples/deadletter/installer.toml -Payload build/stage-demo
```

## Local signing

Needs three environment variables and the client tools:

```
winget install -e --id Microsoft.Azure.ArtifactSigningClientTools
```

`scripts/sign.ps1` probes both the current install location
(`%LOCALAPPDATA%\Microsoft\ArtifactSigningClientTools`) and the pre-rename one
(`...\MicrosoftArtifactSigningClientTools`), because the rename moved it and a
machine can have either. It discovers `signtool` by scanning the Windows Kits
directory newest-first, excluding SDK 10.0.20348 which Microsoft documents as
unsupported for signing. A pinned SDK path goes stale on the next SDK release
and fails as "not found" rather than as "your SDK moved".

Timestamping is mandatory, not optional. Certificates are valid for **three
days**; without an RFC3161 timestamp the signature stops validating within the
week.

Avoid any Public Trust **Test** profile: those certificates carry EKU
`1.3.6.1.4.1.311.10.3.13`, which forces validators to respect the certificate
lifetime regardless of a valid timestamp.

## CI

This section covers Forge's own release. For wiring Forge into a **different**
product's workflow, see `using-forge-in-ci.md`.

The `sign` job runs only on a `v*` tag, in the `release` environment.

Configuration lives in repository variables and one environment secret:

| Name | Kind | Purpose |
|---|---|---|
| `AZURE_TENANT_ID` | variable | Not sensitive |
| `AZURE_CLIENT_ID` | variable | Not sensitive |
| `AZURE_CLIENT_SECRET` | environment secret (`release`) | The signing credential |

Those three are the whole of it. The service principal is a user principal with
a client secret and is not federated: there is no workload identity, no OIDC
subject, and no GitHub trust relationship to configure. Looking for one when
signing fails has already cost a day against a setup that was never broken.

Every artifact is re-verified with `Get-AuthenticodeSignature` afterwards. The
signing step exiting zero is not the same as the file being signed, and that is
the check which catches a silently skipped folder.

## Releasing

```
git tag v0.3.0
git push origin v0.3.0
```

The tag runs the full build, then signs `lwforge.exe` and `lwstub.exe` and
attaches them to a GitHub release.

## Failure modes worth recognising

| Symptom | Cause |
|---|---|
| 403 at sign time, fine at login | Missing **Artifact Signing Certificate Profile Signer** role at `certificateProfiles/<profile>` scope. Granting it at account scope is not enough |
| `0x80080057` | The file is 4 GB or larger. `lwforge` refuses earlier with a clearer message |
| `tomlplusplus` fails to build in stage 2 only | `vcvars64` overwrote `VCPKG_ROOT` with the vcpkg bundled in Visual Studio. `package.ps1` restores it |
| `Get-AuthenticodeSignature` not found inside `sign.ps1` | A developer prompt rewrote `PSModulePath` and the child shell cannot autoload modules. `sign.ps1` falls back to `signtool verify` |
| Signature `Valid` but the uninstaller `NotSigned` | The stub was not signed before being forged. Use `package.ps1` |
