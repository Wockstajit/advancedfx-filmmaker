# Captures the actual CS2 game window to a PNG via PrintWindow (PW_RENDERFULLCONTENT).
# No mouse/keyboard takeover - it grabs the window's rendered content directly.
# Run with Windows PowerShell 5.1 (has System.Drawing built in):
#   powershell.exe -ExecutionPolicy Bypass -File misc\capture-cs2.ps1 -Out C:\path\shot.png
param(
    [string]$Out = (Join-Path $env:TEMP 'cs2_capture.png'),
    [string]$Process = 'cs2'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class WinCap {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static string Capture(IntPtr hwnd, string path) {
    RECT r; GetWindowRect(hwnd, out r);
    int w = r.Right-r.Left, h = r.Bottom-r.Top;
    if (w <= 0 || h <= 0) return "0x0";
    using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
    using (var g = Graphics.FromImage(bmp)) {
      IntPtr hdc = g.GetHdc();
      PrintWindow(hwnd, hdc, 2u); // 2 = PW_RENDERFULLCONTENT
      g.ReleaseHdc(hdc);
      bmp.Save(path, ImageFormat.Png);
    }
    return w + "x" + h;
  }
}
'@ -ReferencedAssemblies System.Drawing
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "No '$Process' window found."; exit 1 }
$dim = [WinCap]::Capture($p.MainWindowHandle, $Out)
$kb = [math]::Round((Get-Item $Out).Length/1kb)
Write-Host "Captured $Process ($dim, $kb KB) -> $Out"
