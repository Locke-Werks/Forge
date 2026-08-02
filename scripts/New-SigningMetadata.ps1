<#
.SYNOPSIS
    Generates signing/metadata.json from metadata.json.in and signing.env.

.DESCRIPTION
    metadata.json is generated rather than committed so that the endpoint,
    account and certificate profile are written down exactly once. Every other
    Locke Werks repository carries a hand-maintained copy, and the profile name
    has drifted into three different values across four of them.

    Values come from signing.env and can be overridden by environment variables
    of the same name, which is how CI supplies a different profile without
    editing a tracked file.
#>

[CmdletBinding()]
param(
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Template = Join-Path $RepoRoot 'signing\metadata.json.in'
$EnvFile  = Join-Path $RepoRoot 'signing\signing.env'
$Output   = Join-Path $RepoRoot 'signing\metadata.json'

foreach ($required in @($Template, $EnvFile)) {
    if (-not (Test-Path $required)) {
        Write-Error "ERROR: missing $required"
        exit 1
    }
}

if ((Test-Path $Output) -and -not $Force) {
    Write-Host "$Output already exists. Pass -Force to regenerate." -ForegroundColor Yellow
    exit 0
}

$values = @{}
foreach ($line in Get-Content $EnvFile) {
    $trimmed = $line.Trim()
    if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
    $split = $trimmed.Split('=', 2)
    if ($split.Count -ne 2) { continue }
    $values[$split[0].Trim()] = $split[1].Trim()
}

# Environment wins, so CI can point at a different profile without a file edit.
foreach ($key in @($values.Keys)) {
    $fromEnv = [Environment]::GetEnvironmentVariable($key)
    if ($fromEnv) { $values[$key] = $fromEnv }
}

$content = Get-Content $Template -Raw
foreach ($key in $values.Keys) {
    $content = $content.Replace("@$key@", $values[$key])
}

if ($content -match '@[A-Z_]+@') {
    Write-Error "ERROR: unsubstituted placeholder remains: $($Matches[0])"
    exit 1
}

Set-Content -Path $Output -Value $content -Encoding UTF8 -NoNewline

Write-Host "Wrote $Output" -ForegroundColor Green
Write-Host "  endpoint $($values['LWI_SIGN_ENDPOINT'])"
Write-Host "  account  $($values['LWI_SIGN_ACCOUNT'])"
Write-Host "  profile  $($values['LWI_SIGN_PROFILE'])"
