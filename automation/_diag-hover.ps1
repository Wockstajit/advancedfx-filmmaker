param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',$Port); $c.NoDelay=$true
$s=$c.GetStream(); $enc=[Text.Encoding]::ASCII
function Send([string]$body){ $js="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();"+$body; $cmd='mirv_filmmaker ui_eval "'+$js+'"'; if($cmd.Length -gt 256){Write-Host "OVER256($($cmd.Length))" -ForegroundColor Red; return}; $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 350 }
foreach($id in @('FmSearchBtn','FmRefreshBtn','FmAddFolderBtn')){
  Send "var b=r.FindChildTraverse('$id');b.RemoveClass('content-navbar__tabs__btn');b.RemoveClass('left-right-flow');"
  Send "var b=r.FindChildTraverse('$id');b.style.flowChildren='right';b.style.width='auto';b.style.height='52px';"
}
# report each button's width to confirm they no longer span the whole bar
foreach($id in @('FmSearchBtn','FmRefreshBtn','FmAddFolderBtn')){
  Send "var b=r.FindChildTraverse('$id');`$.Msg('[w] $id='+b.actuallayoutwidth+'\n');"
}
Start-Sleep -Milliseconds 300
$buf=New-Object byte[] 16384; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}
($o -split "`n") | Where-Object { $_ -match '\[w\]' } | ForEach-Object { Write-Host $_.Trim() -ForegroundColor Yellow }
$c.Close()