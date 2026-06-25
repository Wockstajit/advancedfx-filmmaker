param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'runs\_diag-nav4'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port); $client.NoDelay = $true
$stream = $client.GetStream(); $enc = [System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192
  while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }
  return $sb.ToString() }
function Send([string]$cmd,[double]$s=1.2){ $stream.Write(($enc.GetBytes($cmd+"`n")),0,($cmd.Length+1)); $stream.Flush(); return Drain $s }
function Eval([string]$js,[double]$s=1.2){ return Send ('mirv_filmmaker ui_eval "'+$js+'"') $s }
function Shot([string]$n){ & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-main-monitor.ps1') -Out (Join-Path $out $n) 2>$null | Out-Null; Write-Host "    shot $n" }

Drain 0.5 | Out-Null
Send 'mirv_filmmaker ui' 1.2 | Out-Null
Start-Sleep -Milliseconds 600
Shot 'A_initial_dl.png'

Write-Host 'T1: MainMenu.NavigateToTab JsYourMatches'
Write-Host (Eval "if(typeof MainMenu!=='undefined')`$.Msg('[has MainMenu]\n');if(typeof mainmenu_watch!=='undefined')`$.Msg('[has mainmenu_watch]\n');" 1.0)
Write-Host (Eval "MainMenu.NavigateToTab('JsYourMatches','ym');" 1.2); Start-Sleep -Milliseconds 700; Shot 'B_mainmenu_ym.png'

Write-Host 'T2: mainmenu_watch.NavigateToTab JsYourMatches'
Send 'mirv_filmmaker ui' 1.0 | Out-Null; Start-Sleep -Milliseconds 500
Write-Host (Eval "mainmenu_watch.NavigateToTab('JsYourMatches','ym');" 1.2); Start-Sleep -Milliseconds 700; Shot 'C_mmwatch_ym.png'

Write-Host 'T3: mainmenu_watch back to Downloaded'
Write-Host (Eval "mainmenu_watch.NavigateToTab('JsDownloaded','dl');" 1.2); Start-Sleep -Milliseconds 700; Shot 'D_mmwatch_dl.png'

$client.Close()
Write-Host "screens in $out"
