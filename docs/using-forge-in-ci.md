# Using Forge in another project's GitHub CI

This document is for an agent or engineer wiring Forge into a product repository
that is not Forge itself. It describes the signed release path end to end.

Read the next section before concluding that any part of this does not work.

## Status: this works, and here is the proof

Signing a Windows release with Forge on a GitHub-hosted runner is not
theoretical and is not blocked on anything. It has run green.

| | |
|---|---|
| Run | [30780619380](https://github.com/Locke-Werks/Forge/actions/runs/30780619380), tag `v0.1.0`, 2026-08-03 |
| Job | `sign`, 1m06s, success |
| Runner | `windows-latest`, GitHub-hosted |
| Auth | client secret, three values, no OIDC |
| Result | `Number of files successfully Signed: 2`, `Number of errors: 0` |
| Verify | `lwforge.exe: Valid`, `lwstub.exe: Valid` |
| Signer | `CN="Specter Point Intelligence, LLC", O="Specter Point Intelligence, LLC", L=Colorado Springs, S=Colorado, C=US` |
| Release | https://github.com/Locke-Werks/Forge/releases/tag/v0.1.0 |

If signing fails in a consuming repository, the cause is in that repository's
configuration. It is not a limitation of Forge, of Azure Artifact Signing, or of
GitHub Actions. Go to the failure table at the bottom.

## What has to be signed, and in what order

A Forge installer contains three classes of binary, and each is signed at a
different point. Getting the order wrong is the single most common way to ship
an installer that is signed but contains unsigned files.

1. **The payload.** Your product's own binaries. Forge hashes these into the
   container and extracts them verbatim. Nothing about signing the installer
   reaches them. Sign them **before** staging.
2. **The stub, which becomes the embedded uninstaller.** `lwforge` adds the stub
   it is handed as an ordinary payload member at `.lw\uninstall.exe`. That copy
   is hashed and extracted, never re-signed on the way out. It is signed only if
   the stub handed in was already signed. Use the stub from the Forge release,
   which is signed, and this is already done.
3. **The finished installer.** Signed last, after resource stamping and after
   the container is appended, because the signature is what makes the bytes
   immutable.

`lwforge` refuses an unsigned stub unless `--dev` is passed, so step 2 fails the
build rather than shipping an unsigned uninstaller into Program Files where
Settings invokes it elevated.

### Why signing last does not orphan the container

The design rests on a property of the Authenticode digest that was verified
against the signing whitepaper rather than assumed. The extra-data region runs
from `SUM_OF_BYTES_HASHED` for `file_size - (cert_table_size + SUM_OF_BYTES_HASHED)`
bytes, so data appended past the last section but before the certificate table
is **inside** the hash.

Appending the container and then signing puts the payload under the signature.
Signing first and appending second would leave it outside, which is the
CVE-2013-3900 shape. Forge does the former. See `src/core/lwi/pe_layout.h`.

The reader finds the container by scanning back from `payload_end`, which is the
certificate table offset on a signed image and the file size on an unsigned one,
tolerating up to 8 bytes of padding signtool may insert. A signed installer
reads its own container correctly. Verify it anyway, per the workflow below.

## Getting lwforge and lwstub

`Locke-Werks/Forge` is a **private** repository, so `${{ github.token }}` cannot
read its releases from another repository. Pick one:

**Preferred: download the signed release assets with a token.** Create a
fine-grained PAT or GitHub App installation token with `Contents: read` on
`Locke-Werks/Forge` and store it in the consuming repo as `FORGE_TOKEN`. You get
`lwstub.exe` already signed, which removes an entire signing pass.

Pin the tag rather than tracking latest, so a Forge release cannot change what a
product's release job produces without anyone choosing it. The workflow below
puts the tag in one `env` value for that reason.

**Alternative: vendor the two binaries.** Commit the signed `lwforge.exe` and
`lwstub.exe` into the consuming repo under `tools/`. No token, no network. The
cost is binaries in git and a manual bump when Forge releases.

**Last resort: build Forge from source.** This produces an **unsigned** stub, so
you must add a signing pass on the stub before forging. See the last section.
Do not choose this to avoid a token.

### Which version has what

`[[options]]`, `when`, and `as = "user"` on a hook need **v0.3.0 or later**.
Pinning v0.2.0 or earlier does not fail the build and does not warn. `lwforge`
flattens the keys into the container like any others and skips the validation
that would reject them; the stub then never reads them. The result is not the
feature switched off, it is the feature inverted:

| Key | What a pre-options build does |
|---|---|
| `when` | Ignored, so the action, service, assoc or hook runs **unconditionally** |
| `/O:<id>=off` | Ignored rather than rejected, including ids that do not exist |
| `as = "user"` | Ignored, so the hook runs **elevated**, in the wrong profile |

Each of those is the exact outcome the key was added to prevent, arriving by way
of the version pin. A ticked-by-default option is the least of it: an unticked
one still fires, and per-user work still lands in the administrator's profile
owned by `BUILTIN\Administrators`.

If the product's config uses any of them, pin the Forge release that contains
them and confirm with `--check-only`, which lists the option set a given
installer actually declares and prints no `options` block at all on a build that
has no option support.

## The workflow

Copy this. It is the whole path: build, sign payload, forge, sign installer,
verify, publish.

```yaml
name: release

on:
  push:
    tags: ['v*']

env:
  # Pin it. One place to bump, and a Forge release cannot silently change what
  # this job produces. See the version note under "Getting lwforge and lwstub".
  FORGE_VERSION: v0.3.0

jobs:
  release:
    runs-on: windows-latest
    environment: release
    permissions:
      contents: write        # to publish the release
    steps:
      - uses: actions/checkout@v4

      # 1. Build the product however this repo builds it.
      - name: Build
        run: |
          cmake --preset release
          cmake --build build/release

      # 2. Stage the payload. Everything under here lands on the customer's
      #    disk byte for byte.
      - name: Stage payload
        shell: pwsh
        run: |
          New-Item -ItemType Directory -Force -Path stage/bin | Out-Null
          Copy-Item build/release/myapp.exe stage/bin/

      # 3. Sign the payload BEFORE forging. Forge hashes these into the
      #    container and extracts them verbatim, so an unsigned binary going in
      #    stays unsigned on the installed machine. Signing the finished
      #    installer does nothing for them.
      - name: Sign the payload
        uses: azure/trusted-signing-action@v0
        with:
          azure-tenant-id: ${{ vars.AZURE_TENANT_ID }}
          azure-client-id: ${{ vars.AZURE_CLIENT_ID }}
          azure-client-secret: ${{ secrets.AZURE_CLIENT_SECRET }}
          endpoint: https://eus.codesigning.azure.net
          trusted-signing-account-name: specterpoint
          certificate-profile-name: specterpoint
          files-folder: stage
          files-folder-filter: exe,dll
          files-folder-recurse: true
          file-digest: SHA256
          # Certificates are valid for three days. Without the timestamp the
          # signature stops validating within the week. Not optional.
          timestamp-rfc3161: http://timestamp.acs.microsoft.com
          timestamp-digest: SHA256

      # 4. Fetch the signed toolchain. lwstub.exe is already signed in the
      #    release, which is what makes the embedded uninstaller signed without
      #    a second signing pass in this repository.
      - name: Fetch Forge
        shell: pwsh
        env:
          GH_TOKEN: ${{ secrets.FORGE_TOKEN }}
        run: |
          gh release download $env:FORGE_VERSION --repo Locke-Werks/Forge `
            --pattern '*.exe' --dir tools
          foreach ($name in 'lwforge.exe', 'lwstub.exe') {
            $sig = Get-AuthenticodeSignature "tools/$name"
            "$name : $($sig.Status)"
            if ($sig.Status -ne 'Valid') { throw "tools/$name is $($sig.Status)" }
          }

      # 5. Forge. No --sign here: that flag shells out to scripts/sign.ps1
      #    resolved against the working directory, which exists only in the
      #    Forge repo. Signing is a separate step below.
      - name: Forge the installer
        shell: pwsh
        run: |
          ./tools/lwforge.exe build `
            --config installer.toml `
            --payload stage `
            --stub tools/lwstub.exe `
            --out dist/MyApp-Setup.exe

      # 6. Sign the installer last, after resource stamping and the container
      #    append have already rewritten the image.
      - name: Sign the installer
        uses: azure/trusted-signing-action@v0
        with:
          azure-tenant-id: ${{ vars.AZURE_TENANT_ID }}
          azure-client-id: ${{ vars.AZURE_CLIENT_ID }}
          azure-client-secret: ${{ secrets.AZURE_CLIENT_SECRET }}
          endpoint: https://eus.codesigning.azure.net
          trusted-signing-account-name: specterpoint
          certificate-profile-name: specterpoint
          files-folder: dist
          files-folder-filter: exe
          file-digest: SHA256
          timestamp-rfc3161: http://timestamp.acs.microsoft.com
          timestamp-digest: SHA256

      - name: Verify
        shell: pwsh
        run: |
          $sig = Get-AuthenticodeSignature dist/MyApp-Setup.exe
          "installer: $($sig.Status)  $($sig.SignerCertificate.Subject)"
          if ($sig.Status -ne 'Valid') { throw "installer is $($sig.Status)" }

          # Signing appended a certificate table after the container. This is
          # the check that proves it did not orphan the payload, and it is the
          # one people leave out.
          ./tools/lwforge.exe inspect dist/MyApp-Setup.exe
          if ($LASTEXITCODE -ne 0) {
            throw "the signed installer cannot read its own container"
          }

      - name: Publish
        shell: pwsh
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          gh release create "${{ github.ref_name }}" dist/MyApp-Setup.exe `
            --repo "${{ github.repository }}" `
            --title "MyApp ${{ github.ref_name }}"
```

## Options and per-user hooks in an unattended install

Two things in `installer.toml` change what an unattended deployment does, and
both are worth checking in the release job rather than discovering on a fleet.

**Options.** `[[options]]` are answered from their `default` when nobody is at
the keyboard, and overridden with `/O:<id>=off`. An id the installer does not
declare fails with 1603 before anything is written, so a rename between versions
breaks the deployment loudly instead of quietly reverting to the default. Add
the switches your product ships with to the verify step:

```yaml
      - name: Verify the option surface
        shell: pwsh
        run: |
          # Lists the effective option set after the command line. This is the
          # check that catches a /O: switch the installer no longer recognises.
          $report = & dist/MyApp-Setup.exe --check-only /O:desktop_shortcut=off 2>&1 | Out-String
          $report
          if ($LASTEXITCODE -ne 0) { throw "check-only exited $LASTEXITCODE`n$report" }
```

Print the report and put it in the thrown message, because the exit code alone
does not say which of two things went wrong. `--check-only` returns 1603 both
for an option id the installer does not declare and for a preflight the runner
fails, and a runner failing `free_disk_space` or `process_not_running` is a
property of the runner rather than a defect in the installer. The text
distinguishes them; the code does not.

The option check happens first, before any preflight runs and before anything is
written, so a typo costs nothing and reports itself immediately.

**Per-user hooks.** A hook declared `as = "user"` runs as the interactive user.
Under Intune or SCCM the installer runs as SYSTEM, and at the login screen there
is no interactive user to hand the work to. The hook is skipped and reported as
a warning, or fails the install if it is marked `vital`.

That is a real machine state, not a misconfiguration, and it is a design
decision for the product rather than something CI can fix. If the per-user step
must happen for a machine-wide deployment, either mark it `vital` and accept
that installs at the login screen fail, or have the product do that work on
first run and use the hook only for the interactive case.

**Signing still applies to hook binaries.** A hook can only run a payload
member. Payload members are extracted verbatim, so a hook binary is signed only
if it was signed before staging. Signing the installer does nothing for it. This
is the same rule as everything else in the payload, but a hook is a new reason
to be shipping an executable that nobody thought of as a product binary.

## One-time setup per consuming repository

No new Azure work. The account, the certificate profile and the role assignment
already exist and are shared. You are copying three values.

| Name | Kind | Value |
|---|---|---|
| `AZURE_TENANT_ID` | repository **variable** | not sensitive |
| `AZURE_CLIENT_ID` | repository **variable** | not sensitive |
| `AZURE_CLIENT_SECRET` | environment **secret** on `release` | the secret |
| `FORGE_TOKEN` | repository secret | PAT with `Contents: read` on `Locke-Werks/Forge` |

```
gh variable set AZURE_TENANT_ID  --repo OWNER/NAME --body "..."
gh variable set AZURE_CLIENT_ID  --repo OWNER/NAME --body "..."
gh secret   set AZURE_CLIENT_SECRET --repo OWNER/NAME --env release
gh secret   set FORGE_TOKEN --repo OWNER/NAME
```

The tenant and client IDs and the secret are in
`C:\Users\vexam\projects\azure signing creds.txt`. The service principal already
holds **Artifact Signing Certificate Profile Signer** at the
`certificateProfiles/specterpoint` scope. Do not create a second app
registration, a second account, or a second certificate profile.

Splitting across profiles actively costs you. SmartScreen publisher reputation
accumulates per certificate, and EV certificates no longer bypass SmartScreen,
so three profiles split the reputation three ways for nothing. The profile name
has already drifted into three different values across four repositories.

## Claims that are false

Every item below has been asserted in a previous session and is wrong. If you
are about to say one of these, stop.

**"This needs OIDC / a federated credential / `azure/login`."**
No. Auth is a user principal with a client secret: `AZURE_TENANT_ID`,
`AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`, passed as three action inputs. There
is no workload identity, no OIDC subject, and no GitHub trust relationship to
configure, and none is planned. Run 30780619380 signed two binaries this way.
Hunting for a federated setup has already cost a full day against a
configuration that was never broken. Do not propose it as a fix, do not add
`id-token: write`, and do not add an `azure/login` step.

**"`AZURE_SUBSCRIPTION_ID` is missing, so signing cannot work."**
Signing does not read it. It is not part of this pipeline.

**"`azure/trusted-signing-action` is deprecated, so this is broken."**
Microsoft renamed Trusted Signing to Artifact Signing in January 2026 and moved
the action to `Azure/artifact-signing-action`, now at v2. GitHub redirects the
old reference and `azure/trusted-signing-action@v0` resolved and signed
successfully on 2026-08-03. A rename is not an outage. Migration is optional and
is two lines, described at the bottom.

**"The publisher says Specter Point Intelligence, LLC, so it signed with the
wrong certificate."**
That is correct and expected. Specter Point Intelligence, LLC is the parent
organisation. A Locke Werks product signed by it is right.

**"The runner needs the Artifact Signing client tools installed."**
The action installs its own copy under
`%LOCALAPPDATA%\TrustedSigning\Microsoft.Trusted.Signing.Client\<version>\bin\x64\`
and generates its own `metadata.json`. Do not add a `winget install` step. Do
not call `scripts/New-SigningMetadata.ps1`. Do not commit `signing/metadata.json`
to the consuming repo.

**"`signing/metadata.json` and its `ExcludeCredentials` list must be present."**
Only for **local** signing through `scripts/sign.ps1`. In CI the action builds
its own metadata and exposes the equivalents as `exclude-*-credential` inputs.
The chain resolves to `EnvironmentCredential` because the action exports the
three variables. Both facts are true; they apply to different paths.

**"A self-hosted runner, an HSM, a .pfx or an EV certificate is required."**
None of them. `windows-latest` is sufficient. There is no key material anywhere
in the pipeline; the signing service holds it.

**"Signing will corrupt the appended container."**
It does not. Appended data before the certificate table is inside the
Authenticode digest by design, and the reader locates the container relative to
the certificate table. The `lwforge inspect` step in the workflow proves it per
release rather than assuming it.

**"Just run `scripts/package.ps1` in CI."**
It will not work. It calls `sign.ps1`, which probes
`%LOCALAPPDATA%\Microsoft\ArtifactSigningClientTools\` and the pre-rename
variant of that path. Neither exists on a GitHub runner, and neither is where
the action puts the dlib. `package.ps1` is the local release path.

**"Pass `--sign` to `lwforge`."**
Only inside the Forge repo. `run_sign` in `src/forge/main.cpp` launches
`powershell.exe -File "scripts\sign.ps1"` with no working directory override, so
the path resolves against the current directory. In a consuming repo that file
does not exist and the build fails with `signing failed`, which reads like a
certificate problem and is not one. Sign with the action instead.

## Verification you must not delete

Three checks, each of which has caught a real failure:

1. `Get-AuthenticodeSignature` on every artifact after the signing step. The
   action exiting zero is not the same as the file being signed, and this is
   what catches a silently skipped folder or a wrong `files-folder-filter`.
2. `Get-AuthenticodeSignature` on `lwstub.exe` before forging. If the stub is
   unsigned the uninstaller will be too, permanently, in Program Files.
3. `lwforge inspect` on the **signed** installer. Confirms the container is
   still readable after the certificate table was appended.

## Failure table

| Symptom | Cause | Fix |
|---|---|---|
| `signing failed` from `lwforge` | `--sign` passed outside the Forge repo | Drop `--sign`, sign with the action |
| `Azure.CodeSigning.Dlib.dll not found` | `sign.ps1` or `package.ps1` called on a runner | Use the action |
| 403 at sign time, login fine | Missing **Artifact Signing Certificate Profile Signer** at `certificateProfiles/specterpoint` scope | Grant the role at profile scope, not account scope |
| `missing signing configuration` | Variable set at repo scope but secret expected on the `release` environment, or the job has no `environment: release` | Match the job's environment to where the secret lives |
| Installer `Valid`, uninstaller `NotSigned` | Forged against an unsigned stub | Use the release `lwstub.exe`, or sign the stub first |
| Payload files unsigned on disk after install | Payload staged after signing, or never signed | Sign the staging directory before forging |
| `the stub is unsigned` from `lwforge` | Working as designed | Sign the stub, or pass `--dev` for a non-shipping build |
| `0x80080057` | The file is 4 GB or larger | `lwforge` refuses earlier with a clearer message |
| Signature valid today, invalid next week | `timestamp-rfc3161` omitted | Certificates last three days; timestamping is mandatory |
| Job never starts | `environment: release` has required reviewers | Approve the deployment, or drop the gate |
| Exit 1603, nothing written, message names an option | A `/O:` switch the installer does not declare | Run `--check-only` to list the ids this version declares |
| A `when` fires unconditionally, `/O:` is ignored instead of rejected, or an `as = "user"` hook runs elevated | `FORGE_VERSION` pinned to v0.2.0 or earlier, which predates `[[options]]` | Bump the pin; `--check-only` prints no `options` block on a build with no option support |
| `when names "x", which is not a declared option` | An option was renamed on one side only | Fix the `when`, or restore the id |
| Per-user hook warned as skipped on a fleet | Installed as SYSTEM with nobody logged in | Expected. Do that work on first run, or mark the hook `vital` and accept the failure |
| Files land in the wrong profile, or owned by Administrators | The hook is missing `as = "user"` | Add it; the elevated token is not the user's |

## If you build Forge from source instead

You get an unsigned stub, so you need two signing passes and the order matters.
Build with `release-stage1`, sign `lwstub.exe`, build with `release-stage2`,
forge against the signed stub, then sign the installer. That is what
`scripts/package.ps1` does locally. In CI, replace its `sign.ps1` calls with
`azure/trusted-signing-action` steps.

Prefer the release assets. This path exists for changes to Forge itself.

## Migrating to artifact-signing-action@v2

Optional. The v0 reference still resolves and signs.

```yaml
- uses: azure/artifact-signing-action@v2
  with:
    signing-account-name: specterpoint     # was trusted-signing-account-name
    certificate-profile-name: specterpoint
    endpoint: https://eus.codesigning.azure.net
    # every other input is unchanged
```

`trusted-signing-account-name` still works and is deprecated in favour of
`signing-account-name`. The Azure roles were renamed the same way: **Trusted
Signing Certificate Profile Signer** is now **Artifact Signing Certificate
Profile Signer**. Same assignment, same scope, no action needed.
