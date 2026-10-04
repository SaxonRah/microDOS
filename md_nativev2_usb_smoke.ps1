param(
    [string]$Uf2 = ".\build-pico\out\microdos_nativev2_usbsmoke.uf2",
    [int]$CaptureSeconds = 10
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }

Write-Host "=== NV2 USB SMOKE FLASH ==="
try {
    & $picotool reboot -f -u
    Start-Sleep -Milliseconds 1000
} catch {}

& $picotool load -v -x $Uf2
if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }

Start-Sleep -Milliseconds 1000

$deadline = (Get-Date).AddSeconds(15)
$port = $null
do {
    $port = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue) |
        Where-Object { $_.PNPDeviceID -match 'VID_2E8A' } |
        Select-Object -First 1
    if (-not $port) { Start-Sleep -Milliseconds 250 }
} while (-not $port -and (Get-Date) -lt $deadline)

if (-not $port) { throw "Pico USB serial port not found" }

Write-Host "port: $($port.DeviceID)"
Write-Host "PNP:  $($port.PNPDeviceID)"
Write-Host ""

$s = [System.IO.Ports.SerialPort]::new(
    $port.DeviceID, 115200, [System.IO.Ports.Parity]::None, 8,
    [System.IO.Ports.StopBits]::One)
$s.DtrEnable = $true
$s.RtsEnable = $false
$s.ReadTimeout = 100

$seen = $false
try {
    $s.Open()
    Start-Sleep -Milliseconds 300
    $end = (Get-Date).AddSeconds($CaptureSeconds)
    while ((Get-Date) -lt $end) {
        $chunk = ($s.ReadExisting() -replace "`0", "")
        if ($chunk.Length -gt 0) {
            Write-Host -NoNewline $chunk
            if ($chunk -match 'NV2-USB-SMOKE') { $seen = $true }
        }
        if ($seen) {
            Start-Sleep -Milliseconds 700
            $tail = ($s.ReadExisting() -replace "`0", "")
            if ($tail.Length -gt 0) { Write-Host -NoNewline $tail }
            break
        }
        Start-Sleep -Milliseconds 20
    }
}
finally {
    if ($s.IsOpen) { $s.Close() }
    $s.Dispose()
}

Write-Host ""
if ($seen) {
    Write-Host "NV2 USB smoke: PASS"
    exit 0
}

Write-Warning "NV2 USB smoke: FAIL - CDC enumerated but no application text arrived."
exit 2
