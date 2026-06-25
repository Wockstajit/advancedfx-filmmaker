param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'runs\_diag-nav'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port); $client.NoDelay = $true
$stream = $client.GetStream(); $enc = [System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192
  while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }
  return $sb.ToString() }
function Send([string]$cmd,[double]$s=1.2){ $stream.Write(($enc.GetBytes($cmd+"`n")),0,($cmd.Length+1)); $stream.Flush(); return Drain $s }
function Eval([string]$js,[double]$s=1.2){ $full='mirv_filmmaker ui_eval "'+$js+'"'; Write-Host ("  [jslen=$($js.Length)] -> ") -NoNewline; return Send $full $s }
function Shot([string]$n){ & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-main-monitor.ps1') -Out (Join-Path $out $n) 2>$null | Out-Null; Write-Host "    shot $n" }

Drain 0.5 | Out-Null
Send 'mirv_filmmaker scan' 2.0 | Out-Null
Send 'mirv_filmmaker ui' 1.5 | Out-Null
Start-Sleep -Milliseconds 800
Shot 'A_initial.png'

# Approach 1: dispatch NavigateToTab as a bubbling event FROM the navbar button (inside watch page)
Write-Host 'Approach1: NavigateToTab event on YourMatches button'
$js = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var b=r.FindChildTraverse('WatchNavBarYourMatches');b.checked=true;`$.DispatchEvent('NavigateToTab',b,'JsYourMatches','x');"
Write-Host (Eval $js 1.2)
Start-Sleep -Milliseconds 600; Shot 'B_ym_navevent.png'

# Approach 2: class toggle (replicate what NavigateToTab does to bodies)
Write-Host 'Approach2: class toggle back to Downloaded'
$js = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');w.FindChildTraverse('JsYourMatches').AddClass('WatchMenu--Hide');w.FindChildTraverse('JsDownloaded').RemoveClass('WatchMenu--Hide');"
Write-Host ("  len=" + $js.Length)
Write-Host (Eval $js 1.2)
Start-Sleep -Milliseconds 600; Shot 'C_dl_classtoggle.png'

# Approach 3: class toggle to YourMatches
Write-Host 'Approach3: class toggle to YourMatches'
$js = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');w.FindChildTraverse('JsDownloaded').AddClass('WatchMenu--Hide');w.FindChildTraverse('JsYourMatches').RemoveClass('WatchMenu--Hide');"
Write-Host ("  len=" + $js.Length)
Write-Host (Eval $js 1.2)
Start-Sleep -Milliseconds 600; Shot 'D_ym_classtoggle.png'

$client.Close()
Write-Host "screens in $out"
