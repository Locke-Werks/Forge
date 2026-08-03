<#
.SYNOPSIS
    Sweeps a deliberate failure through every step of an install and asserts the
    machine comes back to exactly where it started.

.DESCRIPTION
    Rollback runs only when something has already gone wrong, on a machine
    nobody is watching, and several of its failure modes are indistinguishable
    from success. Hoping an install fails is not a test, so the engine can be
    told to fail on purpose at a numbered step.

    Two sweeps:

      LWI_FAULT_INJECT  the ordinary unwind. The installer returns an error and
                        undoes its own work before exiting.

      LWI_FAULT_KILL    TerminateProcess instead, which is what a power loss
                        looks like to the filesystem. Nothing unwinds; the
                        journal is left uncommitted, and the NEXT run has to
                        find it and recover.

    Honest limit: this catches deterministic faults, not races. An antivirus
    scanner holding a handle between the rename and the write is not in the
    matrix. The answer there is retry-with-backoff and the delay-until-reboot
    fallback, and it is not exercised here.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [int]$MaxSteps = 12,
    [string]$Target = (Join-Path $env:LOCALAPPDATA "Programs\LwiFaultTest")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-MachineState {
    param([string]$Path)

    $files = @()
    if (Test-Path -LiteralPath $Path) {
        $files = Get-ChildItem -LiteralPath $Path -Recurse -File |
                 Sort-Object FullName |
                 ForEach-Object { "$($_.FullName.Substring($Path.Length)):$($_.Length)" }
    }

    $arp = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\DeadLetter'
    $arpState = if (Test-Path $arp) { 'present' } else { 'absent' }

    $link = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\DeadLetter.lnk"
    $linkState = if (Test-Path -LiteralPath $link) { 'present' } else { 'absent' }

    # Services and associations are watched too. A state snapshot that only
    # covers files and the ARP key cannot see a service left running or an
    # extension left registered, which is exactly the residue rollback is for.
    $svcState = if (Get-Service -Name LwiTestSvc -ErrorAction SilentlyContinue) {
        'present'
    } else {
        'absent'
    }

    $assocState = if (Test-Path 'HKLM:\SOFTWARE\Classes\.lwitest') { 'present' } else { 'absent' }

    [pscustomobject]@{
        Files   = ($files -join '|')
        Arp     = $arpState
        Link    = $linkState
        Service = $svcState
        Assoc   = $assocState
    }
}

