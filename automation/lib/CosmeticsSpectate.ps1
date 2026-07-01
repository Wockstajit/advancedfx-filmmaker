#requires -Version 5
# Shared spectate + crash.veh helpers for cosmetics automation scripts.

$Script:CosmeticsDbgDir = Join-Path $env:APPDATA 'HLAE\debuglogs'

function Test-Cs2Netcon {
    param(
        [int]$Port = 29010,
        [int]$TimeoutMs = 1500
    )
    $t = [System.Net.Sockets.TcpClient]::new()
    try {
        $c = $t.BeginConnect('127.0.0.1', $Port, $null, $null)
        $ok = $c.AsyncWaitHandle.WaitOne($TimeoutMs)
        if ($ok) { $t.EndConnect($c) }
        return $ok
    }
    catch { return $false }
    finally { $t.Dispose() }
}

function Wait-Cs2Netcon {
    param(
        [int]$Port = 29010,
        [int]$Retries = 20,
        [int]$TimeoutMs = 1500,
        [int]$SleepMs = 250
    )
    for ($i = 0; $i -lt $Retries; $i++) {
        if (Test-Cs2Netcon -Port $Port -TimeoutMs $TimeoutMs) { return $true }
        Start-Sleep -Milliseconds $SleepMs
    }
    return $false
}

function Get-Cs2ProcessAlive {
    return [bool](Get-Process -Name cs2 -ErrorAction SilentlyContinue)
}

function Get-MvmDebugLogFile {
    return Get-ChildItem $Script:CosmeticsDbgDir -Filter 'mvm_debug_*.log' -EA SilentlyContinue |
        Sort-Object LastWriteTime -Desc | Select-Object -First 1
}

function Get-CrashVehLines([string]$LogPath) {
    if (-not $LogPath -or -not (Test-Path -LiteralPath $LogPath)) { return @() }
    return @(Get-Content -LiteralPath $LogPath | Select-String 'crash\.veh')
}

function Get-CrashVehCount {
    $log = Get-MvmDebugLogFile
    if (-not $log) { return 0 }
    return @(Get-CrashVehLines $log.FullName).Count
}

function Export-CrashVehReport {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)][string]$OutDir,
        [int]$Port = 29010,
        [int]$BaselineCount = -1,
        [switch]$FailOnCrash
    )

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $netcon = Test-Cs2Netcon -Port $Port
    $cs2Proc = Get-Cs2ProcessAlive
    $log = Get-MvmDebugLogFile
    $crash = @()
    $logPath = $null
    if ($log) {
        $logPath = $log.FullName
        $crash = @(Get-CrashVehLines $logPath)
        $dest = Join-Path $OutDir ("mvm_debug_{0}.log" -f ($Tag -replace '[^\w.-]', '_'))
        Copy-Item -LiteralPath $logPath -Destination $dest -Force
    }
    $crashCount = $crash.Length
    $crashDelta = if ($BaselineCount -ge 0) { [Math]::Max(0, $crashCount - $BaselineCount) } else { $crashCount }
    $report = [ordered]@{
        tag = $Tag
        at = (Get-Date -Format o)
        netconAlive = $netcon
        cs2ProcessAlive = $cs2Proc
        crashVehCount = $crashCount
        crashVehDelta = $crashDelta
        crashVehBaseline = $(if ($BaselineCount -ge 0) { $BaselineCount } else { $null })
        crashVehLines = @($crash | ForEach-Object { $_.Line })
        mvmLog = $logPath
    }
    $reportFile = Join-Path $OutDir 'crash_report.jsonl'
    ($report | ConvertTo-Json -Depth 6 -Compress) | Add-Content -LiteralPath $reportFile -Encoding UTF8

    $crashForAlert = if ($BaselineCount -ge 0) { $crashDelta } else { $crashCount }
    $likelyCrash = (-not $cs2Proc) -or ($crashForAlert -gt 0)
    if ($likelyCrash -or (-not $netcon -and -not $cs2Proc)) {
        Write-Host ""
        Write-Host "======== CS2 / NETCON PROBLEM ($Tag) ========" -ForegroundColor Red
        Write-Host ("cs2.exe      : {0}" -f $cs2Proc) -ForegroundColor Red
        Write-Host ("netcon port  : {0}" -f $netcon) -ForegroundColor Red
        Write-Host ("crash.veh    : {0} (delta {1})" -f $crashCount, $crashDelta) -ForegroundColor Red
        if ($logPath) { Write-Host ("mvm log      : {0}" -f $logPath) -ForegroundColor DarkGray }
        $crash | Select-Object -Last 8 | ForEach-Object { Write-Host ("  {0}" -f $_.Line) -ForegroundColor Red }
        Write-Host "=============================================" -ForegroundColor Red
        if ($FailOnCrash) { throw "crash.veh=$crashCount cs2=$cs2Proc netcon=$netcon at $Tag" }
    }

    return $report
}

