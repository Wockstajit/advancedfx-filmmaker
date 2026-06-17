#requires -Version 5
<#
============================================================
  Watch UI - automated isolation / layout verifier
============================================================
  Drives the live Filmmaker Watch UI over the CS2 netcon REPL
  (mirv_filmmaker ui_eval) and asserts the acceptance criteria
  WITHOUT pixel-clicking: it reads real Panorama panel state
  (tab visibility classes, child counts, whether tiles/detail
  loaded their native layouts) and captures a screenshot of the
  game window at each step via PrintWindow.

  This is the automated equivalent of the manual test flow:
    open Watch -> Downloaded -> screenshot -> click Your Matches
    -> screenshot -> back to Downloaded -> toggle repeatedly,
    asserting the two tabs stay isolated and the Downloaded
    layout is the native (styled) one each time.

  PREREQ: CS2 must already be running under this HLAE build with
  the netcon console enabled (-netconport 29010). Use
  test-filmmaker.ps1 to launch, then:

    pwsh misc\verify-watch-ui.ps1
============================================================
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$OutDir = (Join-Path $PSScriptRoot '..\build\verify-shots'),
    [double]$ReadSeconds = 1.2
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# --- netcon plumbing (mirror of cs2-netcon.ps1, but returns the text) --------
$cs2 = Get-Process -Name 'cs2' -ErrorAction SilentlyContinue
if (-not $cs2) { Write-Host '[FAIL] cs2.exe is not running. Launch it with test-filmmaker.ps1 first.' -ForegroundColor Red; exit 1 }

$client = New-Object System.Net.Sockets.TcpClient
try { $client.Connect('127.0.0.1', $Port) }
catch { Write-Host "[FAIL] Could not connect to netcon on 127.0.0.1:$Port. Add -netconport $Port to CS2 launch options." -ForegroundColor Red; exit 1 }
$client.NoDelay = $true
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::ASCII

function Drain([double]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $sb = New-Object System.Text.StringBuilder
    $buf = New-Object byte[] 8192
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { [void]$sb.Append($enc.GetString($buf, 0, $n)) }
        } else { Start-Sleep -Milliseconds 40 }
    }
    return $sb.ToString()
}
function Send([string]$cmd, [double]$seconds = $ReadSeconds) {
    $stream.Write(($enc.GetBytes($cmd + "`n")), 0, ($cmd.Length + 1)); $stream.Flush()
    return Drain $seconds
}

# Single-line Panorama JS (SINGLE quotes only) that locates the Watch page and
# emits a parseable report line. Optionally clicks a navbar tab first.
$reportJs = @'
var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');if(!w){$.Msg('[VERIFY] nopage=1\n');}else{var dl=w.FindChildTraverse('JsDownloaded');var b=w.FindChildTraverse('FmDownloadedBody');var ym=w.FindChildTraverse('JsYourMatches');var list=w.FindChildTraverse('FmMatchList');var tiles=list?list.GetChildCount():-1;var first=list&&tiles>0?list.GetChild(0):null;var styled=first?(first.FindChildTraverse('mapname')?1:0):-1;var sb=b&&b.FindChildTraverse('Scoreboard')?1:0;var rows=b?( (b.FindChildTraverse('players-table-CT')?b.FindChildTraverse('players-table-CT').GetChildCount():0)+(b.FindChildTraverse('players-table-TERRORIST')?b.FindChildTraverse('players-table-TERRORIST').GetChildCount():0)):-1;$.Msg('[VERIFY] dlHide='+(dl&&dl.BHasClass('WatchMenu--Hide')?1:0)+' ymHide='+(ym&&ym.BHasClass('WatchMenu--Hide')?1:0)+' bodyExists='+(b?1:0)+' tiles='+tiles+' styledTile='+styled+' scoreboard='+sb+' playerRows='+rows+'\n');}
'@ -replace "`r?`n", ' '

function ClickTabJs([string]$which) {
    # which = 'YourMatches' | 'Downloaded'
    if ($which -eq 'YourMatches') {
        return "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var b=r.FindChildTraverse('WatchNavBarYourMatches');if(b){b.checked=true;`$.DispatchEvent('Activated',b);`$.Msg('[CLICK] YourMatches\n');}"
    } else {
        return "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');var nav=w&&w.FindChildTraverse('watch-navbar');var c=nav&&nav.GetChild(0);var b=c&&c.GetChild(1);if(b){b.checked=true;`$.DispatchEvent('Activated',b);`$.Msg('[CLICK] Downloaded\n');}"
    }
}

