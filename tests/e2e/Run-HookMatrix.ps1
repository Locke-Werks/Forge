<#
.SYNOPSIS
    Installs and uninstalls the demo installer and asserts that every hook phase
    actually ran, in the right context, with its tokens expanded.

.DESCRIPTION
    Hooks had no end-to-end coverage at all. The demo config declared two, one of
    which ran where.exe and left nothing behind, and the other of which ran
    notepad.exe with arguments notepad does not understand: it never exited, was
    terminated by its own 60 second timeout on every single install, and the
    suite recorded that as a pass. Nothing anywhere asserted that a hook had run.

    The payload binaries are hookprobe, which writes a value under
    HKCU\Software\LockeWerks\ForgeHookProbe named after its first argument. That
    is what makes each phase checkable, and HKCU specifically is what makes the
    as = "user" claim checkable: an elevated installer's own HKCU is the
    administrator's hive, so a value arriving in the invoking user's hive is
    evidence the token was dropped.

    Honest limit: run unelevated, the installer and the user are the same
    account, so the as = "user" assertion proves the hook ran and saw the right
    profile but cannot distinguish the two tokens. Run it elevated as well to
    prove that half. CI runs the unelevated form.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [string]$Target = (Join-Path $env:LOCALAPPDATA "Programs\LwiHookTest")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$probeKey = 'HKCU:\Software\LockeWerks\ForgeHookProbe'

function Get-Marker {
    param([string]$Name)

    if (-not (Test-Path -LiteralPath $probeKey)) { return $null }
    $item = Get-ItemProperty -LiteralPath $probeKey
    if ($item.PSObject.Properties.Name -notcontains $Name) { return $null }
    return $item.$Name
}

function Clear-Markers {
    if (Test-Path -LiteralPath $probeKey) {
        Remove-Item -LiteralPath $probeKey -Recurse -Force
    }
}

function Remove-Install {
    param([string]$Path)

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        if (-not (Test-Path -LiteralPath $Path)) { break }
        try {
            Get-ChildItem -LiteralPath $Path -Recurse -Force -File |
                ForEach-Object { $_.Attributes = 'Normal' }
            [IO.Directory]::Delete($Path, $true)
            break
        }
        catch {
            if ($attempt -eq 5) { throw }
            Start-Sleep -Milliseconds (200 * $attempt)
        }
    }

    try {
        [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree(
            'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\DeadLetter')
    } catch {}
    $link = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\DeadLetter.lnk"
    if (Test-Path -LiteralPath $link) { [IO.File]::Delete($link) }
}

$Installer = (Resolve-Path $Installer).Path
$failures = @()

function Assert-Marker {
    param([string]$Name, [string]$Expected, [string]$What)

    $actual = Get-Marker -Name $Name
    if ($null -eq $actual) {
        $script:failures += "$What did not run (no '$Name' marker)"
        Write-Host "  [FAIL] $What : no marker"
        return
    }
    if ($Expected -and $actual -ne $Expected) {
        $script:failures += "$What wrote '$actual', expected '$Expected'"
        Write-Host "  [FAIL] $What : '$actual', expected '$Expected'"
        return
    }
    Write-Host "  [pass] $What : $actual"
}

Remove-Install -Path $Target
Clear-Markers

# ---------------------------------------------------------------------------
# Fresh install
# ---------------------------------------------------------------------------
Write-Host "`n=== Fresh install ==="
$log = Join-Path $env:TEMP "lwi-hook-install.log"
$p = Start-Process -FilePath $Installer -ArgumentList @('/S', "/D=$Target") -Wait -PassThru `
                   -WindowStyle Hidden -RedirectStandardOutput $log
if (Test-Path $log) { Get-Content $log | ForEach-Object { Write-Host "  $_" } }
if ($p.ExitCode -ne 0) { throw "install exited $($p.ExitCode)" }

# pre_install ran before anything else was on disk. On a fresh install
# {PriorVersion} is empty, so the probe falls back to its default data.
Assert-Marker -Name 'pre_install' -Expected 'ok' -What 'pre_install hook'
Assert-Marker -Name 'cmd' -Expected 'ok' -What 'post_install hook (installer context)'

# The per-user hook, with {UserLocalAppData} expanded against the account it ran
# as. Comparing against this session's own LOCALAPPDATA is the assertion: an
# elevated installer that failed to drop its token would write the
# administrator's path here, into the administrator's hive, and this would not
# even find the marker.
$expectedProfile = Join-Path $env:LOCALAPPDATA "DeadLetter"
Assert-Marker -Name '--first-run' -Expected $expectedProfile -What 'post_install hook (user context)'
Assert-Marker -Name '--first-run.localappdata' -Expected $env:LOCALAPPDATA `
              -What 'user hook environment block'

