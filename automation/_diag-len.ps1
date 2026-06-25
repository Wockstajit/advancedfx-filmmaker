param([int]$Port = 29010)
$ErrorActionPreference = 'Stop'
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port); $client.NoDelay = $true
$stream = $client.GetStream(); $enc = [System.Text.Encoding]::ASCII
function Drain([double]$s){ $d=(Get-Date).AddSeconds($s); $sb=New-Object System.Text.StringBuilder; $buf=New-Object byte[] 8192
  while((Get-Date) -lt $d){ if($stream.DataAvailable){ $n=$stream.Read($buf,0,$buf.Length); if($n -gt 0){[void]$sb.Append($enc.GetString($buf,0,$n))} } else { Start-Sleep -Milliseconds 40 } }
  return $sb.ToString() }
function Send([string]$cmd,[double]$s=1.2){ $stream.Write(($enc.GetBytes($cmd+"`n")),0,($cmd.Length+1)); $stream.Flush(); return Drain $s }
Drain 0.5 | Out-Null
foreach($len in @(100,150,180,200,210,220,230,240,260,300,400)){
  # Build a JS that prints a unique token only if it parses & runs fully.
  $pad = 'x' * [Math]::Max(0,$len-60)
  $js = "var a='$pad';`$.Msg('[LEN $len ok='+a.length+']\n');"
  $full = 'mirv_filmmaker ui_eval "' + $js + '"'
  $out = Send $full 1.0
  $got = if($out -match "\[LEN $len ok=(\d+)\]"){ "RAN ok=$($Matches[1])" } else { 'TRUNCATED/no-output' }
  Write-Host ("cmdlen={0,4}  jslen={1,4}  -> {2}" -f $full.Length, $js.Length, $got)
}
$client.Close()
