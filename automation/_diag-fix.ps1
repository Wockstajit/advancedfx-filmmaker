param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port); $client.NoDelay = $true
$stream = $client.GetStream(); $enc = [System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192
  while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }
  return $sb.ToString() }
function Send([string]$cmd,[double]$s=1.0){ $stream.Write(($enc.GetBytes($cmd+"`n")),0,($cmd.Length+1)); $stream.Flush(); return Drain $s }
function E([string]$body,[double]$s=1.0){
  $js = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();" + $body
  $full = 'mirv_filmmaker ui_eval "' + $js + '"'
  if($full.Length -gt 256){ Write-Host "  OVER256($($full.Length))" -ForegroundColor Red }
  $out = Send $full $s
  ($out -split "`n") | Where-Object { $_ -match '^\[' } | ForEach-Object { Write-Host ("   " + $_.Trim()) -ForegroundColor Gray }
}
Drain 0.5 | Out-Null
Send 'mirv_filmmaker ui' 1.0 | Out-Null; Start-Sleep -Milliseconds 600

Write-Host 'host height + navbar height:' -ForegroundColor Cyan
E "var n=r.FindChildTraverse('watch-navbar');var h=n.GetChild(n.GetChildCount()-1);`$.Msg('[host] '+h.id+' h='+h.actuallayoutheight+' navH='+n.actuallayoutheight+'\n');"
E "var n=r.FindChildTraverse('watch-navbar');var c=n.GetChild(0);`$.Msg('[center] h='+c.actuallayoutheight+' kids='+c.GetChildCount()+'\n');"

Write-Host 'tiles (loaded + outcome):' -ForegroundColor Cyan
E "var t=r.FindChildTraverse('FmMatchList').GetChild(0);`$.Msg('[t0] L='+t.__loaded+' '+t.BHasClass('MatchVictory')+t.BHasClass('MatchLoss')+t.BHasClass('MatchTied')+'\n');"
E "var t=r.FindChildTraverse('FmMatchList').GetChild(5);`$.Msg('[t5] L='+t.__loaded+' '+t.BHasClass('MatchVictory')+t.BHasClass('MatchLoss')+t.BHasClass('MatchTied')+'\n');"
E "var t=r.FindChildTraverse('FmMatchList').GetChild(0);`$.Msg('[t0o] oc='+(t.FindChildTraverse('outcome')?t.FindChildTraverse('outcome').text:'?')+'\n');"
$client.Close()
