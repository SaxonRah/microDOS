param(
    [Parameter(Mandatory=$true)]
    [string]$Uf2,

    [string]$Label = "engine",

    [int]$CaptureSeconds = 300,

    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not (Test-Path $Uf2)) {
    throw "UF2 not found: $Uf2"
}

if (-not $NoFlash) {
    if (-not (Test-Path $picotool)) {
        throw "picotool not found: $picotool"
    }

    Write-Host "=== microDOS ENGINE SPLITBENCH FLASH: $Label ==="

    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) {
        throw "picotool load failed: $LASTEXITCODE"
    }

    Start-Sleep -Milliseconds 1000
} else {
    Write-Host "=== microDOS ENGINE SPLITBENCH RESUME: $Label ==="
}

# Use the same strict Raspberry Pi Pico SDK CDC selection as the known-good
# production Phase3P runner. This intentionally avoids generic COM devices.
$deadline = (Get-Date).AddSeconds(20)
$port = $null

do {
    $port = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue) |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -and
            $_.PNPDeviceID -match 'PID_0009|PID_000A'
        } |
        Select-Object -First 1

    if (-not $port) {
        Start-Sleep -Milliseconds 250
    }
} while (-not $port -and (Get-Date) -lt $deadline)

if (-not $port) {
    Write-Host "Available serial ports:"
    Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Format-Table DeviceID, Name, PNPDeviceID -AutoSize
    throw "Pico SDK CDC (VID_2E8A, PID 0009/000A) not found"
}

Write-Host "Pico application port: $($port.DeviceID)"
Write-Host "PNP: $($port.PNPDeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$safeLabel = ($Label -replace '[^A-Za-z0-9_.-]', '_')
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logDir ("engine-splitbench-$safeLabel-$stamp.txt")
$csv = Join-Path $logDir ("engine-splitbench-$safeLabel-$stamp.csv")

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
    $log,
    $false,
    [System.Text.UTF8Encoding]::new($false)
)
$w.AutoFlush = $true

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
$statsSawFooter = $false
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
    $script:statsSawFooter = $false
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

Write-Host "label: $Label"
Write-Host "UF2:   $Uf2"
Write-Host "log:   $log"
Write-Host "auto: BOOT stats -> DOS2TEST x3 -> MDSTRESS AUTO"
Write-Host "Boundary mode: Ctrl+] is queued at program completion."
Write-Host "Generic engine mode: stats completion uses the common [perf] AOT footer."
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
            if ($window.Length -gt 24000) {
                $window = $window.Substring($window.Length - 24000)
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

            if ($state -eq "command" -and
                -not $completionQueued -and
                $commandIndex -ge 0) {
                $entry = $commands[$commandIndex]

                if ($window.Contains([string]$entry.cue)) {
                    Request-Stats ([string]$entry.label) `
                        ("completion cue: " + [string]$entry.cue)
                    continue
                }
            }

            # This line is common to the Pico DOS frontend regardless of
            # interpreter/AOT/JIT/Native-v2 configuration. Engine-specific
            # telemetry may follow it, so we still require a quiet dwell.
            if ($state -eq "stats" -and
                ($chunk -match '\[perf\] aot: attaches=' -or
                 $window -match '\[perf\] aot: attaches=')) {
                $statsSawFooter = $true
            }
        }

        # Boot has no program completion cue.
        if ($state -eq "boot" -and $promptPending) {
            Request-Stats "BOOT" "first COMMAND prompt"
            continue
        }

        # Wait for all optional JIT/Native-v2 telemetry to drain after the
        # common footer before launching the next workload.
        if ($state -eq "stats" -and
            $statsSawFooter -and
            ((Get-Date)-$lastRx).TotalMilliseconds -ge 120) {

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
Write-Host "=== ENGINE SPLITBENCH COMPLETE: $Label ==="
Write-Host "saved: $log"

if ($state -ne "done") {
    Write-Warning "Benchmark did not finish before timeout."
    exit 2
}

$raw = Get-Content -Raw -Path $log

$matches = [regex]::Matches(
    $raw,
    '(?s)=== SPLIT INTERVAL: (?<label>[^=\r\n]+) ===.*?' +
    '\[perf\] --- since previous Ctrl\+\] ---\s*\r?\n' +
    '\[perf\] wall\s+(?<wall>[0-9.]+) s\s+in-guest-loop\s+(?<loop>[0-9.]+) s\s*\r?\n' +
    '\[perf\] active\s+(?<active>[0-9.]+) s\s+idle-sleep\s+(?<idle>[0-9.]+) s.*?\r?\n' +
    '\[perf\] instructions\s+(?<inst>[0-9]+)\s+active\s+(?<mips>[0-9.]+) MIPS\s*\r?\n' +
    '\[perf\] tiers:\s+static-aot\s+(?<aot>[0-9]+) \((?<aotpct>[0-9.]+)%\)\s+' +
    'old-jit\s+(?<jit>[0-9]+) \((?<jitpct>[0-9.]+)%\)\s+' +
    'native-v2\s+(?<native>[0-9]+) \((?<nativepct>[0-9.]+)%\)\s+' +
    'interpreted\s+(?<interp>[0-9]+) \((?<interppct>[0-9.]+)%\)'
)

if ($matches.Count -eq 0) {
    Write-Warning "No interval summaries parsed from $log"
    exit 3
}

$rows = @()

Write-Host ""
Write-Host "=== ENGINE INTERVAL SUMMARY: $Label ==="

foreach ($m in $matches) {
    $row = [pscustomobject]@{
        Engine       = $Label
        Interval     = $m.Groups['label'].Value.Trim()
        Instructions = [uint64]$m.Groups['inst'].Value
        ActiveSec    = [double]$m.Groups['active'].Value
        IdleSec      = [double]$m.Groups['idle'].Value
        MIPS         = [double]$m.Groups['mips'].Value
        AOTPct       = [double]$m.Groups['aotpct'].Value
        JITPct       = [double]$m.Groups['jitpct'].Value
        NativePct    = [double]$m.Groups['nativepct'].Value
        InterpPct    = [double]$m.Groups['interppct'].Value
        AOT          = [uint64]$m.Groups['aot'].Value
        JIT          = [uint64]$m.Groups['jit'].Value
        Native       = [uint64]$m.Groups['native'].Value
        Interpreted  = [uint64]$m.Groups['interp'].Value
    }

    $rows += $row

    Write-Host (
        "{0,-14} inst={1,10} rate={2,8:N3} MIPS  AOT={3,5:N1}% JIT={4,5:N1}% NV2={5,5:N1}% INT={6,5:N1}%" -f `
        $row.Interval,
        $row.Instructions,
        $row.MIPS,
        $row.AOTPct,
        $row.JITPct,
        $row.NativePct,
        $row.InterpPct
    )
}

$rows | Export-Csv -NoTypeInformation -Encoding UTF8 $csv

Write-Host ""
Write-Host "CSV: $csv"
exit 0
