# Headlessly launches CS2 with the AfxHookSource2 hook injected and a TCP
# console (-netconport) open, using HLAE's CustomLoader CLI (-noGui).
# No HLAE GUI, no mouse/keyboard takeover.
#
#   powershell.exe -ExecutionPolicy Bypass -File misc\launch-cs2-netcon.ps1 -Port 29010
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = "F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive",
    [int]$Width = 1280,
    [int]$Height = 720
)
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $PSScriptRoot
$bin     = Join-Path $root 'build\staging-release\bin'
$hlae    = Join-Path $bin 'HLAE.exe'
$dll     = Join-Path $bin 'x64\AfxHookSource2.dll'
$cs2exe  = Join-Path $Cs2Dir 'game\bin\win64\cs2.exe'

foreach ($f in @($hlae,$dll,$cs2exe)) { if (-not (Test-Path $f)) { Write-Error "Missing: $f"; exit 1 } }

# Close any running CS2 (only one Steam instance allowed; staging DLL re-injects fresh).
Get-Process -Name cs2 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
if (-not $steam) { $steam = 'C:\Program Files (x86)\Steam' }

# Mirror hlae/LaunchCs2.cs cmdline, plus the net console flags.
$cmdLine = "-steam -insecure -sw -w $Width -h $Height -console +sv_lan 1 -netconport $Port -afxFixNetCon"

$argList = @(
    '-customLoader','-autoStart','-noGui',
    '-hookDllPath', $dll,
    '-programPath', $cs2exe,
    '-cmdLine', $cmdLine,
    '-addEnv', "SteamPath=$steam",
    '-addEnv', 'SteamClientLaunch=1',
    '-addEnv', 'SteamGameId=730',
    '-addEnv', 'SteamAppId=730',
    '-addEnv', 'SteamOverlayGameId=730'
)

Write-Host "Launching CS2 headless (hook + netconport $Port, ${Width}x${Height} windowed)..."
# Use ProcessStartInfo.ArgumentList so the multi-word -cmdLine stays a single argument.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $hlae
$psi.UseShellExecute = $false
$psi.WorkingDirectory = $bin
# PowerShell 7 (.NET Core) exposes ProcessStartInfo.ArgumentList for robust quoting;
# Windows PowerShell 5.1 (.NET Framework) does NOT, so fall back to a hand-quoted
# Arguments string. Either way the multi-word -cmdLine value stays ONE argument.
if ($null -ne $psi.ArgumentList) {
    foreach ($a in $argList) { $psi.ArgumentList.Add([string]$a) }
} else {
    $quote = { param($s) if ($s -match '[\s"]') { '"' + ($s -replace '"', '\"') + '"' } else { $s } }
    $psi.Arguments = (($argList | ForEach-Object { & $quote ([string]$_) }) -join ' ')
}
[void][System.Diagnostics.Process]::Start($psi)
Write-Host "Done. CS2 will reach the main menu in ~30-45s; netconport listens on 127.0.0.1:$Port"