function Test-PlayerDeadFromDiag {
    param([hashtable]$Diag)
    if ($Diag.isDead) { return $true }
    if ($Diag.health -and [int]$Diag.health -le 0) { return $true }
    if (-not $Diag.steam -or "$($Diag.steam)" -eq '0') { return $true }
    $raw = "$($Diag.raw)"
    if ($raw -match 'is dead \(health=') { return $true }
    if ($raw -match 'no spectated pawn|pawn=\d+ not resolvable') { return $true }
    return $false
}

function Get-VisualDiagPaint {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $m = [regex]::Matches($Text, 'paint\(def6\)=(\d+)')
    if ($m.Count) { return $m[$m.Count - 1].Groups[1].Value }
    $m2 = [regex]::Matches($Text, 'fallback: paint=(\d+)')
    if ($m2.Count) { return $m2[$m2.Count - 1].Groups[1].Value }
    return $null
}

function Test-VisualDiagHasNetworkedPaint {
    param([string]$Text)
    return ($Text -match 'paint\(def6\)=(\d+)')
}

function Resolve-WeaponPaintSteamId {
    param(
        [hashtable]$Diag,
        [string]$SpectateSteam = ''
    )
    $spectate = if ($SpectateSteam) { "$SpectateSteam" } else { "$($Diag.steam)" }
    $owner = if ($Diag.ownerXuid) { "$($Diag.ownerXuid)" } else { '' }
    $pickup = ($owner -and $spectate -and $owner -ne '0' -and $spectate -ne '0' -and $owner -ne $spectate)
    $paintSteam = if ($pickup) { $owner } else { $spectate }
    return @{
        paintSteam = $paintSteam
        spectateSteam = $spectate
        ownerSteam = $owner
        pickup = [bool]$pickup
    }
}

function Read-WeaponDiag {
    param(
        [int]$ExpectedDef,
        [string]$SpectateSteam,
        [scriptblock]$DiagReader,
        [scriptblock]$SpectateFn,
        [int]$Retries = 8,
        [int]$SleepMs = 200
    )
    for ($i = 0; $i -lt $Retries; $i++) {
        if ($i -gt 0 -and $SpectateSteam) {
            & $SpectateFn $SpectateSteam | Out-Null
            Start-Sleep -Milliseconds $SleepMs
        }
        $d = & $DiagReader
        if ($d.isDead) { return $d }
        if ($ExpectedDef -le 0) { return $d }
        if ($d.defIndex -and [int]$d.defIndex -eq $ExpectedDef) { return $d }
        Start-Sleep -Milliseconds $SleepMs
    }
    return (& $DiagReader)
}

function Invoke-CosmeticsSpectate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SteamId,
        [Parameter(Mandatory = $true)][scriptblock]$Netcon
    )
    $text = & $Netcon @("mirv_filmmaker cosmetics spectate $SteamId") 0.9 "spec_$SteamId"
    $dead = ($text -match 'spectate: steam=\d+ dead')
    $ok = ($text -match 'spectate: steam=\d+ specIndex=\d+ health=\d+ ok')
    return @{ ok = $ok; dead = $dead; raw = $text }
}

function Lock-PlayerBySteam {
    param(
        [Parameter(Mandatory = $true)][string]$SteamId,
        [hashtable]$SlotMap = @{},
        [Parameter(Mandatory = $true)][scriptblock]$Netcon,
        [scriptblock]$ParseDiag = {},
        [int]$SpecSleepMs = 0
    )
    $r = Invoke-CosmeticsSpectate -SteamId $SteamId -Netcon $Netcon
    return $r.ok
}
