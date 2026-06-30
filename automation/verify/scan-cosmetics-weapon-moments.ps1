#requires -Version 5
<#
Fast SCAN: find ticks where a player holds a primary/secondary (gun in hand).
Does NOT apply skins - verify-cosmetics-weapon-moments.ps1 does that.

Speed tips (default -Fast):
  * Few probe ticks from fmjson gun pickups only (not 20 uniform seeks)
  * Known players from fmjson - cosmetics spectate by steam (never spec_player 1..10)
  * Skips dead players offline (fmjson weapon_drop) and live (spectateHealth=0)
  * One demo load, ticks visited in ascending order
  * Skin metadata enriched in one Python batch at the end

Use -Thorough to disable fast defaults (transition scan, more ticks).
#>
[CmdletBinding()]
param(
    [int]$Port = 29010,
    [string]$Cs2Dir = "F:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive",
    [string]$Demo = "",
    [string]$OutDir = "",
    [int]$MaxProbeTicks = 0,
    [int]$MaxSpectateTries = 8,
    [int]$MaxGunHoldersPerTick = 2,
    [int]$StopAfterUniqueWeapons = 6,
    [int]$TransitionWindowTicks = 48,
    [int]$TransitionStep = 16,
    [switch]$Fast,
    [switch]$Thorough,
    [switch]$ScanTransitions,
    [switch]$NoLaunch,
    [switch]$SkipOfflinePlan,
    [switch]$LeaveCs2Open
)

$ErrorActionPreference = 'Stop'
$automationRoot = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $automationRoot
. (Join-Path $automationRoot 'lib\CosmeticsSpectate.ps1')

if (-not $Thorough -and -not $PSBoundParameters.ContainsKey('Fast')) { $Fast = $true }
if ($Fast) {
    if ($MaxProbeTicks -le 0) { $MaxProbeTicks = 5 }
    if (-not $PSBoundParameters.ContainsKey('MaxGunHoldersPerTick')) { $MaxGunHoldersPerTick = 2 }
    if (-not $PSBoundParameters.ContainsKey('StopAfterUniqueWeapons')) { $StopAfterUniqueWeapons = 6 }
} elseif ($MaxProbeTicks -le 0) {
    $MaxProbeTicks = 10
}

$Script:PrimaryDefs = @(7,8,9,10,11,13,14,16,17,19,23,24,25,26,27,28,29,33,34,35,38,39,40,60)
$Script:SecondaryDefs = @(1,2,3,4,30,32,36,61,63,64)
$Script:KnifeDefs = @(42,59,500,503,505,506,507,508,509,512,514,515,516,517,518,519,520,521,522,523,525,526)
$Script:DiagReadSec = if ($Fast) { 0.65 } else { 1.2 }
$Script:SeekReadSec = if ($Fast) { 2.0 } else { 4.5 }
$Script:SpecSleepMs = if ($Fast) { 150 } else { 350 }

function Test-Netcon([int]$p) {
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $p, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne(400)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    } catch { return $false } finally { $t.Dispose() }
}

function Resolve-DemoPath {
    $replayDir = Join-Path $Cs2Dir 'game\csgo\replays'
    if ($Demo) {
        foreach ($c in @($Demo, (Join-Path $root $Demo), (Join-Path $Cs2Dir "game\csgo\$Demo"), (Join-Path $replayDir ($Demo -replace '^replays/', '')))) {
            $dem = if ($c -match '\.dem$') { $c } else { "$c.dem" }
            if (Test-Path -LiteralPath $dem) { return (Resolve-Path -LiteralPath $dem).Path }
        }
    }
    $pref = Join-Path $replayDir 'match730_003827583940474962026_0709708846_125.dem'
    if (Test-Path -LiteralPath $pref) { return (Resolve-Path -LiteralPath $pref).Path }
    $any = Get-ChildItem -LiteralPath $replayDir -Filter '*.dem' -EA SilentlyContinue | Sort-Object LastWriteTime -Desc | Select-Object -First 1
    if ($any) { return $any.FullName }
    throw "No demo found under $replayDir"
}

