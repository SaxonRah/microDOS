param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_nativev2.uf2",
    [int]$CaptureSeconds = 180,
    [switch]$NoFlash,
    [switch]$Interactive
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not $NoFlash) {
    if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }

    Write-Host "=== microDOS NATIVE V2 LIVE-DOS FLASH ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
}
else {
    Write-Host "=== microDOS NATIVE V2 LIVE-DOS RESUME ==="
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
$log = Join-Path $logDir ("nativev2-phase3a-dos-v2-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

$s = [System.IO.Ports.SerialPort]::new(
    $port.DeviceID,115200,[System.IO.Ports.Parity]::None,8,
    [System.IO.Ports.StopBits]::One)
$s.DtrEnable = $true
$s.RtsEnable = $false
$s.ReadTimeout = 50
$s.WriteTimeout = 1000

$enc = [System.Text.UTF8Encoding]::new($false)
$w = [System.IO.StreamWriter]::new($log,$false,$enc)
$w.AutoFlush = $true

$stage = 0
$window = ""
$lastAction = [DateTime]::MinValue
$statsSeen = $false
$dateAnswered = $false
$timeAnswered = $false

Write-Host "log: $log"
if ($Interactive) {
    Write-Host "mode: interactive keyboard forwarding + automatic date/time acceptance"
    Write-Host "Press Ctrl+] for microDOS statistics. Press Ctrl+C to stop capture."
} else {
    Write-Host "auto: accept date/time -> DOS2TEST -> MDSTRESS -> Ctrl+] stats"
}
Write-Host ""

function Send-Raw([string]$text) {
    $s.Write($text)
}

function Send-Dos([string]$cmd) {
    Write-Host ""
    Write-Host ">>> $cmd"
    $s.Write($cmd + "`r")
    $script:lastAction = Get-Date
}

try {
    $s.Open()
    Start-Sleep -Milliseconds 250
    $end = (Get-Date).AddSeconds($CaptureSeconds)

    while ((Get-Date) -lt $end) {
        $chunk = ($s.ReadExisting() -replace "`0","")

        if ($chunk.Length -gt 0) {
            Write-Host -NoNewline $chunk
            $w.Write($chunk)

            $window += $chunk
            if ($window.Length -gt 4000) {
                $window = $window.Substring($window.Length - 4000)
            }

            # DOS 2.x startup date/time questions. A bare CR keeps the value
            # already supplied by the microDOS boot clock.
            if (-not $dateAnswered -and $window -match 'Enter new date:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS date]"
                Send-Raw "`r"
                $dateAnswered = $true
                $window = ""
                $lastAction = Get-Date
                continue
            }

            if (-not $timeAnswered -and $window -match 'Enter new time:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS time]"
                Send-Raw "`r"
                $timeAnswered = $true
                $window = ""
                $lastAction = Get-Date
                continue
            }

            # COMMAND.COM prompt. Accept A>, A:>, A:\>, etc.
            $prompt = [regex]::IsMatch(
                $window,
                '(?im)(^|\r|\n)[A-Z]:(?:\\[^>\r\n]*)?>\s*$|(^|\r|\n)[A-Z]>\s*$'
            )

            if (-not $Interactive -and $prompt) {
                if ($stage -eq 0 -and ((Get-Date)-$lastAction).TotalMilliseconds -gt 300) {
                    Send-Dos "DOS2TEST"
                    $stage = 1
                    $window = ""
                    continue
                }
                elseif ($stage -eq 1 -and ((Get-Date)-$lastAction).TotalMilliseconds -gt 500) {
                    Send-Dos "MDSTRESS"
                    $stage = 2
                    $window = ""
                    continue
                }
                elseif ($stage -eq 2 -and ((Get-Date)-$lastAction).TotalMilliseconds -gt 500) {
                    Write-Host ""
                    Write-Host ">>> Ctrl+] statistics"
                    $s.Write([string][char]0x1D)
                    $stage = 3
                    $window = ""
                    $lastAction = Get-Date
                    continue
                }
            }

            if ($stage -eq 3 -and
                ($chunk -match '\[native-v2\] retired=' -or
                 $window -match '\[native-v2\] retired=')) {
                $statsSeen = $true
            }
        }

        if ($Interactive) {
            # Forward any pending console keystrokes to DOS.
            while ([Console]::KeyAvailable) {
                $k = [Console]::ReadKey($true)

                if (($k.Modifiers -band [ConsoleModifiers]::Control) -and
                    $k.Key -eq [ConsoleKey]::Oem6) {
                    # Ctrl+] is ASCII 0x1D.
                    $s.Write([string][char]0x1D)
                    continue
                }

                switch ($k.Key) {
                    'Enter'     { $s.Write("`r") }
                    'Backspace' { $s.Write([string][char]8) }
                    default {
                        if ($k.KeyChar -ne [char]0) {
                            $s.Write([string]$k.KeyChar)
                        }
                    }
                }
            }
        }

        if ($statsSeen -and -not $Interactive -and
            ((Get-Date)-$lastAction).TotalSeconds -gt 2) {
            break
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
Write-Host "=== CAPTURE COMPLETE ==="
Write-Host "saved: $log"

if (-not $Interactive -and -not $statsSeen) {
    Write-Warning "Automatic sequence did not reach Native v2 statistics before timeout."
    Write-Host "Send the log/output; the last DOS stage is diagnostic."
    exit 2
}

exit 0