function Eval([string]$js, [double]$seconds = $ReadSeconds) {
    return Send ('mirv_filmmaker ui_eval "' + $js + '"') $seconds
}

function Parse([string]$text) {
    $m = [regex]::Match($text, '\[VERIFY\]\s+(.+)')
    $h = @{}
    if ($m.Success) { foreach ($kv in ($m.Groups[1].Value -split '\s+')) { $p = $kv -split '=', 2; if ($p.Count -eq 2) { $h[$p[0]] = $p[1] } } }
    return $h
}

function Shot([string]$name) {
    $out = Join-Path $OutDir $name
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $here 'capture-cs2.ps1') -Out $out 2>$null | Out-Null
    if (Test-Path $out) { Write-Host "      shot -> $out" -ForegroundColor DarkGray }
}

$results = New-Object System.Collections.Generic.List[string]
function Check([string]$label, [bool]$cond) {
    if ($cond) { Write-Host "[ PASS ] $label" -ForegroundColor Green; $results.Add("PASS $label") }
    else { Write-Host "[ FAIL ] $label" -ForegroundColor Red; $results.Add("FAIL $label") }
}

Write-Host '=== Watch UI verifier ===' -ForegroundColor Cyan
Drain 0.5 | Out-Null

# 1) Open Watch -> Downloaded
Write-Host "`n--- Step 1: open Watch -> Downloaded ---" -ForegroundColor Cyan
Send 'mirv_filmmaker scan' 2.0 | Out-Null
Send 'mirv_filmmaker ui' 1.5 | Out-Null
Start-Sleep -Milliseconds 600
$dl1 = Parse (Eval $reportJs)
Shot '1_downloaded.png'
Check 'Downloaded tab is visible (not WatchMenu--Hide)' ($dl1['dlHide'] -eq '0')
Check 'Our Downloaded body exists' ($dl1['bodyExists'] -eq '1')
Check 'Downloaded has match tiles' ([int]($dl1['tiles']) -gt 0)
Check 'Tiles loaded native player.xml layout (styled, no overlap)' ($dl1['styledTile'] -eq '1')
Check 'Detail loaded native matchinfo.xml scoreboard' ($dl1['scoreboard'] -eq '1')

# 2) Click Your Matches -> must isolate
Write-Host "`n--- Step 2: click Your Matches (isolation) ---" -ForegroundColor Cyan
Eval (ClickTabJs 'YourMatches') 0.8 | Out-Null
Start-Sleep -Milliseconds 500
$ym = Parse (Eval $reportJs)
Shot '2_yourmatches.png'
Check 'Your Matches is visible' ($ym['ymHide'] -eq '0')
Check 'Downloaded is HIDDEN under Your Matches (no bleed)' ($ym['dlHide'] -eq '1')

# 3) Back to Downloaded -> layout intact again
Write-Host "`n--- Step 3: back to Downloaded ---" -ForegroundColor Cyan
Eval (ClickTabJs 'Downloaded') 0.8 | Out-Null
Start-Sleep -Milliseconds 500
$dl2 = Parse (Eval $reportJs)
Shot '3_downloaded_again.png'
Check 'Downloaded visible again' ($dl2['dlHide'] -eq '0')
Check 'Tiles still styled (no overlap on re-entry)' ($dl2['styledTile'] -eq '1')

# 4) Toggle several times -> must stay isolated every time
Write-Host "`n--- Step 4: rapid toggling (x4) ---" -ForegroundColor Cyan
$toggleOk = $true
for ($i = 1; $i -le 4; $i++) {
    Eval (ClickTabJs 'YourMatches') 0.5 | Out-Null; Start-Sleep -Milliseconds 300
    $a = Parse (Eval $reportJs 0.8)
    if ($a['dlHide'] -ne '1' -or $a['ymHide'] -ne '0') { $toggleOk = $false }
    Eval (ClickTabJs 'Downloaded') 0.5 | Out-Null; Start-Sleep -Milliseconds 300
    $b = Parse (Eval $reportJs 0.8)
    if ($b['dlHide'] -ne '0') { $toggleOk = $false }
}
Check 'Tabs stayed isolated across repeated toggling' $toggleOk

