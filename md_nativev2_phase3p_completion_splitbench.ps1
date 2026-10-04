param(
    [string]$Uf2 = ".\\build-pico\\out\\microdos_pico_nativev2.uf2",
    [int]$CaptureSeconds = 300,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\\.pico-sdk\\picotool\\2.3.0\\picotool\\picotool.exe"

if (-not $NoFlash) {
    if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }
    Write-Host "=== microDOS NATIVE V2 PHASE 3P COMPLETION-BOUNDARY BENCH FLASH ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}
    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
} else {
    Write-Host "=== microDOS NATIVE V2 PHASE 3P COMPLETION-BOUNDARY BENCH RESUME ==="
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
$log = Join-Path $logDir ("nativev2-phase3p-completion-splitbench-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

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

# The completion cues occur after the tested program has finished useful work,
# but before COMMAND.COM settles into its keyboard-poll loop.  Ctrl+] is sent
# at that point, so the first COMMAND poll consumes the stats request.
$commands = @(
    @{ cmd = "DOS2TEST"; label = "DOS2TEST #1"; cue = "ALL TESTS PASSED" },
    @{ cmd = "DOS2TEST"; label = "DOS2TEST #2"; cue = "ALL TESTS PASSED" },
    @{ cmd = "DOS2TEST"; label = "DOS2TEST #3"; cue = "ALL TESTS PASSED" },
    @{ cmd = "MDSTRESS"; label = "MDSTRESS AUTO"; cue = "Return to COMMAND.COM." }
)

$window = ""
$dateAnswered = $false
$timeAnswered = $false
$promptPending = $false
$state = "boot"
$commandIndex = -1
$statsLabel = "BOOT"
$statsSawNative = $false
$completionQueued = $false
$lastRx = Get-Date

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

function Request-Stats([string]$label, [string]$why) {
    Marker ("SPLIT INTERVAL: " + $label)
    Write-Host ">>> Ctrl+] statistics ($why)"
    $script:s.Write([string][char]0x1D)
    $script:statsLabel = $label
    $script:statsSawNative = $false
    $script:state = "stats"
    $script:window = ""
    $script:promptPending = $false
    $script:completionQueued = $true
}

function Send-Dos([hashtable]$entry) {
    Marker ("RUN: " + $entry.cmd)
    Write-Host ">>> $($entry.cmd)"
    Write-Host ">>> completion cue: $($entry.cue)"
    $script:s.Write($entry.cmd + "`r")
    $script:window = ""
    $script:promptPending = $false
    $script:completionQueued = $false
    $script:state = "command"
}

Write-Host "log: $log"
Write-Host "auto: BOOT stats -> DOS2TEST x3 -> MDSTRESS AUTO"
Write-Host "Boundary mode: Ctrl+] is queued from program completion text, before COMMAND prompt-idle polling."
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
            if ($window.Length -gt 16000) {
                $window = $window.Substring($window.Length - 16000)
            }

            if (-not $dateAnswered -and $window -match 'Enter new date:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS date]"
                $s.Write("`r")
                $dateAnswered = $true
                $window = ""
                $promptPending = $false
                continue
            }
            if (-not $timeAnswered -and $window -match 'Enter new time:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS time]"
                $s.Write("`r")
                $timeAnswered = $true
                $window = ""
                $promptPending = $false
                continue
            }

            Update-PromptState

            if ($state -eq "command" -and -not $completionQueued -and $commandIndex -ge 0) {
                $entry = $commands[$commandIndex]
                if ($window.Contains([string]$entry.cue)) {
                    Request-Stats ([string]$entry.label) ("completion cue: " + [string]$entry.cue)
                    continue
                }
            }

            if ($state -eq "stats" -and
                ($chunk -match '\[native-v2\] retired=' -or
                 $window -match '\[native-v2\] retired=')) {
                $statsSawNative = $true
            }
        }

        # Boot has no safe completion text; take its snapshot at the first prompt.
        if ($state -eq "boot" -and $promptPending) {
            Request-Stats "BOOT" "first COMMAND prompt"
            continue
        }

        # Once the complete stats block has gone quiet, launch the next command.
        # This dwell is outside the interval that was just closed by Ctrl+].
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

        Start-Sleep -Milliseconds 5
    }
}
finally {
    if ($s.IsOpen) { $s.Close() }
    $s.Dispose()
    $w.Dispose()
}

Write-Host ""
Write-Host "=== COMPLETION-BOUNDARY BENCH COMPLETE ==="
Write-Host "saved: $log"

if ($state -ne "done") {
    Write-Warning "Completion-boundary benchmark did not finish before timeout. Send the log anyway."
    exit 2
}

$raw = Get-Content -Raw -Path $log
$matches = [regex]::Matches(
    $raw,
    '(?s)=== SPLIT INTERVAL: (?<label>[^=\r\n]+) ===.*?\[perf\] --- since previous Ctrl\+\] ---\s*\r?\n' +
    '\[perf\] wall\s+(?<wall>[0-9.]+) s\s+in-guest-loop\s+(?<loop>[0-9.]+) s\s*\r?\n' +
    '\[perf\] active\s+(?<active>[0-9.]+) s\s+idle-sleep\s+(?<idle>[0-9.]+) s.*?\r?\n' +
    '\[perf\] instructions\s+(?<inst>[0-9]+)\s+active\s+(?<mips>[0-9.]+) MIPS\s*\r?\n' +
    '\[perf\] tiers:.*?native-v2\s+(?<native>[0-9]+) \((?<pct>[0-9.]+)%\).*?interpreted\s+(?<interp>[0-9]+)'
)

if ($matches.Count -gt 0) {
    Write-Host ""
    Write-Host "=== COMPLETION-BOUNDARY INTERVAL SUMMARY ==="
    foreach ($m in $matches) {
        Write-Host ("{0,-14} inst={1,10} active={2,7}s idle={3,7}s rate={4,8} MIPS native={5,10} ({6,5}%) interpreted={7,9}" -f `
            $m.Groups['label'].Value.Trim(),
            $m.Groups['inst'].Value,
            $m.Groups['active'].Value,
            $m.Groups['idle'].Value,
            $m.Groups['mips'].Value,
            $m.Groups['native'].Value,
            $m.Groups['pct'].Value,
            $m.Groups['interp'].Value)
    }
}

exit 0
