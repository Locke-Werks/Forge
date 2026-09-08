<#
.SYNOPSIS
    Builds a throwaway payload directory for exercising the forge.

.DESCRIPTION
    The binaries are real executables rather than files with an "MZ" prefix and
    filler text.

    That distinction is not cosmetic. IPersistFile::Save resolves a shortcut's
    target while writing the link, and a file named .exe that is not a valid PE
    makes it fail with a bare E_FAIL. A fixture built from fake executables
    therefore produces an installer whose shortcuts silently never appear, and
    the failure looks like an installer bug rather than a fixture bug.

    They are hookprobe rather than borrowed system binaries, because the payload
    is also what the config's hooks run. A hook has to exit, and it has to leave
    something a test can read back, or "the hook ran" is an assumption rather
    than an assertion.
#>

[CmdletBinding()]
param(
    [string]$Path = "build/stage-demo",
    [string]$Probe = "build/ci/tests/hookprobe/hookprobe.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The two executables are hooks as well as shortcut targets, so they have to be
# something that exits and leaves evidence. This used to be notepad.exe, which
# never exits: every install spent the hook's full 60 second timeout being
# terminated, and the suite counted that as a pass.
if (-not (Test-Path -LiteralPath $Probe)) {
    throw "hookprobe not found at $Probe. Build first, or pass -Probe."
}

if (Test-Path -LiteralPath $Path) {
    Remove-Item -LiteralPath $Path -Recurse -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $Path "bin") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Path "share/qml") | Out-Null

Copy-Item $Probe (Join-Path $Path "bin/deadletter.exe")  -Force
Copy-Item $Probe (Join-Path $Path "bin/deadletterd.exe") -Force

Set-Content -LiteralPath (Join-Path $Path "share/qml/Main.qml") -NoNewline -Value @"
import QtQuick
Item { }
"@

Set-Content -LiteralPath (Join-Path $Path "README.txt") -NoNewline -Value "DeadLetter demo payload."

$count = (Get-ChildItem -LiteralPath $Path -Recurse -File | Measure-Object).Count
$bytes = (Get-ChildItem -LiteralPath $Path -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host "wrote $count files, $bytes bytes to $Path"
