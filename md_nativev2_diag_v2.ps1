param(
    [string]$Uf2 = ".\build-pico\out\microdos_nativev2_bench.uf2",
    [int]$CaptureSeconds = 30,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

function Show-RaspberryPiPorts {
    Write-Host ""
    Write-Host "Raspberry Pi serial devices currently visible:"
    $all = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -match 'VID_2E8A' })

    if ($all.Count -eq 0) {
        Write-Host "  (none)"
        return
    }

    foreach ($p in $all) {
        $kind = "other"
        if ($p.PNPDeviceID -match 'PID_0009|PID_000A') {
            $kind = "Pico SDK CDC"
        } elseif ($p.PNPDeviceID -match 'PID_000C') {
            $kind = "Debug Probe - IGNORE"
        } elseif ($p.PNPDeviceID -match 'PID_000F') {
            $kind = "RP2350 BOOTROM"
        }

        Write-Host ("  {0,-6} {1,-22} {2}" -f $p.DeviceID, $kind, $p.PNPDeviceID)
    }
}

if (-not (Test-Path $Uf2)) {
    throw "UF2 not found: $Uf2"
}

Show-RaspberryPiPorts

if (-not $NoFlash) {
    if (-not (Test-Path $picotool)) {
        throw "picotool not found: $picotool"
    }

    Write-Host ""
    Write-Host "=== NATIVE V2 DIAGNOSTIC FLASH ==="
    Write-Host "UF2: $Uf2"

    # -f may use an SDK CDC reset interface if one exists. Failure is harmless
    # if the target is already in BOOTSEL.
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) {
        throw "picotool load failed: $LASTEXITCODE"
    }

    Start-Sleep -Milliseconds 1000
}

Write-Host ""
Write-Host "=== PICO SDK CDC DISCOVERY ==="

$deadline = (Get-Date).AddSeconds(20)
$portInfo = $null

do {
    $ports = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue)

    # Raspberry Pi USB PID allocation:
    #   0009 / 000A = Pico SDK CDC UART
    #   000C        = Raspberry Pi Debug Probe (NOT our firmware)
    $portInfo = $ports |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -and
            $_.PNPDeviceID -match 'PID_0009|PID_000A'
        } |
        Select-Object -First 1

    if (-not $portInfo) {
        Start-Sleep -Milliseconds 250
    }
} while (-not $portInfo -and (Get-Date) -lt $deadline)

if (-not $portInfo) {
    Show-RaspberryPiPorts
    throw @"
Native v2 Pico SDK CDC port was not found.

IMPORTANT:
  PID_000C is the Raspberry Pi Debug Probe and is intentionally ignored.
  The application CDC interface must enumerate as PID_0009 or PID_000A.

If only PID_000C is present, the benchmark application itself has not exposed
USB stdio and we should debug firmware startup next.
"@
}

$com = $portInfo.DeviceID

Write-Host "Pico application port: $com"
Write-Host "PNP: $($portInfo.PNPDeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ("nativev2-diag-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

$serial = [System.IO.Ports.SerialPort]::new(
    $com,
    115200,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
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

Write-Host ""
Write-Host "=== NATIVE V2 CAPTURE ==="
Write-Host "log: $log"
Write-Host ""

try {
    $serial.Open()
    Start-Sleep -Milliseconds 250

    $end = (Get-Date).AddSeconds($CaptureSeconds)

    while ((Get-Date) -lt $end) {
        $chunk = ($serial.ReadExisting() -replace "`0", "")

        if ($chunk.Length -gt 0) {
            Write-Host -NoNewline $chunk
            $writer.Write($chunk)

            if ($chunk -match '\[NV2-HELLO\]' -and $state -eq "hello") {
                $serial.Write("C")
                $state = "compile"
            }

            if ($chunk -match 'READY - send R' -and $state -ne "execute") {
                $serial.Write("R")
                $state = "execute"
            }

            if ($chunk -match 'host-cycles/guest') {
                $result = $true
                Start-Sleep -Milliseconds 250

                $tail = ($serial.ReadExisting() -replace "`0", "")
                if ($tail.Length -gt 0) {
                    Write-Host -NoNewline $tail
                    $writer.Write($tail)
                }
                break
            }
        }

        # Handle split lines / late opens.
        if (((Get-Date) - $lastNudge).TotalMilliseconds -ge 1500) {
            try {
                if ($state -eq "hello") {
                    $serial.Write("C")
                } elseif ($state -eq "compile") {
                    $serial.Write("R")
                    $state = "execute"
                }
            } catch {}
            $lastNudge = Get-Date
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
Write-Host "=== DIAGNOSTIC COMPLETE ==="
Write-Host "saved: $log"

if ($result) {
    Write-Host "result: PASS - performance result captured"
    exit 0
}

Write-Warning "No final performance line. Send the log; the last emitted stage is diagnostic."
exit 2
