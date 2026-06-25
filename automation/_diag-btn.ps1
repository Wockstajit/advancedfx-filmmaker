param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'runs\_diag-btn'; New-Item -ItemType Directory -Force -Path $out | Out-Null
$client = New-Object System.Net.Sockets.TcpClient; $client.Connect('127.0.0.1',$Port); $client.NoDelay=$true
$stream=$client.GetStream(); $enc=[System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192; while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }; return $sb.ToString() }
function Send([string]$c,[double]$s=1.0){ $stream.Write(($enc.GetBytes($c+"`n")),0,($c.Length+1)); $stream.Flush(); return Drain $s }
function E([string]$body,[double]$s=1.0){ $js="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();"+$body; $full='mirv_filmmaker ui_eval "'+$js+'"'; if($full.Length -gt 256){Write-Host "OVER256($($full.Length))" -ForegroundColor Red}; (Send $full $s) -split "`n" | Where-Object { $_ -match '^\[' } | ForEach-Object { Write-Host $_.Trim() -ForegroundColor Yellow } }
function Shot([string]$n){ & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-main-monitor.ps1') -Out (Join-Path $out $n) 2>$null | Out-Null; Write-Host "  shot $n" }
Drain 0.4 | Out-Null
Send 'mirv_filmmaker ui' 1.0 | Out-Null; Start-Sleep -Milliseconds 600
# Wider buttons + bigger gaps + right padding on the bar so the icons breathe.
E "var b=r.FindChildTraverse('FmNavBtns'),i,c;for(i=0;i<b.GetChildCount();i++){c=b.GetChild(i);c.style.width='40px';c.style.marginLeft='5px';c.style.marginRight='5px';}"
Start-Sleep -Milliseconds 500; Shot 'spaced.png'
$client.Close()