function LastMatch([string]$t, [string]$p) {
    $m = [regex]::Matches($t, $p)
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    return $null
}

function Get-SkipReason([int]$def) {
    if ($def -le 0) { return 'no active weapon' }
    if ($Script:KnifeDefs -contains $def) { return 'knife out' }
    if ($def -eq 43 -or $def -in @(44,45,46,47,48)) { return 'grenade out' }
    if ($def -eq 49) { return 'c4 out' }
    if ($def -eq 31) { return 'zeus out' }
    if ($def -ge 5027 -and $def -le 5040) { return 'gloves' }
    if (($Script:PrimaryDefs -notcontains $def) -and ($Script:SecondaryDefs -notcontains $def)) { return "not a gun (def $def)" }
    return $null
}

function Parse-VisualDiag([string]$text) {
    $health = LastMatch $text 'spectateHealth=(\d+)'
    $isDead = ($text -match 'is dead \(health=') -or ($health -and [int]$health -le 0) -or ($text -match 'no spectated pawn')
    return @{
        defIndex = LastMatch $text 'defIndex=(\d+)'
        steam = LastMatch $text 'spectateSteam=(\d+)'
        pawn = LastMatch $text 'spectatePawn=(\d+)'
        health = $health
        weaponClass = LastMatch $text "class='([^']+)'"
        paint = LastMatch $text 'paint\(def6\)=(\d+)'
        raw = $text
        isDead = [bool]$isDead
    }
}

function New-RawMoment([hashtable]$diag, [int]$tick, [string]$anim, [string]$reason) {
    return [ordered]@{
        tick = $tick
        playerSteamId = $diag.steam
        playerEntityId = $diag.pawn
        playerName = ''
        weaponDefIndex = [int]$diag.defIndex
        weaponClass = $diag.weaponClass
        paintIndex = if ($diag.paint) { [int]$diag.paint } else { 0 }
        animationState = $anim
        reason = $reason
        activeWeapon = $true
    }
}

$demoPath = Resolve-DemoPath
$script:demoName = [IO.Path]::GetFileNameWithoutExtension($demoPath)
$demoPlay = "replays/$script:demoName"
$outAbs = if ([string]::IsNullOrWhiteSpace($OutDir)) {
    Join-Path $root "automation\output\cosmetics_weapon_moments\$script:demoName"
} elseif ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
New-Item -ItemType Directory -Path $outAbs -Force | Out-Null
$logFile = Join-Path $outAbs 'scan.log'
$scanRawFile = Join-Path $outAbs 'scan_raw.json'
$enumFile = Join-Path $outAbs 'enumeration.log'
"=== scan $(Get-Date -Format o) fast=$Fast ===" | Set-Content -LiteralPath $logFile

function NC([string[]]$cmds, [double]$read = 1.0, [string]$tag = '') {
    if (-not (Wait-Cs2Netcon -Port $Port)) {
        $cs2 = Get-Cs2ProcessAlive
        if ($cs2) { throw "netcon timeout ($tag) - CS2 still running (not a crash)" }
        throw "netcon dead ($tag)"
    }
    $log = Join-Path $outAbs ('nc_' + [Guid]::NewGuid().ToString('N') + '.log')
    & (Join-Path $automationRoot 'netcon\cs2-netcon.ps1') -Port $Port -Commands $cmds -ReadSeconds $read -LogPath $log | Out-Null
    $t = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    Remove-Item $log -EA SilentlyContinue
    if ($tag) { Add-Content -LiteralPath $logFile -Value "--- $tag ---`n$t" }
    return $t
}

function Read-Diag {
    return Parse-VisualDiag (NC @('mirv_filmmaker cosmetics visualdiag') $Script:DiagReadSec 'diag')
}

function Seek-Tick([int]$tick) {
    NC @("demo_gototick $tick", 'demo_pause') $Script:SeekReadSec "seek_$tick" | Out-Null
    Start-Sleep -Milliseconds $(if ($Fast) { 200 } else { 500 })
}

