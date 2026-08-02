<#
.SYNOPSIS
    Builds a throwaway payload directory for exercising the forge.

.DESCRIPTION
    The binaries are copies of real system executables rather than files with an
    "MZ" prefix and filler text.

    That distinction is not cosmetic. IPersistFile::Save resolves a shortcut's
    target while writing the link, and a file named .exe that is not a valid PE
    makes it fail with a bare E_FAIL. A fixture built from fake executables
    therefore produces an installer whose shortcuts silently never appear, and
    the failure looks like an installer bug rather than a fixture bug.
#>

[CmdletBinding()]
param(
    [string]$Path = "build/stage-demo"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Test-Path -LiteralPath $Path) {
    Remove-Item -LiteralPath $Path -Recurse -Force
}

New-Item -ItemType Directory -Force -Path (Join-Path $Path "bin") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Path "share/qml") | Out-Null

$system32 = Join-Path $env:SystemRoot "System32"
Copy-Item (Join-Path $system32 "notepad.exe") (Join-Path $Path "bin/deadletter.exe") -Force
Copy-Item (Join-Path $system32 "where.exe")   (Join-Path $Path "bin/deadletterd.exe") -Force

Set-Content -LiteralPath (Join-Path $Path "share/qml/Main.qml") -NoNewline -Value @"
import QtQuick
Item { }
"@

Set-Content -LiteralPath (Join-Path $Path "README.txt") -NoNewline -Value "DeadLetter demo payload."

$count = (Get-ChildItem -LiteralPath $Path -Recurse -File | Measure-Object).Count
$bytes = (Get-ChildItem -LiteralPath $Path -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host "wrote $count files, $bytes bytes to $Path"