# Nothing was placed twice. The pre_install member is written by the stage and
# skipped by the main loop; writing it again would journal a FileReplaced whose
# saved original is this same install's copy.
$backup = Join-Path $Target ".lw\backup"
if (Test-Path -LiteralPath $backup) {
    $failures += "a fresh install left backups in .lw\backup"
    Write-Host "  [FAIL] .lw\backup exists after a fresh install"
} else {
    Write-Host "  [pass] no backup residue"
}

# ---------------------------------------------------------------------------
# Upgrade over the top
# ---------------------------------------------------------------------------
Write-Host "`n=== Upgrade over the same version ==="

# Taken from Add/Remove Programs rather than hardcoded, so bumping the demo
# config's version does not silently turn these into assertions about a string.
$arp = Get-ItemProperty `
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\DeadLetter' -ErrorAction Stop
$installedName = $arp.DisplayName
$installedVersion = $arp.DisplayVersion
Write-Host "  installed: $installedName $installedVersion"

Clear-Markers
$p = Start-Process -FilePath $Installer -ArgumentList @('/S', "/D=$Target") -Wait -PassThru `
                   -WindowStyle Hidden -RedirectStandardOutput $log
if ($p.ExitCode -ne 0) { throw "upgrade exited $($p.ExitCode)" }

# The point of the phase: on an upgrade {PriorVersion} names what is about to be
# replaced, and it is the hook's only way to tell the two cases apart.
Assert-Marker -Name 'pre_install' -Expected $installedVersion `
              -What 'pre_install saw {PriorVersion}'

# The backup of every replaced file is deleted at commit. A running old image
# would hold its own backup open and that delete would fail silently, which is
# the residue a pre_install hook exists to prevent.
if (Test-Path -LiteralPath $backup) {
    $failures += "an upgrade left backups in .lw\backup"
    Write-Host "  [FAIL] .lw\backup survived the upgrade"
} else {
    Write-Host "  [pass] no backup residue after an upgrade"
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
Write-Host "`n=== Uninstall ==="
Clear-Markers
$uninstaller = Join-Path $Target ".lw\uninstall.exe"
if (-not (Test-Path -LiteralPath $uninstaller)) { throw "no uninstaller at $uninstaller" }

$ulog = Join-Path $env:TEMP "lwi-hook-uninstall.log"
$u = Start-Process -FilePath $uninstaller -ArgumentList @('/uninstall', '/S') -Wait -PassThru `
                   -WindowStyle Hidden -RedirectStandardOutput $ulog
if (Test-Path $ulog) { Get-Content $ulog | ForEach-Object { Write-Host "  $_" } }
if ($u.ExitCode -ne 0) { throw "uninstall exited $($u.ExitCode)" }

Assert-Marker -Name '--remove-profile' -Expected $expectedProfile `
              -What 'pre_uninstall hook (user context)'

# The one that was broken outright: an uninstall hook's {Product} and {Version}
# expanded to empty strings, because the uninstall plan carried neither. They
# come out of the install manifest now, so they name what was removed.
Assert-Marker -Name 'post_uninstall' -Expected "$installedName $installedVersion" `
              -What 'post_uninstall {Product} and {Version}'

Start-Sleep -Seconds 5
if (Test-Path -LiteralPath $Target) {
    $failures += "the install directory survived the uninstall"
    Write-Host "  [FAIL] $Target survived"
} else {
    Write-Host "  [pass] install directory removed"
}

Clear-Markers
Remove-Install -Path $Target

if ($failures.Count -ne 0) {
    Write-Host ""
    foreach ($f in $failures) { Write-Host "FAILED: $f" }
    throw "$($failures.Count) hook assertion(s) failed"
}

Write-Host "`nhook matrix clean: every phase ran and every token expanded"