function Find-GunHoldersAtTick([int]$tick, [object[]]$targets) {
    $found = [System.Collections.Generic.List[object]]::new()
    Add-Content -LiteralPath $enumFile -Value "`n--- tick $tick ---"
    Write-Host "Tick $tick :" -NoNewline -ForegroundColor Cyan

    $tickTargets = @($targets | Where-Object { [int]$_.probeTick -eq $tick })
    if ($tickTargets.Count -eq 0) {
        Write-Host " no targets" -ForegroundColor DarkGray
        return @()
    }

    foreach ($t in $tickTargets) {
        if ($found.Count -ge $MaxGunHoldersPerTick) { break }
        $steam = "$($t.steamId)"
        Write-Host " [$($t.playerName)]" -NoNewline -ForegroundColor DarkCyan
        if ($t.PSObject.Properties.Match('aliveAtProbe').Count -gt 0 -and $t.aliveAtProbe -eq $false) {
            Write-Host " .dead(off)" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam offline-dead"
            continue
        }
        $spec = Invoke-CosmeticsSpectate -SteamId $steam -Netcon ${function:NC}
        if ($spec.dead) {
            Write-Host " .dead" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam live-dead"
            continue
        }
        if (-not $spec.ok) {
            Write-Host " no-spec" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam spectate failed"
            continue
        }
        Start-Sleep -Milliseconds $Script:SpecSleepMs
        $d = Read-Diag
        if (Test-PlayerDeadFromDiag $d) {
            Write-Host " .dead" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam diag-dead"
            continue
        }
        $def = if ($d.defIndex) { [int]$d.defIndex } else { 0 }
        $skip = Get-SkipReason $def
        if ($skip) {
            Write-Host " .$skip" -NoNewline -ForegroundColor DarkYellow
            Add-Content -LiteralPath $enumFile -Value "  steam=$steam skip=$skip def=$def"
            continue
        }
        $found.Add((New-RawMoment $d $tick 'idle' "targeted player steam $steam at tick $tick"))
        Write-Host " def$def" -NoNewline -ForegroundColor Green
        Add-Content -LiteralPath $enumFile -Value "  steam=$steam def=$def ok"
    }
    Write-Host ""
    return @($found)
}

$planFile = Join-Path $outAbs 'plan.json'
if (-not $SkipOfflinePlan) {
    & python (Join-Path $automationRoot 'tools\scan_demo_weapon_moments.py') $demoPath --out $planFile --max-probe-ticks $MaxProbeTicks
    if ($LASTEXITCODE -ne 0) { throw 'offline plan failed' }
}
$plan = Get-Content -LiteralPath $planFile -Raw | ConvertFrom-Json
$probeTicks = @($plan.probeTicks | ForEach-Object { [int]$_ } | Sort-Object)
if ($probeTicks.Count -eq 0) { $probeTicks = @(4000, 12000) }

$doTransitions = $ScanTransitions -or ($Thorough -and -not $Fast)

