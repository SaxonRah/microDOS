param(
    [int]$CaptureSeconds = 240,
    [switch]$NoBuild,
    [switch]$NoFlash,
    [switch]$SkipStress
)

$ErrorActionPreference = "Stop"

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $Repo "build-pico\out"
$Target = "microdos_pico_nativev2g"
$Uf2 = Join-Path $BuildDir "$Target.uf2"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

function Get-LastRegexValue([string]$text, [string]$pattern, [int]$group) {
    $m = [regex]::Matches($text, $pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($m.Count -eq 0) { return $null }
    return $m[$m.Count - 1].Groups[$group].Value
}

function Parse-StatsBlock([string]$name, [string]$text) {
    $instructions = Get-LastRegexValue $text '^\[perf\] instructions\s+(\d+)\s+active\s+([0-9.]+)\s+MIPS' 1
    $mips = Get-LastRegexValue $text '^\[perf\] instructions\s+(\d+)\s+active\s+([0-9.]+)\s+MIPS' 2
    $active = Get-LastRegexValue $text '^\[perf\] active\s+([0-9.]+)\s+s' 1
    $tierNative = Get-LastRegexValue $text '^\[perf\] tiers:.*native-v2\s+(\d+)\s+\(([0-9.]+)%\)' 1
    $tierNativePct = Get-LastRegexValue $text '^\[perf\] tiers:.*native-v2\s+(\d+)\s+\(([0-9.]+)%\)' 2
    $tierInterp = Get-LastRegexValue $text '^\[perf\] tiers:.*interpreted\s+(\d+)\s+\(([0-9.]+)%\)' 1
    $tierInterpPct = Get-LastRegexValue $text '^\[perf\] tiers:.*interpreted\s+(\d+)\s+\(([0-9.]+)%\)' 2

    $nv2Retired = Get-LastRegexValue $text '^\[native-v2\]\s+retired=(\d+)\s+entries=(\d+).*compiles=(\d+)' 1
    $nv2Entries = Get-LastRegexValue $text '^\[native-v2\]\s+retired=(\d+)\s+entries=(\d+).*compiles=(\d+)' 2
    $nv2Compiles = Get-LastRegexValue $text '^\[native-v2\]\s+retired=(\d+)\s+entries=(\d+).*compiles=(\d+)' 3

    $gSlots = [regex]::Matches(
        $text,
        '^\[native-v2\]\s+slot\d+\s+\S+\s+bytes=\d+\s+ops=\d+\s+phase=17\s+dyn=3\b.*entries=(\d+)\s+retired=(\d+)',
        [System.Text.RegularExpressions.RegexOptions]::Multiline
    )

    [uint64]$gEntries = 0
    [uint64]$gRetired = 0
    foreach ($m in $gSlots) {
        $gEntries += [uint64]$m.Groups[1].Value
        $gRetired += [uint64]$m.Groups[2].Value
    }

    return [pscustomobject]@{
        Name            = $name
        Instructions    = if ($instructions) { [uint64]$instructions } else { 0 }
        ActiveSeconds   = if ($active) { [double]$active } else { 0.0 }
        MIPS            = if ($mips) { [double]$mips } else { 0.0 }
        NativeRetired   = if ($tierNative) { [uint64]$tierNative } else { 0 }
        NativePct       = if ($tierNativePct) { [double]$tierNativePct } else { 0.0 }
        InterpRetired   = if ($tierInterp) { [uint64]$tierInterp } else { 0 }
        InterpPct       = if ($tierInterpPct) { [double]$tierInterpPct } else { 0.0 }
        Nv2CumRetired   = if ($nv2Retired) { [uint64]$nv2Retired } else { 0 }
        Nv2CumEntries   = if ($nv2Entries) { [uint64]$nv2Entries } else { 0 }
        Nv2CumCompiles  = if ($nv2Compiles) { [uint64]$nv2Compiles } else { 0 }
        GResidentSlots  = $gSlots.Count
        GResidentEntries= $gEntries
        GResidentRetired= $gRetired
    }
}

Push-Location $Repo
try {
    if (-not $NoBuild) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "BUILD NATIVE-V2G SPLITBENCH"
        Write-Host "============================================================"
        & cmake --build $BuildDir --target $Target
        if ($LASTEXITCODE -ne 0) {
            throw "Pico build failed: $LASTEXITCODE"
        }
    }

    if (-not (Test-Path $Uf2)) {
        throw "UF2 not found: $Uf2"
    }

    if (-not $NoFlash) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "FLASH NATIVE-V2G SPLITBENCH"
        Write-Host "============================================================"

        try {
            & $Picotool reboot -f -u 2>$null
        }
        catch {
            # Fine if board is already in BOOTSEL mode.
        }

        Start-Sleep -Milliseconds 500

        & $Picotool load -v -x $Uf2
        if ($LASTEXITCODE -ne 0) {
            throw "picotool load failed: $LASTEXITCODE"
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "WAITING FOR PICO USB CDC"
    Write-Host "============================================================"

    $deadline = (Get-Date).AddSeconds(20)
    $port = $null

    do {
        $port = @(
            Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
        ) |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -and
            $_.PNPDeviceID -match 'PID_0009|PID_000A'
        } |
        Select-Object -First 1

        if (-not $port) {
            Start-Sleep -Milliseconds 50
        }
    }
    while (-not $port -and (Get-Date) -lt $deadline)

    if (-not $port) {
        throw "Pico SDK CDC port not found"
    }

    Write-Host "Pico application port: $($port.DeviceID)"

    $LogDir = Join-Path $Repo "logs"
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $Log = Join-Path $LogDir "pico-nativev2g-splitbench-$stamp.txt"
    $Csv = Join-Path $LogDir "pico-nativev2g-splitbench-$stamp.csv"

    $s = [System.IO.Ports.SerialPort]::new(
        $port.DeviceID,
        115200,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )

    $s.DtrEnable = $true
    $s.RtsEnable = $false
    $s.ReadTimeout = 50
    $s.WriteTimeout = 1000

    $w = [System.IO.StreamWriter]::new(
        $Log,
        $false,
        [System.Text.UTF8Encoding]::new($false)
    )
    $w.AutoFlush = $true

    $window = ""
    $dateAnswered = $false
    $timeAnswered = $false
    $dos2Pass = $false
    $stressDone = $false

    $stage = "WAIT_BOOT"
    $lastAction = [DateTime]::MinValue
    $lastRx = Get-Date

    $awaitingStats = $false
    $statsName = ""
    $statsBuffer = [System.Text.StringBuilder]::new()
    $snapshots = New-Object System.Collections.Generic.List[object]

    function Write-Marker([string]$text) {
        $line = "`r`n===== $text =====`r`n"
        Write-Host ""
        Write-Host "===== $text ====="
        $w.Write($line)
    }

    function Send-Dos([string]$cmd) {
        Write-Host ""
        Write-Host ">>> $cmd"
        $w.Write("`r`n>>> $cmd`r`n")
        $s.Write($cmd + "`r")
        $script:lastAction = Get-Date
        $script:window = ""
    }

    function Start-Snapshot([string]$name) {
        Write-Marker "SNAPSHOT $name"
        Write-Host ">>> Ctrl+] statistics"
        $w.Write(">>> Ctrl+] statistics`r`n")
        $script:statsName = $name
        $script:statsBuffer.Clear() | Out-Null
        $script:awaitingStats = $true
        $script:window = ""
        $script:lastAction = Get-Date
        $s.Write([string][char]0x1D)
    }

    function Snapshot-Complete {
        if (-not $script:awaitingStats) { return $false }
        $txt = $script:statsBuffer.ToString()
        if ($txt -notmatch '(?m)^\[native-v2\]\s+retired=') {
            return $false
        }
        return (((Get-Date) - $script:lastRx).TotalMilliseconds -ge 350)
    }

    function Finish-Snapshot {
        $txt = $script:statsBuffer.ToString()
        $row = Parse-StatsBlock $script:statsName $txt
        $script:snapshots.Add($row)

        Write-Host ""
        Write-Host ("[{0}] instructions={1} active={2:N3}s rate={3:N3} MIPS native={4:N1}% interp={5:N1}% NV2cum={6} compiles={7} Gslots={8}" -f `
            $row.Name,
            $row.Instructions,
            $row.ActiveSeconds,
            $row.MIPS,
            $row.NativePct,
            $row.InterpPct,
            $row.Nv2CumRetired,
            $row.Nv2CumCompiles,
            $row.GResidentSlots)

        $script:awaitingStats = $false
        $script:statsName = ""
        $script:statsBuffer.Clear() | Out-Null
        $script:window = ""
        $script:lastAction = Get-Date
    }

    Write-Host "log: $Log"
    Write-Host "csv: $Csv"
    Write-Host "auto: boot snapshot -> DOS2TEST snapshot -> MDSTRESS snapshot"
    Write-Host ""

    try {
        $s.Open()
        $end = (Get-Date).AddSeconds($CaptureSeconds)

        while ((Get-Date) -lt $end) {
            $chunk = ($s.ReadExisting() -replace "`0", "")

            if ($chunk.Length -gt 0) {
                $lastRx = Get-Date

                Write-Host -NoNewline $chunk
                $w.Write($chunk)

                if ($awaitingStats) {
                    $statsBuffer.Append($chunk) | Out-Null
                }

                $window += $chunk
                if ($window.Length -gt 16000) {
                    $window = $window.Substring($window.Length - 16000)
                }

                if ($window -match 'ALL TESTS PASSED') {
                    $dos2Pass = $true
                }

                if ($window -match 'MDSTRESS complete\. deterministic checksum = 0xA298') {
                    $stressDone = $true
                }

                if (-not $dateAnswered -and
                    $window -match 'Enter new date:\s*$') {

                    Write-Host ""
                    Write-Host ">>> [accept current DOS date]"
                    $w.Write("`r`n>>> [accept current DOS date]`r`n")
                    $s.Write("`r")

                    $dateAnswered = $true
                    $window = ""
                    $lastAction = Get-Date
                    continue
                }

                if (-not $timeAnswered -and
                    $window -match 'Enter new time:\s*$') {

                    Write-Host ""
                    Write-Host ">>> [accept current DOS time]"
                    $w.Write("`r`n>>> [accept current DOS time]`r`n")
                    $s.Write("`r")

                    $timeAnswered = $true
                    $window = ""
                    $lastAction = Get-Date
                    continue
                }
            }

            if (Snapshot-Complete) {
                Finish-Snapshot

                switch ($stage) {
                    "WAIT_BOOT_STATS" {
                        Send-Dos "DOS2TEST"
                        $stage = "WAIT_DOS2"
                    }
                    "WAIT_DOS2_STATS" {
                        if ($SkipStress) {
                            $stage = "DONE"
                        }
                        else {
                            Send-Dos "MDSTRESS"
                            $stage = "WAIT_STRESS"
                        }
                    }
                    "WAIT_STRESS_STATS" {
                        $stage = "DONE"
                    }
                }
            }

            $prompt = [regex]::IsMatch(
                $window,
                '(?im)(^|\r|\n)[A-Z]:(?:\\[^>\r\n]*)?>\s*$|(^|\r|\n)[A-Z]>\s*$'
            )

            if (-not $awaitingStats -and $prompt) {
                $ageMs = ((Get-Date) - $lastAction).TotalMilliseconds

                switch ($stage) {
                    "WAIT_BOOT" {
                        if ($ageMs -ge 250) {
                            Start-Snapshot "BOOT"
                            $stage = "WAIT_BOOT_STATS"
                        }
                    }
                    "WAIT_DOS2" {
                        if ($ageMs -ge 500) {
                            Start-Snapshot "DOS2TEST"
                            $stage = "WAIT_DOS2_STATS"
                        }
                    }
                    "WAIT_STRESS" {
                        if ($ageMs -ge 500) {
                            Start-Snapshot "MDSTRESS"
                            $stage = "WAIT_STRESS_STATS"
                        }
                    }
                }
            }

            if ($stage -eq "DONE") {
                break
            }

            Start-Sleep -Milliseconds 10
        }
    }
    finally {
        if ($s.IsOpen) {
            $s.Close()
        }
        $s.Dispose()
        $w.Dispose()
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "NATIVE-V2G SPLITBENCH RESULT"
    Write-Host "============================================================"

    if ($snapshots.Count -gt 0) {
        $snapshots |
            Select-Object Name, Instructions, ActiveSeconds, MIPS,
                          NativeRetired, NativePct, InterpRetired, InterpPct,
                          Nv2CumRetired, Nv2CumEntries, Nv2CumCompiles,
                          GResidentSlots, GResidentEntries, GResidentRetired |
            Format-Table -AutoSize

        $snapshots | Export-Csv -NoTypeInformation -Encoding UTF8 $Csv
    }

    Write-Host ""
    if ($dos2Pass) {
        Write-Host "DOS2TEST: PASS"
    }
    else {
        Write-Host "DOS2TEST: NOT CONFIRMED"
    }

    if (-not $SkipStress) {
        if ($stressDone) {
            Write-Host "MDSTRESS: PASS checksum 0xA298"
        }
        else {
            Write-Host "MDSTRESS: NOT CONFIRMED"
        }
    }

    if ($snapshots.Count -ge 2) {
        $boot = $snapshots[0]
        $dos2 = $snapshots[1]

        $dos2Nv2Delta = [int64]$dos2.Nv2CumRetired - [int64]$boot.Nv2CumRetired
        $dos2CompileDelta = [int64]$dos2.Nv2CumCompiles - [int64]$boot.Nv2CumCompiles

        Write-Host ("DOS2TEST NV2 cumulative delta: retired={0} compiles={1}" -f `
            $dos2Nv2Delta, $dos2CompileDelta)
    }

    if (-not $SkipStress -and $snapshots.Count -ge 3) {
        $dos2 = $snapshots[1]
        $stress = $snapshots[2]

        $stressNv2Delta = [int64]$stress.Nv2CumRetired - [int64]$dos2.Nv2CumRetired
        $stressCompileDelta = [int64]$stress.Nv2CumCompiles - [int64]$dos2.Nv2CumCompiles

        Write-Host ("MDSTRESS NV2 cumulative delta: retired={0} compiles={1}" -f `
            $stressNv2Delta, $stressCompileDelta)
        Write-Host ("MDSTRESS interval rate: {0:N3} MIPS" -f $stress.MIPS)
    }

    Write-Host "log: $Log"
    Write-Host "csv: $Csv"

    if (-not $dos2Pass) {
        exit 2
    }
    if (-not $SkipStress -and -not $stressDone) {
        exit 3
    }
    if (($SkipStress -and $snapshots.Count -lt 2) -or
        (-not $SkipStress -and $snapshots.Count -lt 3)) {
        exit 4
    }

    exit 0
}
finally {
    Pop-Location
}
