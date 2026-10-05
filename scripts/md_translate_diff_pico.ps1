param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_translate_diff.uf2",

    [string]$Label = "m25",

    [int]$CaptureSeconds = 1200,

    [switch]$Build,

    [switch]$NoFlash
)

# M25 on-device translator check.
#
#   .\scripts\md_translate_diff_pico.ps1 -Build
#
# -Build     builds microdos_pico_translate_diff in .\build-pico\out first
# -NoFlash   reuse the firmware already on the board (sends R to rerun)
#
# Console output is mirrored to logs\translate-diff-<label>-<stamp>.txt.
# Exit code 0 only if the firmware reports "COMPLETE result=PASS".

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if ($Build) {
    Write-Host "=== microDOS M25 TRANSLATE-DIFF BUILD ==="
    & cmake --build .\build-pico\out --target microdos_pico_translate_diff
    if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }
}

if (-not $NoFlash) {
    if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2 (use -Build)" }
    if (-not (Test-Path $picotool)) { throw "picotool not found: $picotool" }

    Write-Host "=== microDOS M25 TRANSLATE-DIFF FLASH: $Label ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}

    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
}

# Same strict Pico SDK CDC selection as md_engine_splitbench.ps1.
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
    Write-Host "Available serial ports:"
    Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Format-Table DeviceID, Name, PNPDeviceID -AutoSize
    throw "Pico SDK CDC (VID_2E8A, PID 0009/000A) not found"
}
Write-Host "Pico application port: $($port.DeviceID)"

$logDir = Join-Path (Get-Location) "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$safeLabel = ($Label -replace '[^A-Za-z0-9_.-]', '_')
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logDir ("translate-diff-$safeLabel-$stamp.txt")

$s = [System.IO.Ports.SerialPort]::new($port.DeviceID, 115200,
    [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$s.DtrEnable = $true          # firmware starts when DTR is asserted
$s.RtsEnable = $false
$s.ReadTimeout = 50
$s.WriteTimeout = 1000

$w = [System.IO.StreamWriter]::new($log, $false, [System.Text.UTF8Encoding]::new($false))
$w.AutoFlush = $true

Write-Host "log: $log"
Write-Host "Running on-device differential test + benchmark (takes several minutes)..."

$result = $null
$pending = ""
$deadline = (Get-Date).AddSeconds($CaptureSeconds)

try {
    $s.Open()
    if ($NoFlash) { Start-Sleep -Milliseconds 300; $s.Write("R") }

    while ((Get-Date) -lt $deadline -and -not $result) {
        $chunk = ""
        try { $chunk = $s.ReadExisting() } catch {}
        if ($chunk.Length -eq 0) { Start-Sleep -Milliseconds 50; continue }

        $pending += ($chunk -replace "`r", "")
        while ($pending.Contains("`n")) {
            $i = $pending.IndexOf("`n")
            $line = $pending.Substring(0, $i)
            $pending = $pending.Substring($i + 1)
            Write-Host $line
            $w.WriteLine($line)
            if ($line -match '\[translate-diff\] COMPLETE result=(PASS|FAIL)') {
                $result = $Matches[1]
            }
        }
    }
} finally {
    if ($pending.Length -gt 0) { Write-Host $pending; $w.WriteLine($pending) }
    if ($s.IsOpen) { $s.Close() }
    $w.Close()
}

Write-Host ""
if (-not $result) {
    Write-Host "=== M25 TRANSLATE-DIFF: no completion marker within $CaptureSeconds s ==="
    Write-Host "saved: $log"
    exit 2
}
Write-Host "=== M25 TRANSLATE-DIFF: $result ==="
Write-Host "saved: $log"
if ($result -eq "PASS") { exit 0 } else { exit 1 }