try {
    $mode = if ($Fast) { 'FAST' } else { 'THOROUGH' }
    Write-Host ""
    Write-Host "=== SCAN ($mode) - $MaxProbeTicks ticks max, stop after $StopAfterUniqueWeapons weapons ===" -ForegroundColor Cyan
    Write-Host "    No skin changes. Demo seeks are the slow part." -ForegroundColor DarkGray
    Write-Host ""

    if (-not $NoLaunch -and -not (Test-Netcon $Port)) {
        Get-Process -Name cs2,hlae -EA SilentlyContinue | Stop-Process -Force
        Start-Sleep -Seconds 1
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $automationRoot 'launch\launch-cs2-netcon.ps1') `
            -Port $Port -Cs2Dir $Cs2Dir -Width 1600 -Height 1200 -SettleSeconds $(if ($Fast) { 6 } else { 10 }) -OutDir (Join-Path $outAbs 'launch')
    }

    NC @('mvm_debug start') 1.2 'mvm' | Out-Null
    NC @("playdemo `"$demoPlay`"") $(if ($Fast) { 7.0 } else { 10.0 }) 'playdemo' | Out-Null
    Start-Sleep -Seconds $(if ($Fast) { 1 } else { 2 })
    NC @('demo_pause', 'mirv_filmmaker follow stop', 'mirv_filmmaker follow clear', 'mirv_filmmaker cosmetics ticknudge off') 1.0 'setup' | Out-Null

    $scanTargets = @($plan.scanTargets)
    if ($scanTargets.Count -eq 0) {
        Write-Host "WARN: plan has no scanTargets - offline plan may be stale; re-run without -SkipOfflinePlan" -ForegroundColor Yellow
    }

    $allMoments = [System.Collections.Generic.List[object]]::new()
    $uniqueDefs = @{}
    $lastTick = -1

    foreach ($tick in $probeTicks) {
        if (-not (Test-Netcon $Port)) { throw "CS2 died at tick $tick" }
        if ($uniqueDefs.Count -ge $StopAfterUniqueWeapons) {
            Write-Host "Enough weapon types ($($uniqueDefs.Count)) - stopping early." -ForegroundColor Green
            break
        }

        Seek-Tick $tick
        $lastTick = $tick
        if (-not (Test-Cs2Netcon $Port)) {
            Export-CrashVehReport -Tag "seek_$tick" -OutDir $outAbs -Port $Port | Out-Null
            throw "CS2 died at tick $tick"
        }
        foreach ($m in (Find-GunHoldersAtTick $tick $scanTargets)) {
            $allMoments.Add($m)
            $uniqueDefs[[string]$m.weaponDefIndex] = $true
        }

        if ($doTransitions -and $allMoments.Count -gt 0) {
            $steam = $allMoments[-1].playerSteamId
            $prevDef = [int]$allMoments[-1].weaponDefIndex
            for ($d = $TransitionStep; $d -le $TransitionWindowTicks; $d += $TransitionStep) {
                NC @("mirv_skip tick $TransitionStep") $(if ($Fast) { 1.0 } else { 2.0 }) "skip" | Out-Null
                Start-Sleep -Milliseconds 200
                $diag = Read-Diag
                if ($diag.steam -ne $steam) { continue }
                $def = if ($diag.defIndex) { [int]$diag.defIndex } else { 0 }
                if ((Get-SkipReason $def)) { continue }
                if ($def -ne $prevDef -and $def -gt 0) {
                    $allMoments.Add((New-RawMoment $diag ($tick + $d) 'weapon_switch' "switched to def $def"))
                    $uniqueDefs["$def"] = $true
                    $prevDef = $def
                }
            }
            Seek-Tick $tick
        }
    }

    Write-Host "Raw moments: $($allMoments.Count), unique defs: $($uniqueDefs.Count)" -ForegroundColor Cyan

    $scanRaw = [ordered]@{
        schemaVersion = 1
        scannedAt = (Get-Date -Format o)
        fastMode = [bool]$Fast
        demo = $demoPlay
        demoName = $script:demoName
        map = $plan.map
        probeTicks = $probeTicks
        moments = @($allMoments)
    }
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($scanRawFile, ($scanRaw | ConvertTo-Json -Depth 6), $utf8NoBom)

    & python (Join-Path $automationRoot 'tools\enrich_scan_moments.py') $scanRawFile
    & python (Join-Path $automationRoot 'tools\build_weapon_moment_run.py') $scanRawFile --out-dir $outAbs

    $mj = Get-Content (Join-Path $outAbs 'moments.json') -Raw | ConvertFrom-Json
    Write-Host "`nDone: $($mj.selectedMomentCount) moments -> verify step" -ForegroundColor Green
    Write-Host "  $(Join-Path $outAbs 'verification_report.txt')" -ForegroundColor DarkGray
    if ($LeaveCs2Open) { Write-Host "  CS2 left open for verify (-NoLaunch)" -ForegroundColor DarkGray }
    exit 0
}
catch {
    Add-Content -LiteralPath $logFile -Value "ERROR $($_.Exception.Message)"
    Export-CrashVehReport -Tag 'scan_fail' -OutDir $outAbs -Port $Port | Out-Null
    Write-Host "SCAN FAIL: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