function Remove-Everything {
    param([string]$Path)

    # Clear attributes and retry. A recovered install leaves files that were
    # written moments earlier, and an antivirus scanner or an indexer can still
    # be holding one when the next iteration starts. Failing the cleanup would
    # abort the sweep for a reason unrelated to what is being tested.
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

    if (Get-Service -Name LwiTestSvc -ErrorAction SilentlyContinue) {
        & sc.exe delete LwiTestSvc | Out-Null
    }
    foreach ($key in 'SOFTWARE\Classes\.lwitest', 'SOFTWARE\Classes\LockeWerks.LwiTest.1') {
        try { [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($key) } catch {}
    }
}

function Invoke-Install {
    param([string]$Exe, [string]$Dir, [hashtable]$Environment = @{})

    foreach ($key in $Environment.Keys) {
        Set-Item "env:$key" $Environment[$key]
    }
    try {
        $log = Join-Path $env:TEMP "lwi-fault.log"
        $p = Start-Process -FilePath $Exe -ArgumentList @('/S', "/D=$Dir") -Wait -PassThru `
                           -WindowStyle Hidden -RedirectStandardOutput $log
        return $p.ExitCode
    }
    finally {
        foreach ($key in $Environment.Keys) {
            Remove-Item "env:$key" -ErrorAction SilentlyContinue
        }
    }
}

$Installer = (Resolve-Path $Installer).Path
Remove-Everything -Path $Target
$baseline = Get-MachineState -Path $Target
Write-Host ("baseline: arp={0} link={1} svc={2} assoc={3} files='{4}'" -f `
            $baseline.Arp, $baseline.Link, $baseline.Service, $baseline.Assoc, $baseline.Files)

# Control run, before any sweeping.
#
# Without it the matrix passes vacuously whenever the installer fails early for
# an unrelated reason: every step "rolls back cleanly" because nothing was ever
# written. That is exactly what happened the first time this ran, and the sweep
# reported ten clean steps while the installer was failing on its first call.
Write-Host "`n=== Control: an uninjected install must succeed ===" -ForegroundColor Cyan
$controlCode = Invoke-Install -Exe $Installer -Dir $Target
$controlState = Get-MachineState -Path $Target
if ($controlCode -ne 0 -or $controlState.Arp -ne 'present' -or -not $controlState.Files) {
    Write-Host "  exit $controlCode arp=$($controlState.Arp) files='$($controlState.Files)'" `
               -ForegroundColor Red
    Write-Host (Get-Content (Join-Path $env:TEMP "lwi-fault.log") -Raw)
    throw "the control install failed, so every sweep result would be meaningless"
}
Write-Host "  exit 0, $(($controlState.Files -split '\|').Count) files, ARP present"
Remove-Everything -Path $Target

$failures = @()

Write-Host "`n=== Sweep 1: injected error, installer unwinds itself ===" -ForegroundColor Cyan
for ($step = 0; $step -lt $MaxSteps; $step++) {
    Remove-Everything -Path $Target
    $code = Invoke-Install -Exe $Installer -Dir $Target -Environment @{ LWI_FAULT_INJECT = "$step" }
    $state = Get-MachineState -Path $Target

    # Exit 0 means the step number is past the last fault point, so the install
    # ran to completion. That is the end of the sweep, not a failure.
    if ($code -eq 0) {
        Write-Host ("  step {0,2}: beyond the last fault point, install completed" -f $step)
        $lastStep = $step - 1
        break
    }

    $clean = ($state.Files -eq $baseline.Files) -and
             ($state.Arp -eq $baseline.Arp) -and
             ($state.Link -eq $baseline.Link) -and
             ($state.Service -eq $baseline.Service) -and
             ($state.Assoc -eq $baseline.Assoc)

    if ($clean) {
        Write-Host ("  step {0,2}: exit {1,-5} clean" -f $step, $code)
    } else {
        Write-Host ("  step {0,2}: exit {1,-5} RESIDUE arp={2} link={3} svc={4} assoc={5} files='{6}'" -f `
                    $step, $code, $state.Arp, $state.Link, $state.Service, $state.Assoc,
                    $state.Files) -ForegroundColor Red
        $failures += "inject step $step"
    }
}

Write-Host "`n=== Sweep 2: killed mid-install, next run recovers ===" -ForegroundColor Cyan
for ($step = 0; $step -lt $MaxSteps; $step++) {
    Remove-Everything -Path $Target

    # Kill partway through, leaving an uncommitted journal.
    Invoke-Install -Exe $Installer -Dir $Target -Environment @{ LWI_FAULT_KILL = "$step" } | Out-Null

    # A clean run must recover the wreckage and then succeed.
    $code = Invoke-Install -Exe $Installer -Dir $Target
    $state = Get-MachineState -Path $Target

    $recovered = ($code -eq 0) -and ($state.Arp -eq 'present')
    if ($recovered) {
        Write-Host ("  step {0,2}: recovered, install completed" -f $step)
    } else {
        Write-Host ("  step {0,2}: exit {1} arp={2} NOT RECOVERED" -f $step, $code, $state.Arp) `
                   -ForegroundColor Red
        $failures += "kill step $step"
    }
}

Remove-Everything -Path $Target

if ($failures.Count -gt 0) {
    Write-Host "`n$($failures.Count) failures: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}

Write-Host "`nfault matrix clean over $MaxSteps steps, both sweeps" -ForegroundColor Green
