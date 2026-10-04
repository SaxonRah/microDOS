param(
    [string]$Uf2 = ".\build-pico\out\microdos_nativev2_bench.uf2",
    [int]$CaptureSeconds = 30,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }

if (-not $NoFlash) {
    Write-Host "=== NATIVE V2 DIAGNOSTIC FLASH ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
}

$deadline = (Get-Date).AddSeconds(15)
$portInfo = $null
do {
    $portInfo = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue) |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -or
            $_.Name -match 'Pico|RP2|Raspberry Pi'
        } |
        Select-Object -First 1
    if (-not $portInfo) { Start-Sleep -Milliseconds 250 }
} while (-not $portInfo -and (Get-Date) -lt $deadline)

if (-not $portInfo) { throw "Pico USB serial port not found" }

$com = $portInfo.DeviceID
Write-Host "port: $com"
Write-Host "PNP:  $($portInfo.PNPDeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ("nativev2-diag-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

$serial = [System.IO.Ports.SerialPort]::new(
    $com, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.DtrEnable = $true
$serial.RtsEnable = $false
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000

$enc = [System.Text.UTF8Encoding]::new($false)
$writer = [System.IO.StreamWriter]::new($log, $false, $enc)
$writer.AutoFlush = $true

$state = "hello"
$lastNudge = [DateTime]::MinValue
$result = $false

Write-Host "log: $log"
Write-Host ""

try {
    $serial.Open()
    Start-Sleep -Milliseconds 200
    $end = (Get-Date).AddSeconds($CaptureSeconds)

    while ((Get-Date) -lt $end) {
        $chunk = ($serial.ReadExisting() -replace "`0", "")

        if ($chunk.Length -gt 0) {
            Write-Host -NoNewline $chunk
            $writer.Write($chunk)

            if ($chunk -match '\[NV2-HELLO\]' -and $state -eq "hello") {
                # Any key tells firmware the host is definitely listening and
                # causes compile + code dump.
                $serial.Write("C")
                $state = "compile"
            }

            if ($chunk -match 'READY - send R' -and $state -ne "execute") {
                $serial.Write("R")
                $state = "execute"
            }

            if ($chunk -match 'host-cycles/guest') {
                $result = $true
                Start-Sleep -Milliseconds 200
                $tail = ($serial.ReadExisting() -replace "`0", "")
                if ($tail.Length -gt 0) {
                    Write-Host -NoNewline $tail
                    $writer.Write($tail)
                }
                break
            }
        }

        # If a line boundary was split across ReadExisting chunks, periodic
        # nudges still advance the firmware deterministically.
        if (((Get-Date) - $lastNudge).TotalMilliseconds -ge 1500) {
            if ($state -eq "hello") {
                try { $serial.Write("C") } catch {}
            } elseif ($state -eq "compile") {
                try { $serial.Write("R") } catch {}
                $state = "execute"
            }
            $lastNudge = Get-Date
        }

        Start-Sleep -Milliseconds 20
    }
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    $writer.Dispose()
}

Write-Host ""
Write-Host ""
Write-Host "=== DIAGNOSTIC COMPLETE ==="
Write-Host "saved: $log"

if ($result) {
    Write-Host "result: PASS - performance result captured"
    exit 0
}

Write-Warning "No final performance line. The last printed stage tells us where execution stopped."
exit 2
