param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_nativev2.uf2",
    [int]$CaptureSeconds = 300,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not $NoFlash) {
    if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }
    Write-Host "=== microDOS NATIVE V2 PHASE 3P IMMEDIATE SPLIT BENCH FLASH ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}
    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
} else {
    Write-Host "=== microDOS NATIVE V2 PHASE 3P IMMEDIATE SPLIT BENCH RESUME ==="
}

$deadline = (Get-Date).AddSeconds(20)
$port = $null
do {
    $port = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue) |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -and
            $_.PNPDeviceID -match 'PID_0009|PID_000A'
        } |
        Select-Object -First 1
    if (-not $port) { Start-Sleep -Milliseconds 250 }
} while (-not $port -and (Get-Date) -lt $deadline)

if (-not $port) { throw "Pico SDK CDC (PID 0009/000A) not found" }

Write-Host "Pico application port: $($port.DeviceID)"
Write-Host "PNP: $($port.PNPDeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ("nativev2-phase3p-immediate-splitbench-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

$s = [System.IO.Ports.SerialPort]::new(
    $port.DeviceID,115200,[System.IO.Ports.Parity]::None,8,
    [System.IO.Ports.StopBits]::One)
$s.DtrEnable = $true
$s.RtsEnable = $false
$s.ReadTimeout = 50
$s.WriteTimeout = 1000

$w = [System.IO.StreamWriter]::new(
    $log,$false,[System.Text.UTF8Encoding]::new($false))
$w.AutoFlush = $true

$commands = @("DOS2TEST", "DOS2TEST", "DOS2TEST", "MDSTRESS")
$labels = @("BOOT", "DOS2TEST #1", "DOS2TEST #2", "DOS2TEST #3", "MDSTRESS AUTO")
$window = ""
$dateAnswered = $false
$timeAnswered = $false
$promptPending = $false
$state = "boot"
$commandIndex = -1
$statsIndex = 0
$statsSawNative = $false
$lastRx = Get-Date
$lastAction = [DateTime]::MinValue

function Marker([string]$text) {
    $line = "`r`n=== $text ===`r`n"
    Write-Host ""
    Write-Host "=== $text ==="
    $script:w.Write($line)
}

function Update-PromptState {
    $script:promptPending = [regex]::IsMatch(
        $script:window,
        '(?im)(^|\r|\n)[A-Z]:(?:\\[^>\r\n]*)?>\s*$|(^|\r|\n)[A-Z]>\s*$'
    )
}

function Request-Stats([string]$label) {
    Marker ("SPLIT INTERVAL: " + $label)
    Write-Host ">>> Ctrl+] statistics"
    $script:s.Write([string][char]0x1D)
    $script:statsSawNative = $false
    $script:state = "stats"
    $script:lastAction = Get-Date
    $script:window = ""
    $script:promptPending = $false
}

function Send-Dos([string]$cmd) {
    Marker ("RUN: " + $cmd)
    Write-Host ">>> $cmd"
    $script:s.Write($cmd + "`r")
    $script:lastAction = Get-Date
    $script:window = ""
    $script:promptPending = $false
    $script:state = "command"
}

Write-Host "log: $log"
Write-Host "auto: BOOT stats -> DOS2TEST x3 stats -> MDSTRESS AUTO stats"
Write-Host "Immediate-boundary runner: no intentional 250/350/750 ms prompt dwell."
Write-Host "Use with the production Phase 3P firmware/build."
Write-Host ""

try {
    $s.Open()
    Start-Sleep -Milliseconds 250
    $end = (Get-Date).AddSeconds($CaptureSeconds)

    while ((Get-Date) -lt $end -and $state -ne "done") {
        $chunk = ($s.ReadExisting() -replace "`0","")
        if ($chunk.Length -gt 0) {
            $lastRx = Get-Date
            Write-Host -NoNewline $chunk
            $w.Write($chunk)
            $window += $chunk
            if ($window.Length -gt 12000) {
                $window = $window.Substring($window.Length - 12000)
            }

            if (-not $dateAnswered -and $window -match 'Enter new date:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS date]"
                $s.Write("`r")
                $dateAnswered = $true
                $window = ""
                $promptPending = $false
                $lastAction = Get-Date
                continue
            }
            if (-not $timeAnswered -and $window -match 'Enter new time:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS time]"
                $s.Write("`r")
                $timeAnswered = $true
                $window = ""
                $promptPending = $false
                $lastAction = Get-Date
                continue
            }

            Update-PromptState
            if ($state -eq "stats" -and
                ($chunk -match '\[native-v2\] retired=' -or
                 $window -match '\[native-v2\] retired=')) {
                $statsSawNative = $true
            }
        }

        if ($state -eq "boot" -and $promptPending) {
            Request-Stats $labels[0]
            continue
        }

        if ($state -eq "command" -and $promptPending) {
            ++$statsIndex
            Request-Stats $labels[$statsIndex]
            continue
        }

        if ($state -eq "stats" -and $statsSawNative -and
            ((Get-Date)-$lastRx).TotalMilliseconds -ge 40) {
            if ($commandIndex -ge ($commands.Count - 1)) {
                $state = "done"
                break
            }
            ++$commandIndex
            Send-Dos $commands[$commandIndex]
            continue
        }

        Start-Sleep -Milliseconds 10
    }
}
finally {
    if ($s.IsOpen) { $s.Close() }
    $s.Dispose()
    $w.Dispose()
}

Write-Host ""
Write-Host "=== IMMEDIATE SPLIT BENCH COMPLETE ==="
Write-Host "saved: $log"

if ($state -ne "done") {
    Write-Warning "Immediate split benchmark did not finish before timeout. Send the log anyway."
    exit 2
}

# Print a compact summary from each interval's 'since previous Ctrl+]' block.
$raw = Get-Content -Raw -Path $log
$matches = [regex]::Matches(
    $raw,
    '(?s)=== SPLIT INTERVAL: (?<label>[^=\r\n]+) ===.*?\[perf\] --- since previous Ctrl\+\] ---\s*\r?\n' +
    '\[perf\] wall\s+(?<wall>[0-9.]+) s\s+in-guest-loop\s+(?<loop>[0-9.]+) s\s*\r?\n' +
    '\[perf\] active\s+(?<active>[0-9.]+) s.*?\r?\n' +
    '\[perf\] instructions\s+(?<inst>[0-9]+)\s+active\s+(?<mips>[0-9.]+) MIPS\s*\r?\n' +
    '\[perf\] tiers:.*?native-v2\s+(?<native>[0-9]+) \((?<pct>[0-9.]+)%\).*?interpreted\s+(?<interp>[0-9]+)'
)

if ($matches.Count -gt 0) {
    Write-Host ""
    Write-Host "=== IMMEDIATE INTERVAL SUMMARY ==="
    foreach ($m in $matches) {
        Write-Host ("{0,-14} inst={1,10} active={2,7}s rate={3,8} MIPS native={4,10} ({5,5}%) interpreted={6,9}" -f `
            $m.Groups['label'].Value.Trim(),
            $m.Groups['inst'].Value,
            $m.Groups['active'].Value,
            $m.Groups['mips'].Value,
            $m.Groups['native'].Value,
            $m.Groups['pct'].Value,
            $m.Groups['interp'].Value)
    }
}

exit 0
