param(
    [int]$CaptureSeconds = 900,
    [switch]$Build,
    [switch]$Flash
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$target = "microdos_pico_m28h_failure_diag"
$buildDir = Join-Path $repo "build-pico\out"
$uf2 = Join-Path $buildDir "$target.uf2"
$logDir = Join-Path $repo "logs"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if ($Build) {
    Write-Host "=== M28h FAILURE DIAG BUILD ==="
    & cmake -S (Join-Path $repo "pico") -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & cmake --build $buildDir --target $target
    if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }
}

if ($Flash) {
    if (-not (Test-Path $uf2)) { throw "UF2 not found: $uf2" }
    Write-Host "=== M28h FAILURE DIAG FLASH ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}
    & $picotool load -v -x $uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
}

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
if (-not $port) { throw "Pico SDK CDC port not found" }

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logDir "m28h-v2-targeted-$stamp.txt"

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

Write-Host "Pico application port: $($port.DeviceID)"
Write-Host "repo: $repo"
Write-Host "log:  $log"

$pending = ""
$result = $null
$deadline = (Get-Date).AddSeconds($CaptureSeconds)

try {
    $s.Open()
    Start-Sleep -Milliseconds 300
    while ((Get-Date) -lt $deadline -and -not $result) {
        $chunk = ""
        try { $chunk = $s.ReadExisting() } catch {}
        if ($chunk.Length -eq 0) {
            Start-Sleep -Milliseconds 50
            continue
        }
        $pending += ($chunk -replace "`r", "")
        while ($pending.Contains("`n")) {
            $i = $pending.IndexOf("`n")
            $line = $pending.Substring(0,$i)
            $pending = $pending.Substring($i+1)
            Write-Host $line
            $w.WriteLine($line)
            if ($line -match '^\[m28h-diag\] COMPLETE result=(PASS|FAIL)') {
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
    Write-Host "=== M28h v2 TARGETED: no completion marker ==="
    Write-Host "saved: $log"
    exit 2
}
Write-Host "=== M28h v2 TARGETED: $result ==="
Write-Host "saved: $log"
if ($result -eq "PASS") { exit 0 } else { exit 1 }
