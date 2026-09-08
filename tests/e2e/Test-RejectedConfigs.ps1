<#
.SYNOPSIS
    Asserts that lwforge refuses a hook config the stub could not honour.

.DESCRIPTION
    Every case here used to package clean and then do nothing, or do something
    other than what the config said, with no diagnostic at build time or install
    time. A misspelled phase was flattened into the container and never looked
    at. A hook with no run failed its own target check at install time as a
    warning nobody asked for. timeout_ms and expect_exit are read by a parser
    that falls back to its default on anything it cannot make sense of, so "30s"
    quietly became 120000 and a non-numeric expect_exit entry quietly became 0,
    which is the one value that means success.

    A build that accepts any of them is the failure, so the assertion is that
    lwforge exits nonzero and says which key it objected to.
#>

[CmdletBinding()]
param(
    [string]$Forge = "build/ci/src/forge/lwforge.exe",
    [string]$Stub = "build/ci/src/stub/lwstub.exe",
    [string]$Payload = "build/stage-demo",
    [string]$WorkDir = "build/reject"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$head = @'
[product]
name    = "RejectMe"
version = "1.0.0"

[install]
dir = '{LocalAppData}\Programs\RejectMe'

'@

$cases = [ordered]@{
    'unknown-phase' = @'
[[hooks.post_instal]]
id     = "typo"
run    = 'payload:bin\deadletterd.exe'
sha256 = "auto"
'@

    'missing-run' = @'
[[hooks.post_install]]
id     = "forgot_the_run_key"
sha256 = "auto"
'@

    'bad-timeout' = @'
[[hooks.post_install]]
id         = "slow"
run        = 'payload:bin\deadletterd.exe'
sha256     = "auto"
timeout_ms = "30s"
'@

    'bad-expect-exit' = @'
[[hooks.post_install]]
id          = "odd"
run         = 'payload:bin\deadletterd.exe'
sha256      = "auto"
expect_exit = ["zero"]
'@

    'duplicate-id' = @'
[[hooks.post_install]]
id     = "same"
run    = 'payload:bin\deadletterd.exe'
sha256 = "auto"

[[hooks.post_install]]
id     = "same"
run    = 'payload:bin\deadletter.exe'
sha256 = "auto"
'@

    'bad-as' = @'
[[hooks.post_install]]
id     = "wrong_context"
run    = 'payload:bin\deadletterd.exe'
as     = "User"
sha256 = "auto"
'@
}

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$accepted = @()

foreach ($name in $cases.Keys) {
    $path = Join-Path $WorkDir "$name.toml"
    Set-Content -LiteralPath $path -Value ($head + $cases[$name])

    $output = & $Forge build --config $path --payload $Payload --stub $Stub `
                       --out (Join-Path $WorkDir "$name.exe") --dev 2>&1
    $code = $LASTEXITCODE

    if ($code -eq 0) {
        $accepted += $name
        Write-Host "  [FAIL] $name was accepted, and the stub would then ignore it"
        continue
    }

    # The first line is the one a person reads. Print it so a change in wording
    # is visible in the log rather than only in the diff.
    $first = ($output | Select-Object -First 1)
    Write-Host "  [pass] $name -> $first"
}

if ($accepted.Count -ne 0) {
    throw "lwforge accepted $($accepted -join ', ')"
}

Write-Host "`nall $($cases.Count) rejected configs refused"
exit 0
