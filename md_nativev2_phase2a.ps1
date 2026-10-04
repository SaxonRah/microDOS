param(
    [string]$Uf2 = ".\build-pico\out\microdos_nativev2_bench.uf2",
    [int]$CaptureSeconds = 30
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }

Write-Host "=== NATIVE V2 PHASE 2A FLASH ==="
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

if (-not $port) {
    Write-Host "Raspberry Pi serial devices:"
    Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -match 'VID_2E8A' } |
        Format-Table DeviceID,Name,PNPDeviceID -AutoSize
    throw "Pico SDK CDC (PID 0009/000A) not found"
}

Write-Host "Pico application port: $($port.DeviceID)"
Write-Host "PNP: $($port.PNPDeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ("nativev2-phase2a-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

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

$state = "hello"
$done = $false
$lastNudge = [DateTime]::MinValue

Write-Host "log: $log"
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

            if ($chunk -match 'NV2-P2A' -and $state -eq "hello") {
                $s.Write("C")
                $state = "compile"
            }
            if ($chunk -match 'READY - send R' -and $state -ne "run") {
                $s.Write("R")
                $state = "run"
            }
            if ($chunk -match 'PHASE2A PASS') {
                $done = $true
                Start-Sleep -Milliseconds 250
                $tail = ($s.ReadExisting() -replace "`0","")
                if ($tail.Length -gt 0) {
                    Write-Host -NoNewline $tail
                    $w.Write($tail)
                }
                break
            }
        }

        if (((Get-Date)-$lastNudge).TotalMilliseconds -ge 1500) {
            try {
                if ($state -eq "hello") { $s.Write("C") }
                elseif ($state -eq "compile") {
                    $s.Write("R")
                    $state = "run"
                }
            } catch {}
            $lastNudge = Get-Date
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
Write-Host "saved: $log"

if ($done) {
    Write-Host "Native v2 Phase 2A: PASS"
    exit 0
}

Write-Warning "Phase 2A did not reach PASS. Send the captured log/output."
exit 2
