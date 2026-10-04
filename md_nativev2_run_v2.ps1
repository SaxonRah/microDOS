param(
    [string]$Uf2 = ".\build-pico\out\microdos_nativev2_bench.uf2",
    [int]$CaptureSeconds = 30,
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

    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1200
    } catch {
        Write-Warning "Forced reboot failed. If load fails, put Pico in BOOTSEL and rerun."
    }

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) {
        throw "picotool load failed with exit code $LASTEXITCODE"
    }

    Start-Sleep -Milliseconds 1000
}

Write-Host ""
Write-Host "=== USB SERIAL DISCOVERY ==="

$deadline = (Get-Date).AddSeconds(15)
$portInfo = $null

do {
    $ports = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue)

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
Write-Host "The runner will periodically trigger another benchmark pass if the first output was missed."
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

# No BOM: makes empty/failed captures visibly empty rather than looking like data.
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$writer = [System.IO.StreamWriter]::new($log, $false, $utf8NoBom)
$writer.AutoFlush = $true

$seenAnyText = $false
$seenResult = $false
$lastTrigger = [DateTime]::MinValue

try {
    $serial.Open()

    # Give TinyUSB/Windows a moment after opening the CDC port.
    Start-Sleep -Milliseconds 250

    $end = (Get-Date).AddSeconds($CaptureSeconds)

    while ((Get-Date) -lt $end) {
        $chunk = $serial.ReadExisting()

        if ($chunk.Length -gt 0) {
            # Ignore pure NUL traffic; it is not benchmark output.
            $printable = $chunk -replace "`0", ""

            if ($printable.Length -gt 0) {
                $seenAnyText = $true
                Write-Host -NoNewline $printable
                $writer.Write($printable)

                if ($printable -match 'host-cycles/guest') {
                    $seenResult = $true
                    Start-Sleep -Milliseconds 250

                    $tail = ($serial.ReadExisting() -replace "`0", "")
                    if ($tail.Length -gt 0) {
                        Write-Host -NoNewline $tail
                        $writer.Write($tail)
                    }
                    break
                }
            }
        }

        # Firmware performs one benchmark, drains stale input, then blocks in
        # getchar(). Sending once per second guarantees that a trigger arrives
        # after the drain even if earlier triggers occurred while the benchmark
        # was still running.
        if (((Get-Date) - $lastTrigger).TotalMilliseconds -ge 1000) {
            try {
                $serial.Write("`r")
            } catch {
                # Keep capturing; transient USB CDC write failures can happen
                # while the device re-enumerates.
            }
            $lastTrigger = Get-Date
        }

        Start-Sleep -Milliseconds 20
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

if ($seenResult) {
    Write-Host "result: PASS - host-cycles/guest captured"
    exit 0
}

if ($seenAnyText) {
    Write-Warning "Text was captured, but no host-cycles/guest result appeared."
    Write-Host "Please send the log above."
    exit 2
}

Write-Warning "No textual USB serial output was captured."
Write-Host "If the Pico is still enumerated, retry without reflashing:"
Write-Host "  .\md_nativev2_run_v2.ps1 -NoFlash -CaptureSeconds 30"
exit 3
