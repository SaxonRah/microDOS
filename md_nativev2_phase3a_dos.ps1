param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_nativev2.uf2",
    [int]$CaptureSeconds = 150,
    [switch]$NoAutoTests
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }

Write-Host "=== microDOS NATIVE V2 LIVE-DOS FLASH ==="
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
        } |
        Select-Object -First 1
    if (-not $port) { Start-Sleep -Milliseconds 250 }
} while (-not $port -and (Get-Date) -lt $deadline)

if (-not $port) { throw "Pico SDK CDC (PID 0009/000A) not found" }

Write-Host "Pico application port: $($port.DeviceID)"
Write-Host "PNP: $($port.PNPDeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ("nativev2-phase3a-dos-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

$s = [System.IO.Ports.SerialPort]::new(
    $port.DeviceID,115200,[System.IO.Ports.Parity]::None,8,
    [System.IO.Ports.StopBits]::One)
$s.DtrEnable = $true
$s.RtsEnable = $false
$s.ReadTimeout = 100
$s.WriteTimeout = 1000

$enc = [System.Text.UTF8Encoding]::new($false)
$w = [System.IO.StreamWriter]::new($log,$false,$enc)
$w.AutoFlush = $true

$stage = if ($NoAutoTests) { 99 } else { 0 }
$window = ""
$lastAction = Get-Date
$statsSeen = $false

Write-Host "log: $log"
if (-not $NoAutoTests) {
    Write-Host "auto: DOS2TEST -> MDSTRESS -> Ctrl+] stats"
}
Write-Host ""

function Send-Dos([string]$cmd) {
    Write-Host ""
    Write-Host ">>> $cmd"
    $s.Write($cmd + "`r")
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
            if ($window.Length -gt 2500) {
                $window = $window.Substring($window.Length - 2500)
            }

            $prompt = [regex]::IsMatch($window, '(?im)(^|\r|\n)[A-Z]:?\\?>\s*$')

            if (-not $NoAutoTests -and $prompt) {
                if ($stage -eq 0 -and ((Get-Date)-$lastAction).TotalMilliseconds -gt 500) {
                    Send-Dos "DOS2TEST"
                    $stage = 1
                    $window = ""
                    $lastAction = Get-Date
                }
                elseif ($stage -eq 1 -and ((Get-Date)-$lastAction).TotalMilliseconds -gt 1000) {
                    Send-Dos "MDSTRESS"
                    $stage = 2
                    $window = ""
                    $lastAction = Get-Date
                }
                elseif ($stage -eq 2 -and ((Get-Date)-$lastAction).TotalMilliseconds -gt 1000) {
                    Write-Host ""
                    Write-Host ">>> Ctrl+] statistics"
                    $s.Write([string][char]0x1D)
                    $stage = 3
                    $window = ""
                    $lastAction = Get-Date
                }
            }

            if ($stage -eq 3 -and
                ($chunk -match '\[native-v2\] retired=' -or
                 $window -match '\[native-v2\] retired=')) {
                $statsSeen = $true
            }

            if ($statsSeen -and ((Get-Date)-$lastAction).TotalSeconds -gt 2) {
                break
            }
        }

        Start-Sleep -Milliseconds 20
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

if (-not $NoAutoTests -and -not $statsSeen) {
    Write-Warning "Automatic sequence did not reach Native v2 statistics before timeout."
    Write-Host "Send the log/output anyway; boot/test progress is still diagnostic."
    exit 2
}

exit 0
