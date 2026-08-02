<#
.SYNOPSIS
    Screenshots the installer window.

.DESCRIPTION
    Uses PrintWindow with PW_RENDERFULLCONTENT rather than CopyFromScreen.
    CopyFromScreen captures whatever is physically on the display, so it picks
    up overlapping windows and misses the window entirely if it is not on top.
    PW_RENDERFULLCONTENT is also what makes a Direct2D-composed client area
    appear at all; without it the client region comes back blank.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$SettleMs = 2500
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class LwiCapture {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

# PowerShell is DPI-unaware, and a DPI-unaware caller gets VIRTUALIZED
# coordinates back from GetWindowRect: an 800x650 window on a 125% display
# measures as 640x520. PrintWindow then renders the real window into an
# undersized bitmap and silently clips the right and bottom edges, which reads
# exactly like a layout bug in the application under test.
# -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
[void][LwiCapture]::SetProcessDpiAwarenessContext([IntPtr](-4))

$process = Start-Process -FilePath $Installer -PassThru
try {
    Start-Sleep -Milliseconds $SettleMs
    $process.Refresh()

    $handle = $process.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) {
        throw "installer window never appeared"
    }

    $rect = New-Object LwiCapture+RECT
    [void][LwiCapture]::GetWindowRect($handle, [ref]$rect)
    $width  = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    [void][LwiCapture]::PrintWindow($handle, $dc, 2)  # PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($dc)

    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()

    Write-Host "captured ${width}x${height} to $Out"
}
finally {
    if (-not $process.HasExited) { $process.Kill() }
}
