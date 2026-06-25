$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',29010)
$s = $c.GetStream(); $enc = [Text.Encoding]::ASCII
function Send($cmd){ $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds 700
  $buf=New-Object byte[] 16384; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; $o }
function E($js){ Send ('mirv_filmmaker ui_eval "'+$js+'"') | Out-Null }
# open demos page + downloaded + search
Send 'mirv_filmmaker ui' | Out-Null
E "`$.Filmmaker.gotoTab('downloaded')"
Start-Sleep -Milliseconds 600
# open search via the button
E "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();`$.DispatchEvent('Activated',r.FindChildTraverse('FmSearchBtn'))"
Start-Sleep -Milliseconds 500
# live-apply the new search-box look (opaque + high z)
E "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();r.FindChildTraverse('FmSearchWrap').style.zIndex='1000'"
E "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var i=r.FindChildTraverse('FmSearchInput');i.style.backgroundColor='#15181dff';i.style.height='36px'"
# put a long query in to populate the dropdown
E "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var i=r.FindChildTraverse('FmSearchInput');i.text='mirage';i.SetFocus()"
Start-Sleep -Milliseconds 500
Write-Host 'applied'
$c.Close()
