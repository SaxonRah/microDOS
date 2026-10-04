param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_nativev2.uf2",
    [int]$CaptureSeconds = 600,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not $NoFlash) {
    if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }

    Write-Host "=== microDOS NATIVE V2 REMAINING-WORK PROFILE FLASH ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
} else {
    Write-Host "=== microDOS NATIVE V2 REMAINING-WORK PROFILE RESUME ==="
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
$log = Join-Path $logDir ("nativev2-remaining-profile-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

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

$commands = @(
    "DOS2TEST",
    "MDSTRESS 1",
    "MDSTRESS 2",
    "MDSTRESS 3",
    "MDSTRESS 4",
    "MDSTRESS 5",
    "MDSTRESS 6",
    "MDSTRESS 7",
    "MDSTRESS 8",
    "MDSTRESS 9"
)

$window = ""
$dateAnswered = $false
$timeAnswered = $false
$promptPending = $false
$commandIndex = -1
$state = "initial-prompt"       # initial-prompt, stats, command, done
$currentLabel = ""
$statsSawNative = $false
$lastRx = Get-Date
$lastAction = [DateTime]::MinValue

function Write-Marker([string]$text) {
    $line = "`r`n=== $text ===`r`n"
    Write-Host ""
    Write-Host "=== $text ==="
    $script:w.Write($line)
}

function Send-Dos([string]$cmd) {
    Write-Marker ("PROFILE COMMAND: " + $cmd)
    $script:s.Write($cmd + "`r")
    $script:lastAction = Get-Date
    $script:promptPending = $false
    $script:window = ""
}

function Request-Stats([string]$label) {
    Write-Marker ("PROFILE INTERVAL: " + $label)
    Write-Host ">>> Ctrl+] statistics"
    $script:s.Write([string][char]0x1D)
    $script:currentLabel = $label
    $script:statsSawNative = $false
    $script:state = "stats"
    $script:lastAction = Get-Date
    $script:window = ""
    $script:promptPending = $false
}

function Update-PromptState {
    $script:promptPending = [regex]::IsMatch(
        $script:window,
        '(?im)(^|\r|\n)[A-Z]:(?:\\[^>\r\n]*)?>\s*$|(^|\r|\n)[A-Z]>\s*$'
    )
}

Write-Host "log: $log"
Write-Host "auto: baseline -> DOS2TEST -> MDSTRESS 1..9, Ctrl+] after every interval"
Write-Host "The firmware is unchanged; this run only measures where the remaining interpreted work lives."
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

        if ($state -eq "initial-prompt" -and $promptPending -and
            ((Get-Date)-$lastAction).TotalMilliseconds -ge 250) {
            Request-Stats "BOOT BASELINE"
            continue
        }

        if ($state -eq "command" -and $promptPending -and
            ((Get-Date)-$lastAction).TotalMilliseconds -ge 250) {
            Request-Stats $commands[$commandIndex]
            continue
        }

        # The native-v2 summary line appears before the resident/reject slot dump.
        # Wait for a quiet period so the next DOS command cannot interleave with it.
        if ($state -eq "stats" -and $statsSawNative -and
            ((Get-Date)-$lastRx).TotalMilliseconds -ge 750) {

            if ($commandIndex -ge ($commands.Count - 1)) {
                $state = "done"
                break
            }

            ++$commandIndex
            Send-Dos $commands[$commandIndex]
            $state = "command"
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
Write-Host ""
Write-Host "=== PROFILE CAPTURE COMPLETE ==="
Write-Host "saved: $log"

if ($state -ne "done") {
    Write-Warning "Profile did not complete before timeout. Send the log anyway; completed intervals are still useful."
    exit 2
}

Write-Host "Completed intervals: BOOT BASELINE, DOS2TEST, MDSTRESS 1..9"
Write-Host "Send this log back and the next Native-v2 target can be chosen from measured interpreted retirement."
exit 0
