param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1',$Port); $c.NoDelay=$true
$s=$c.GetStream(); $enc=[Text.Encoding]::ASCII
function Send([string]$cmd,[int]$wait){ if($cmd.Length -gt 256){Write-Host "OVER256($($cmd.Length))" -ForegroundColor Red}; $b=$enc.GetBytes($cmd+"`n"); $s.Write($b,0,$b.Length); $s.Flush(); Start-Sleep -Milliseconds $wait; $buf=New-Object byte[] 8192; $o=''; while($s.DataAvailable){ $n=$s.Read($buf,0,$buf.Length); $o+=$enc.GetString($buf,0,$n)}; return $o }
function E([string]$body,[int]$wait=700){ $js="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();"+$body; (Send ('mirv_filmmaker ui_eval "'+$js+'"') $wait) -split "`n" | Where-Object { $_ -match '^\[' } | ForEach-Object { Write-Host $_.Trim() -ForegroundColor Yellow } }
Send '' 200 | Out-Null
# navbar children (class first token), and the host (last child) child count
E "var n=r.FindChildTraverse('watch-navbar');var o='';for(var i=0;i<n.GetChildCount();i++)o+=i+':'+n.GetChild(i).GetAttributeString('class','-').split(' ')[0]+';';`$.Msg('[nav] '+o+'\n');"
# bar parent's OTHER child (the sibling of FmNavBtns)
E "var b=r.FindChildTraverse('FmNavBtns'),p=b.GetParent();var o='';for(var i=0;i<p.GetChildCount();i++){var c=p.GetChild(i);o+=i+':'+(c.id||'-')+'/'+c.GetAttributeString('class','-').split(' ')[0]+'/vis'+(c.visible?1:0)+';';}`$.Msg('[sib] '+o+'\n');"
$c.Close()
