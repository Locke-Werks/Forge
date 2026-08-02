<#
.SYNOPSIS
    Signs a file with Azure Artifact Signing (formerly Trusted Signing).

.PARAMETER FilePath
    Path to the file to sign.

.NOTES
    Certificates issued by Artifact Signing are valid for three days, so the
    RFC3161 timestamp is mandatory rather than optional: without it the binary
    stops validating within the week.
#>

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$FilePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$MetadataJson = Join-Path $RepoRoot 'signing\metadata.json'

if (-not (Test-Path $MetadataJson)) {
    $template = Join-Path $RepoRoot 'signing\metadata.json.in'
    $envFile  = Join-Path $RepoRoot 'signing\signing.env'
    Write-Error @"
ERROR: $MetadataJson does not exist.

It is generated rather than committed, so the certificate profile lives in one
place. Across the other repositories this value drifted into three different
answers (specterpoint, lockewerks-public, specterworks-public) precisely because
each one carried its own copy.

Run: scripts\New-SigningMetadata.ps1
Template: $template
Overrides: $envFile
"@
    exit 1
}

# The client tools moved when Trusted Signing was renamed to Artifact Signing.
# Probe both rather than pinning either: a machine can have the old install, the
# new one, or both.
$DlibCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Microsoft\ArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll'),
    (Join-Path $env:LOCALAPPDATA 'Microsoft\MicrosoftArtifactSigningClientTools\Azure.CodeSigning.Dlib.dll')
)
$Dlib = $DlibCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $Dlib) {
    Write-Error @"
ERROR: Azure.CodeSigning.Dlib.dll not found. Looked in:
$($DlibCandidates -join "`n")

Install with: winget install -e --id Microsoft.Azure.ArtifactSigningClientTools
"@
    exit 1
}

# Discovered newest first rather than pinned. A pinned SDK version goes stale on
# the next SDK release and fails as "not found" rather than as "your SDK moved",
# which is the trap the sibling script in the specterpoint repo still has.
# 10.0.20348 is excluded: Microsoft documents it as unsupported for signing.
$SignTool = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory `
    -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^10\.' -and $_.Name -notmatch '^10\.0\.20348' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1

if (-not $SignTool) {
    Write-Error "ERROR: signtool.exe not found. Install the Windows SDK signing tools."
    exit 1
}

foreach ($envVar in @('AZURE_TENANT_ID', 'AZURE_CLIENT_ID', 'AZURE_CLIENT_SECRET')) {
    if (-not [Environment]::GetEnvironmentVariable($envVar)) {
        Write-Error "ERROR: Environment variable $envVar is not set."
        exit 1
    }
}

if (-not (Test-Path $FilePath)) {
    Write-Error "ERROR: Not found: $FilePath"
    exit 1
}

$resolved = (Resolve-Path $FilePath).Path
Write-Host "--- Signing $resolved ---" -ForegroundColor Cyan
Write-Host "    signtool: $SignTool"
Write-Host "    dlib:     $Dlib"

& $SignTool sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 `
    /dlib $Dlib /dmdf $MetadataJson $resolved
if ($LASTEXITCODE -ne 0) {
    Write-Error "ERROR: signtool sign failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "`n--- Verifying ---" -ForegroundColor Cyan
& $SignTool verify /pa /v $resolved
if ($LASTEXITCODE -ne 0) {
    Write-Error "ERROR: signtool verify failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

# signtool exiting zero is not the same as the file being signed. DeadLetter's
# CI learned this the expensive way and checks the same thing afterwards.
$signature = Get-AuthenticodeSignature -FilePath $resolved
if ($signature.Status -ne 'Valid') {
    Write-Error "ERROR: signature status is $($signature.Status), expected Valid"
    exit 1
}

Write-Host "`nSigned by: $($signature.SignerCertificate.Subject)" -ForegroundColor Green
