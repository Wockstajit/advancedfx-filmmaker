param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'runs\_diag-verify'; New-Item -ItemType Directory -Force -Path $out | Out-Null
$client = New-Object System.Net.Sockets.TcpClient; $client.Connect('127.0.0.1',$Port); $client.NoDelay=$true
$stream=$client.GetStream(); $enc=[System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192; while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }; return $sb.ToString() }
function Send([string]$c,[double]$s=1.0){ $stream.Write(($enc.GetBytes($c+"`n")),0,($c.Length+1)); $stream.Flush(); return Drain $s }
function E([string]$body,[double]$s=1.2){ $js="var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();"+$body; $full='mirv_filmmaker ui_eval "'+$js+'"'; if($full.Length -gt 256){Write-Host "OVER256($($full.Length))" -ForegroundColor Red}; (Send $full $s) -split "`n" | Where-Object { $_ -match '^\[' } | ForEach-Object { Write-Host $_.Trim() -ForegroundColor Yellow } }
Drain 0.5 | Out-Null
Send 'mirv_filmmaker scan' 2.5 | Out-Null
Send 'mirv_filmmaker ui' 1.5 | Out-Null
Start-Sleep -Seconds 1
E "var l=r.FindChildTraverse('FmMatchList'),n=l.GetChildCount(),c=0,i;for(i=0;i<n;i++)if(l.GetChild(i).BHasClass('MatchVictory'))c++;`$.Msg('[wins] '+c+'/'+n+'\n');"
E "var l=r.FindChildTraverse('FmMatchList'),n=l.GetChildCount(),c=0,i;for(i=0;i<n;i++)if(l.GetChild(i).BHasClass('MatchLoss'))c++;`$.Msg('[loss] '+c+'/'+n+'\n');"
E "var b=r.FindChildTraverse('FmNavBtns');`$.Msg('[btns] kids='+b.GetChildCount()+' h='+b.actuallayoutheight+'\n');"
$client.Close()
& powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-game-window.ps1') -Out (Join-Path $out 'verify.png') | Out-Null
Write-Host "shot -> $out\verify.png"
