param(
    [string]$Uf2 = ".\build-pico\out\microdos_nativev2_bench.uf2",
    [int]$CaptureSeconds = 15,
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

    Write-Host "=== NATIVE V2 FLASH ==="
    Write-Host "UF2: $Uf2"

    # Pico SDK USB stdio supports forced picotool reboot on compatible firmware.
    # If the currently-running image does not expose that interface, the user can
    # put the board in BOOTSEL manually and rerun this script.
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1200
    } catch {
        Write-Warning "Forced reboot failed. If load also fails, put Pico in BOOTSEL and rerun."
    }

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) {
        throw "picotool load failed with exit code $LASTEXITCODE"
    }

    Start-Sleep -Milliseconds 1500
}

Write-Host ""
Write-Host "=== USB SERIAL DISCOVERY ==="

$deadline = (Get-Date).AddSeconds(12)
$portInfo = $null

do {
    $ports = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue)

    # Raspberry Pi Pico SDK USB stdio normally enumerates under Raspberry Pi's
    # USB VID. Prefer that, then fall back to a Pico/RP2 named serial device.
    $portInfo = $ports |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -or
            $_.Name -match 'Pico|RP2|Raspberry Pi'
        } |
        Select-Object -First 1

    if (-not $portInfo) {
        Start-Sleep -Milliseconds 250
    }
} while (-not $portInfo -and (Get-Date) -lt $deadline)

if (-not $portInfo) {
    Write-Host "Available serial ports:"
    Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Format-Table DeviceID, Name, PNPDeviceID -AutoSize
    throw "Native v2 USB serial port was not found."
}

$com = $portInfo.DeviceID
Write-Host "port: $com"
Write-Host "device: $($portInfo.Name)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logDir "nativev2-phase1-$stamp.txt"

Write-Host ""
Write-Host "=== NATIVE V2 CAPTURE ==="
Write-Host "capture: $CaptureSeconds s"
Write-Host "log: $log"
Write-Host ""

$serial = [System.IO.Ports.SerialPort]::new(
    $com,
    115200,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000
$serial.DtrEnable = $true
$serial.RtsEnable = $false

$writer = [System.IO.StreamWriter]::new($log, $false, [System.Text.Encoding]::UTF8)
$writer.AutoFlush = $true

try {
    $serial.Open()
    $end = (Get-Date).AddSeconds($CaptureSeconds)

    while ((Get-Date) -lt $end) {
        $chunk = $serial.ReadExisting()
        if ($chunk.Length -gt 0) {
            Write-Host -NoNewline $chunk
            $writer.Write($chunk)

            # One complete benchmark result is enough for Phase 1.
            if ($chunk -match 'host-cycles/guest') {
                Start-Sleep -Milliseconds 300
                $tail = $serial.ReadExisting()
                if ($tail.Length -gt 0) {
                    Write-Host -NoNewline $tail
                    $writer.Write($tail)
                }
                break
            }
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
    $writer.Dispose()
}

Write-Host ""
Write-Host ""
Write-Host "=== CAPTURE COMPLETE ==="
Write-Host "saved: $log"