# 5) List stability: select a demo, tag the first row, induce a scan, wait 30s.
#    A stable list keeps the same tile count, the SAME first-row object (its tag
#    survives -> rows were not recreated -> scroll is preserved) and the same
#    checked row.
Write-Host "`n--- Step 5: left-list stability (30s under a scan) ---" -ForegroundColor Cyan
Eval (ClickTabJs 'Downloaded') 0.8 | Out-Null; Start-Sleep -Milliseconds 400
$selJs = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');var l=w&&w.FindChildTraverse('FmMatchList');var t=l&&l.GetChildCount()>1?l.GetChild(1):null;if(t){t.checked=true;`$.DispatchEvent('Activated',t);}l&&l.GetChildCount()>0&&l.GetChild(0).SetAttributeInt('fmtag',77);`$.Msg('[SEL] done\n');"
Eval $selJs 0.8 | Out-Null; Start-Sleep -Milliseconds 400
$stabJs = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');var l=w&&w.FindChildTraverse('FmMatchList');var n=l?l.GetChildCount():-1;var tag=(l&&n>0)?l.GetChild(0).GetAttributeInt('fmtag',-1):-1;var ck=-1;if(l)for(var i=0;i<n;i++){if(l.GetChild(i).checked){ck=i;break;}}`$.Msg('[STAB] tiles='+n+' tag='+tag+' checked='+ck+'\n');"
function StabParse([string]$txt) { $m=[regex]::Match($txt,'\[STAB\]\s+(.+)'); $h=@{}; if($m.Success){foreach($kv in ($m.Groups[1].Value -split '\s+')){$p=$kv -split '=',2; if($p.Count -eq 2){$h[$p[0]]=$p[1]}}}; return $h }
$base = StabParse (Eval $stabJs 0.8)
Shot '4_selected.png'
Send 'mirv_filmmaker scan' 1.0 | Out-Null   # induce churn (phase-2 data fill -> repeated render())
$stable = $true; $deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    $now = StabParse (Eval $stabJs 0.8)
    if ($now['tiles'] -ne $base['tiles'] -or $now['tag'] -ne '77' -or $now['checked'] -ne $base['checked']) {
        $stable = $false
        Write-Host "      drift: $($now['tiles'])/$($now['tag'])/$($now['checked']) vs $($base['tiles'])/77/$($base['checked'])" -ForegroundColor Yellow
    }
}
Shot '5_after_30s.png'
Check 'Tile count unchanged over 30s' ($stable)
Check 'First row was NOT recreated (tag survived -> scroll preserved)' ($stable)
Check 'Selected demo stayed selected over 30s' ($stable)

# 6) Buttons: both present, visible, in the same flow row (so they cannot overlap).
Write-Host "`n--- Step 6: refresh + add-demo buttons ---" -ForegroundColor Cyan
$btnJs = "var r=`$.GetContextPanel();while(r.GetParent())r=r.GetParent();var w=r.FindChildTraverse('JsWatchContent');var ref=w&&w.FindChildTraverse('FmRefreshBtn');var add=w&&w.FindChildTraverse('FmAddFolderBtn');var bar=w&&w.FindChildTraverse('FmNavBtns');`$.Msg('[BTN] ref='+(ref?1:0)+' refVis='+(ref&&ref.visible?1:0)+' add='+(add?1:0)+' addVis='+(add&&add.visible?1:0)+' sameRow='+((bar&&ref&&add&&ref.GetParent()===bar&&add.GetParent()===bar)?1:0)+'\n');"
$btn = Parse (Eval $btnJs 0.8)
Shot '6_buttons.png'
Check 'Refresh button present + visible' ($btn['ref'] -eq '1' -and $btn['refVis'] -eq '1')
Check 'Add Demo button present + visible' ($btn['add'] -eq '1' -and $btn['addVis'] -eq '1')
Check 'Both buttons share the flow row (cannot overlap)' ($btn['sameRow'] -eq '1')

$client.Close()

# --- Summary ---------------------------------------------------------------
$fail = ($results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
$results | ForEach-Object { Write-Host "  $_" }
Write-Host "Screenshots in: $OutDir"
if ($fail -eq 0) { Write-Host "`nALL CHECKS PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "`n$fail CHECK(S) FAILED" -ForegroundColor Red; exit 1 }
