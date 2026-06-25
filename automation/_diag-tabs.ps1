param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port); $client.NoDelay = $true
$stream = $client.GetStream(); $enc = [System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192
  while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }
  return $sb.ToString() }
function Send([string]$cmd,[double]$s=1.5){ $stream.Write(($enc.GetBytes($cmd+"`n")),0,($cmd.Length+1)); $stream.Flush(); return Drain $s }
function Eval([string]$js,[double]$s=1.5){ return Send ('mirv_filmmaker ui_eval "'+$js+'"') $s }

Drain 0.5 | Out-Null
Write-Host '=== 1. ping roundtrip ===' -ForegroundColor Cyan
Write-Host (Eval "`$.Msg('[PING] ok\n');" 1.5)

Write-Host '=== 2. find page from context root ===' -ForegroundColor Cyan
$js = "var r=`$.GetContextPanel();if(!r){`$.Msg('[DIAG] noctx\n');}else{var depth=0;while(r.GetParent()){r=r.GetParent();depth++;}var w=r.FindChildTraverse('JsWatchContent');`$.Msg('[DIAG] rootId='+r.id+' depth='+depth+' watch='+(w?1:0)+'\n');}"
Write-Host (Eval $js 1.5)

Write-Host '=== 3. tab visibility (which tab is showing) ===' -ForegroundColor Cyan
$js2 = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');if(!w){`$.Msg('[DIAG] nowatch\n');}else{var dl=w.FindChildTraverse('JsDownloaded');var ym=w.FindChildTraverse('JsYourMatches');`$.Msg('[DIAG] dlHide='+(dl&&dl.BHasClass('WatchMenu--Hide')?1:0)+' ymHide='+(ym&&ym.BHasClass('WatchMenu--Hide')?1:0)+'\n');}"
Write-Host (Eval $js2 1.5)

$client.Close()
