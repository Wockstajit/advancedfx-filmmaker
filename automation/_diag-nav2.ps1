param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'runs\_diag-nav2'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port); $client.NoDelay = $true
$stream = $client.GetStream(); $enc = [System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192
  while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }
  return $sb.ToString() }
function Send([string]$cmd,[double]$s=1.2){ $stream.Write(($enc.GetBytes($cmd+"`n")),0,($cmd.Length+1)); $stream.Flush(); return Drain $s }
function Eval([string]$js,[double]$s=1.2){ $full='mirv_filmmaker ui_eval "'+$js+'"'; Write-Host ("  jslen=$($js.Length) cmdlen=$($full.Length)"); return Send $full $s }
function Shot([string]$n){ & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-main-monitor.ps1') -Out (Join-Path $out $n) 2>$null | Out-Null; Write-Host "    shot $n" }

Drain 0.5 | Out-Null
Send 'mirv_filmmaker ui' 1.2 | Out-Null
Start-Sleep -Milliseconds 600

# T1: canonical 2-arg NavigateToTab from our context panel ($)
Write-Host 'T1: $.DispatchEvent NavigateToTab JsYourMatches (from $ ctx)'
$js = "`$.DispatchEvent('NavigateToTab','JsYourMatches','ym');"
Write-Host (Eval $js 1.2); Start-Sleep -Milliseconds 600; Shot 'T1_ym.png'

# T2: dispatch NavigateToTab sourced from the YourMatches button panel
Write-Host 'T2: dispatch sourced from the button'
$js = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var b=r.FindChildTraverse('WatchNavBarYourMatches');`$.DispatchEvent('NavigateToTab','JsYourMatches','ym');"
Write-Host (Eval $js 1.2); Start-Sleep -Milliseconds 600; Shot 'T2_ym.png'

# T3: back to downloaded via canonical event
Write-Host 'T3: NavigateToTab JsDownloaded'
$js = "`$.DispatchEvent('NavigateToTab','JsDownloaded','dl');"
Write-Host (Eval $js 1.2); Start-Sleep -Milliseconds 600; Shot 'T3_dl.png'

$client.Close()
Write-Host "screens in $out"
