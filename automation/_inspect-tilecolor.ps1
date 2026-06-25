param([int]$Port=29010,[string]$OutDir)
$ErrorActionPreference='Stop'
$here=$PSScriptRoot
if([string]::IsNullOrWhiteSpace($OutDir)){$OutDir=Join-Path $here ("runs\inspect-tilecolor\"+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$client=New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1',$Port); $client.NoDelay=$true
$stream=$client.GetStream(); $enc=[System.Text.Encoding]::ASCII
function Drain([double]$s){$d=(Get-Date).AddSeconds($s);$sb=New-Object System.Text.StringBuilder;$buf=New-Object byte[] 8192;while((Get-Date) -lt $d){if($stream.DataAvailable){$n=$stream.Read($buf,0,$buf.Length);if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))}}else{Start-Sleep -Milliseconds 40}};return $sb.ToString()}
function Send([string]$c,[double]$s=1.2){$stream.Write(($enc.GetBytes($c+"`n")),0,($c.Length+1));$stream.Flush();return Drain $s}
function Eval([string]$js,[double]$s=1.5){return Send ('mirv_filmmaker ui_eval "'+$js+'"') $s}
function Shot([string]$name){$out=Join-Path $OutDir $name; & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-main-monitor.ps1') -Out $out 2>$null | Out-Null; if(Test-Path $out){Write-Host "shot -> $out"}}

Drain 0.6 | Out-Null
Send 'mirv_filmmaker scan' 2.5 | Out-Null
Send 'mirv_filmmaker ui' 1.5 | Out-Null
Start-Sleep -Milliseconds 800

# Make sure we are on Downloaded
$clickDl="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');var nav=w&&w.FindChildTraverse('watch-navbar');var c=nav&&nav.GetChild(0);var b=c&&c.GetChild(1);if(b){b.checked=true;`$.DispatchEvent('Activated',b);}`$.Msg('[NAV] downloaded\n');"
Eval $clickDl 0.8 | Out-Null
Start-Sleep -Milliseconds 500

# Dump tiles: class + checked + which class the engine resolved
$dump="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');var l=w&&w.FindChildTraverse('FmMatchList');var n=l?l.GetChildCount():-1;`$.Msg('[T] count='+n+'\n');if(l)for(var i=0;i<n;i++){var c=l.GetChild(i);`$.Msg('[T] i='+i+' cls=['+c.GetAttributeString('class','?')+'] checked='+(c.checked?1:0)+'\n');}"
Write-Host '--- Downloaded tiles (none selected yet) ---'
Write-Host (Eval $dump 1.5)
Shot '1_downloaded_unselected.png'

# Select tile index 1 explicitly and re-dump
$sel="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');var l=w&&w.FindChildTraverse('FmMatchList');if(l&&l.GetChildCount()>1){var t=l.GetChild(1);t.checked=true;`$.DispatchEvent('Activated',t);}`$.Msg('[SEL] picked idx1\n');"
Eval $sel 0.8 | Out-Null
Start-Sleep -Milliseconds 500
Write-Host '--- Downloaded tiles (idx1 selected) ---'
Write-Host (Eval $dump 1.5)
Shot '2_downloaded_idx1_selected.png'

$client.Close()
Write-Host "OutDir: $OutDir"
