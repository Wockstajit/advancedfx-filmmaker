param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'runs\_diag-nav3'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port); $client.NoDelay = $true
$stream = $client.GetStream(); $enc = [System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192
  while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }
  return $sb.ToString() }
function Send([string]$cmd,[double]$s=1.2){ $stream.Write(($enc.GetBytes($cmd+"`n")),0,($cmd.Length+1)); $stream.Flush(); return Drain $s }
function Eval([string]$js,[double]$s=1.2){ $full='mirv_filmmaker ui_eval "'+$js+'"'; if($full.Length -gt 256){Write-Host "  !! cmdlen=$($full.Length) OVER 256" -ForegroundColor Red}else{Write-Host "  ok cmdlen=$($full.Length)"}; return Send $full $s }
function Shot([string]$n){ & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-main-monitor.ps1') -Out (Join-Path $out $n) 2>$null | Out-Null; Write-Host "    shot $n" }

Drain 0.5 | Out-Null
Send 'mirv_filmmaker ui' 1.2 | Out-Null
Start-Sleep -Milliseconds 600
Shot 'A_initial_dl.png'

# Hide Downloaded, show YourMatches (tight: single-letter vars, hoisted class string)
Write-Host 'Switch -> YourMatches (class toggle)'
$js = "var w=`$.GetContextPanel();while(w.GetParent())w=w.GetParent();w=w.FindChildTraverse('JsWatchContent');var H='WatchMenu--Hide';w.FindChildTraverse('JsDownloaded').AddClass(H);w.FindChildTraverse('JsYourMatches').RemoveClass(H);"
Write-Host (Eval $js 1.2); Start-Sleep -Milliseconds 700; Shot 'B_yourmatches.png'

# Back to Downloaded
Write-Host 'Switch -> Downloaded (class toggle)'
$js = "var w=`$.GetContextPanel();while(w.GetParent())w=w.GetParent();w=w.FindChildTraverse('JsWatchContent');var H='WatchMenu--Hide';w.FindChildTraverse('JsYourMatches').AddClass(H);w.FindChildTraverse('JsDownloaded').RemoveClass(H);"
Write-Host (Eval $js 1.2); Start-Sleep -Milliseconds 700; Shot 'C_downloaded.png'

$client.Close()
Write-Host "screens in $out"
