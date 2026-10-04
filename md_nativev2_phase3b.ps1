param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_nativev2.uf2",
    [int]$CaptureSeconds = 180
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }

Write-Host "=== microDOS NATIVE V2 PHASE 3B ==="
try {
    & $picotool reboot -f -u
    Start-Sleep -Milliseconds 1000
} catch {}

& $picotool load -v -x $Uf2
if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
Start-Sleep -Milliseconds 1000

$deadline = (Get-Date).AddSeconds(20)
$port = $null
do {
    $port = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue) |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -and
            $_.PNPDeviceID -match 'PID_0009|PID_000A'
        } | Select-Object -First 1
    if (-not $port) { Start-Sleep -Milliseconds 250 }
} while (-not $port -and (Get-Date) -lt $deadline)

if (-not $port) { throw "Pico SDK CDC (PID 0009/000A) not found" }

Write-Host "Pico application port: $($port.DeviceID)"
Write-Host "PNP: $($port.PNPDeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ("nativev2-phase3b-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

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

$window = ""
$stage = 0
$lastAction = [DateTime]::MinValue
$dateAnswered = $false
$timeAnswered = $false
$statsSeen = $false

function Send-Dos([string]$cmd) {
    Write-Host ""
    Write-Host ">>> $cmd"
    $s.Write($cmd + "`r")
    $script:lastAction = Get-Date
}

Write-Host "log: $log"
Write-Host "auto: accept date/time -> DOS2TEST -> MDSTRESS -> Ctrl+] stats"
Write-Host ""

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
            if ($window.Length -gt 5000) {
                $window = $window.Substring($window.Length - 5000)
            }

            if (-not $dateAnswered -and $window -match 'Enter new date:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS date]"
                $s.Write("`r")
                $dateAnswered = $true
                $window = ""
                $lastAction = Get-Date
                continue
            }

            if (-not $timeAnswered -and $window -match 'Enter new time:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS time]"
                $s.Write("`r")
                $timeAnswered = $true
                $window = ""
                $lastAction = Get-Date
                continue
            }

            $prompt = [regex]::IsMatch(
                $window,
                '(?im)(^|\r|\n)[A-Z]:(?:\\[^>\r\n]*)?>\s*$|(^|\r|\n)[A-Z]>\s*$'
            )

            if ($prompt) {
                if ($stage -eq 0 -and ((Get-Date)-$lastAction).TotalMilliseconds -gt 250) {
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

        if ($statsSeen -and ((Get-Date)-$lastAction).TotalSeconds -gt 2) { break }
        Start-Sleep -Milliseconds 10
    }
}
finally {
    if ($s.IsOpen) { $s.Close() }
    $s.Dispose()
    $w.Dispose()
}

Write-Host ""
Write-Host "=== CAPTURE COMPLETE ==="
Write-Host "saved: $log"

if (-not $statsSeen) {
    Write-Warning "Did not reach Native v2 stats before timeout; send the output/log."
    exit 2
}
exit 0
