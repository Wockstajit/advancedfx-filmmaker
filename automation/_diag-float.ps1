param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',$Port); $c.NoDelay=$true
$s=$c.GetStream(); $enc=[Text.Encoding]::ASCII
function Send([string]$cmd,[int]$wait){ if($cmd.Length -gt 256){Write-Host "OVER256($($cmd.Length))" -ForegroundColor Red}; $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds $wait; $buf=New-Object byte[] 8192; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; return $o }
# Minimal root-walk prefix only.
function E([string]$body,[int]$wait=450){ $js="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();"+$body; (Send ('mirv_filmmaker ui_eval "'+$js+'"') $wait) -split "`n" | Where-Object { $_ -match '^\[' } | ForEach-Object { Write-Host $_.Trim() -ForegroundColor Yellow } }
Send '' 200 | Out-Null
E "var w=r.FindChildTraverse('JsWatchContent');`$.CreatePanel('Button',w,'FmFloatTest',{});"
E "var t=r.FindChildTraverse('FmFloatTest');t.style.horizontalAlign='right';t.style.verticalAlign='top';t.hittest=true;t.style.zIndex='400';"
E "var t=r.FindChildTraverse('FmFloatTest');t.style.marginTop='13px';t.style.marginRight='260px';t.style.width='90px';t.style.height='30px';"
E "var t=r.FindChildTraverse('FmFloatTest');t.style.backgroundColor='#ff8800ee';t.SetPanelEvent('onmouseover',function(){t.style.backgroundColor='#00ff00ee';});"
E "var t=r.FindChildTraverse('FmFloatTest');`$.Msg('[float] w='+t.actuallayoutwidth+' h='+t.actuallayoutheight+'\n');"
$c.Close()
