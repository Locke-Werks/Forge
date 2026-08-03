<#
.SYNOPSIS
    Two-stage release build: sign the stub, then forge and sign the installer.

.DESCRIPTION
    The order is the point.

    lwforge embeds the stub it is given as the uninstaller, and that copy is
    payload: it is hashed and extracted, not re-signed on the way out. Signing
    only the finished installer therefore leaves an UNSIGNED uninstaller sitting
    permanently in Program Files, invoked elevated by Settings. So the stub is
    signed first, and the installer second.

    Payload staging happens after the build, never before. DeadLetter shipped
    signed installers that failed at the copy step because the build cleared the
    output directory the payload had already been staged into, and nothing
    checked. See DeadLetter/scripts/package.ps1 lines 39-46.

.PARAMETER Config
    The product's installer.toml.

.PARAMETER Payload
    Directory whose contents become the installed files.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Config,
    [Parameter(Mandatory = $true)][string]$Payload,
    [string]$Out = "dist",
    [switch]$SkipSigning
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Enter-DeveloperEnvironment {
    # The release presets use Ninja, which needs cl.exe on PATH. Requiring the
    # operator to remember to open an "x64 Native Tools" prompt is how a release
    # script fails with "configure failed" and nothing else, which is exactly
    # what happened the first time this ran.
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe not found. Install Visual Studio 2022 with the C++ workload."
    }

    $install = & $vswhere -latest -products * `
                          -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                          -property installationPath
    if (-not $install) {
        throw "no Visual Studio installation with the C++ x64 toolset was found"
    }

    $vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw "vcvars64.bat not found at $vcvars"
    }

    # vcvars64 sets VCPKG_ROOT to the copy of vcpkg bundled inside Visual
    # Studio, silently replacing whichever one the machine is configured to use.
    # The presets resolve their toolchain file through $env{VCPKG_ROOT}, so
    # importing the developer environment wholesale switches vcpkg instances
    # halfway through a release build.
    #
    # It surfaces as a port failing to build rather than as anything about
    # vcpkg: stage 1 needs no ports and succeeds, then stage 2 reports
    # "tomlplusplus build failure" from a vcpkg nobody chose.
    $originalVcpkgRoot = $env:VCPKG_ROOT

    # Run it in cmd and import the resulting environment, since a batch file
    # cannot change this process's environment on its own.
    & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item "env:$($Matches[1])" $Matches[2]
        }
    }

    if ($originalVcpkgRoot) {
        $env:VCPKG_ROOT = $originalVcpkgRoot
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "the developer environment did not take: cl.exe is still not on PATH"
    }

    # vcvars64 does NOT put Ninja on PATH. It ships inside the Visual Studio
    # CMake extension, which the developer prompt never adds, so the release
    # presets fail with "unable to find a build program corresponding to Ninja"
    # even from a correctly set up x64 Native Tools prompt. vcpkg fails first
    # and louder, reporting a port build failure that has nothing to do with the
    # port.
    #
    # Discovered rather than pinned, for the same reason signtool is.
    if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        $ninja = Get-ChildItem -LiteralPath $install -Filter ninja.exe -Recurse `
                               -ErrorAction SilentlyContinue |
                 Select-Object -First 1
        if (-not $ninja) {
            throw "ninja.exe not found under $install. Install the C++ CMake tools component."
        }
        $env:PATH = "$($ninja.DirectoryName);$env:PATH"
    }

    Write-Host "developer environment: $install" -ForegroundColor DarkGray
    Write-Host "ninja:       $((Get-Command ninja.exe).Source)" -ForegroundColor DarkGray
    Write-Host "VCPKG_ROOT:  $env:VCPKG_ROOT" -ForegroundColor DarkGray
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {
    Enter-DeveloperEnvironment

    foreach ($required in @($Config, $Payload)) {
        if (-not (Test-Path -LiteralPath $required)) {
            throw "not found: $required"
        }
    }

    if (-not $SkipSigning) {
        foreach ($name in 'AZURE_TENANT_ID', 'AZURE_CLIENT_ID', 'AZURE_CLIENT_SECRET') {
            if (-not [Environment]::GetEnvironmentVariable($name)) {
                throw "$name is not set. Set the three signing variables, or pass -SkipSigning."
            }
        }
        & "$PSScriptRoot/New-SigningMetadata.ps1" -Force | Out-Null
    }

    Write-Host "--- Stage 1: build the stub ---" -ForegroundColor Cyan
    cmake --preset release-stage1
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    cmake --build build/release-stage1
    if ($LASTEXITCODE -ne 0) { throw "stub build failed" }

    $stub = "build/release-stage1/src/stub/lwstub.exe"
    if (-not (Test-Path -LiteralPath $stub)) { throw "stub not produced at $stub" }

    if (-not $SkipSigning) {
        Write-Host "`n--- Stage 1b: sign the stub ---" -ForegroundColor Cyan
        # Before it is embedded, so the uninstaller left on the customer's
        # machine carries a signature of its own.
        & "$PSScriptRoot/sign.ps1" -FilePath $stub
        if ($LASTEXITCODE -ne 0) { throw "signing the stub failed" }
    }

    Write-Host "`n--- Stage 2: build lwforge ---" -ForegroundColor Cyan
    cmake --preset release-stage2
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    cmake --build build/release-stage2
    if ($LASTEXITCODE -ne 0) { throw "forge build failed" }

    $forge = "build/release-stage2/src/forge/lwforge.exe"
    if (-not (Test-Path -LiteralPath $forge)) { throw "lwforge not produced at $forge" }

    # Assert the payload is non-empty only now, after every build step that
    # could have cleared a directory has already run.
    $count = (Get-ChildItem -LiteralPath $Payload -Recurse -File | Measure-Object).Count
    if ($count -eq 0) { throw "payload directory is empty: $Payload" }
    Write-Host "`npayload: $count files" -ForegroundColor DarkGray

    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    $product = (Select-String -Path $Config -Pattern '^\s*name\s*=\s*"([^"]+)"' |
                Select-Object -First 1).Matches.Groups[1].Value
    $installer = Join-Path $Out "$product-Setup.exe"

    Write-Host "`n--- Stage 3: forge ---" -ForegroundColor Cyan
    $forgeArgs = @('build', '--config', $Config, '--payload', $Payload, '--stub', $stub,
                   '--out', $installer)
    if (-not $SkipSigning) { $forgeArgs += '--sign' } else { $forgeArgs += '--dev' }
    & $forge @forgeArgs
    if ($LASTEXITCODE -ne 0) { throw "forge failed" }

    if (-not $SkipSigning) {
        Write-Host "`n--- Verify ---" -ForegroundColor Cyan
        # Both artifacts, because the one that ends up on the customer's disk
        # permanently is the uninstaller, not the installer.
        $sig = Get-AuthenticodeSignature -FilePath $installer
        if ($sig.Status -ne 'Valid') { throw "installer signature is $($sig.Status)" }
        Write-Host "installer:   $($sig.Status)"

        $stubSig = Get-AuthenticodeSignature -FilePath $stub
        if ($stubSig.Status -ne 'Valid') { throw "embedded uninstaller is $($stubSig.Status)" }
        Write-Host "uninstaller: $($stubSig.Status)"
        Write-Host "signer:      $($sig.SignerCertificate.Subject)"
    }

    Write-Host "`n$installer" -ForegroundColor Green
}
finally {
    Pop-Location
}
