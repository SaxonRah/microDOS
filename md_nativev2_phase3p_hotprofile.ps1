param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_nativev2.uf2",
    [int]$CaptureSeconds = 300,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

if (-not $NoFlash) {
    if (-not (Test-Path $Uf2)) { throw "UF2 not found: $Uf2" }
    Write-Host "=== microDOS NATIVE V2 PHASE 3P HOT-REJECT PROFILE FLASH ==="
    try {
        & $picotool reboot -f -u
        Start-Sleep -Milliseconds 1000
    } catch {}
    & $picotool load -v -x $Uf2
    if ($LASTEXITCODE -ne 0) { throw "picotool load failed: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 1000
} else {
    Write-Host "=== microDOS NATIVE V2 PHASE 3P HOT-REJECT PROFILE RESUME ==="
}

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
$log = Join-Path $logDir ("nativev2-phase3p-hotreject-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt")

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

$commands = @("DOS2TEST", "DOS2TEST", "DOS2TEST")
$labels = @("BOOT", "DOS2TEST #1", "DOS2TEST #2", "DOS2TEST #3")
$window = ""
$dateAnswered = $false
$timeAnswered = $false
$promptPending = $false
$state = "boot"
$commandIndex = -1
$statsIndex = 0
$statsSawNative = $false
$lastRx = Get-Date
$lastAction = [DateTime]::MinValue

function Marker([string]$text) {
    $line = "`r`n=== $text ===`r`n"
    Write-Host ""
    Write-Host "=== $text ==="
    $script:w.Write($line)
}

function Update-PromptState {
    $script:promptPending = [regex]::IsMatch(
        $script:window,
        '(?im)(^|\r|\n)[A-Z]:(?:\\[^>\r\n]*)?>\s*$|(^|\r|\n)[A-Z]>\s*$'
    )
}

function Request-Stats([string]$label) {
    Marker ("HOT PROFILE INTERVAL: " + $label)
    Write-Host ">>> Ctrl+] statistics"
    $script:s.Write([string][char]0x1D)
    $script:statsSawNative = $false
    $script:state = "stats"
    $script:lastAction = Get-Date
    $script:window = ""
    $script:promptPending = $false
}

function Send-Dos([string]$cmd, [int]$ordinal) {
    Marker ("RUN: " + $cmd + " #" + $ordinal)
    Write-Host ">>> $cmd"
    $script:s.Write($cmd + "`r")
    $script:lastAction = Get-Date
    $script:window = ""
    $script:promptPending = $false
    $script:state = "command"
}

Write-Host "log: $log"
Write-Host "auto: BOOT stats -> DOS2TEST x3, Ctrl+] after each run"
Write-Host "diagnostic: 64 Native-v2 slots; reject reason includes saturated hits=1..63"
Write-Host "restore production Phase 3P after this profile"
Write-Host ""

try {
    $s.Open()
    Start-Sleep -Milliseconds 250
    $end = (Get-Date).AddSeconds($CaptureSeconds)

    while ((Get-Date) -lt $end -and $state -ne "done") {
        $chunk = ($s.ReadExisting() -replace "`0","")
        if ($chunk.Length -gt 0) {
            $lastRx = Get-Date
            Write-Host -NoNewline $chunk
            $w.Write($chunk)
            $window += $chunk
            if ($window.Length -gt 16000) {
                $window = $window.Substring($window.Length - 16000)
            }

            if (-not $dateAnswered -and $window -match 'Enter new date:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS date]"
                $s.Write("`r")
                $dateAnswered = $true
                $window = ""
                $promptPending = $false
                $lastAction = Get-Date
                continue
            }
            if (-not $timeAnswered -and $window -match 'Enter new time:\s*$') {
                Write-Host ""
                Write-Host ">>> [accept current DOS time]"
                $s.Write("`r")
                $timeAnswered = $true
                $window = ""
                $promptPending = $false
                $lastAction = Get-Date
                continue
            }

            Update-PromptState
            if ($state -eq "stats" -and
                ($chunk -match '\[native-v2\] retired=' -or
                 $window -match '\[native-v2\] retired=')) {
                $statsSawNative = $true
            }
        }

        if ($state -eq "boot" -and $promptPending -and
            ((Get-Date)-$lastAction).TotalMilliseconds -ge 250) {
            Request-Stats $labels[0]
            continue
        }

        if ($state -eq "command" -and $promptPending -and
            ((Get-Date)-$lastAction).TotalMilliseconds -ge 350) {
            ++$statsIndex
            Request-Stats $labels[$statsIndex]
            continue
        }

        if ($state -eq "stats" -and $statsSawNative -and
            ((Get-Date)-$lastRx).TotalMilliseconds -ge 900) {
            if ($commandIndex -ge ($commands.Count - 1)) {
                $state = "done"
                break
            }
            ++$commandIndex
            Send-Dos $commands[$commandIndex] ($commandIndex + 1)
            continue
        }

        Start-Sleep -Milliseconds 10
    }
}
finally {
    if ($s.IsOpen) { $s.Close() }
    $s.Dispose()
    $w.Dispose()
}

Write-Host ""
Write-Host "=== HOT-REJECT PROFILE COMPLETE ==="
Write-Host "saved: $log"

if ($state -ne "done") {
    Write-Warning "Hot-reject profile did not finish before timeout. Send the log anyway."
    exit 2
}

$raw = Get-Content -Raw -Path $log
$rx = [regex]'(?m)^\[native-v2\] reject-slot(?<slot>[0-9]+)\s+(?<cs>[0-9A-Fa-f]{4}):(?<ip>[0-9A-Fa-f]{4})\s+reason=(?<kind>compile|store)/hits=(?<hits>[0-9]+)\s+status=(?<status>\S+)\s+code=(?<code>.*)$'
$bySite = @{}
foreach ($m in $rx.Matches($raw)) {
    $key = ($m.Groups['cs'].Value.ToUpper() + ':' + $m.Groups['ip'].Value.ToUpper())
    $hits = [int]$m.Groups['hits'].Value
    if (-not $bySite.ContainsKey($key) -or $hits -gt $bySite[$key].Hits) {
        $bySite[$key] = [pscustomobject]@{
            Site   = $key
            Hits   = $hits
            Kind   = $m.Groups['kind'].Value
            Status = $m.Groups['status'].Value
            Code   = $m.Groups['code'].Value.Trim()
        }
    }
}

Write-Host ""
Write-Host "=== TOP REJECTED HOT SITES (max observed sighting count) ==="
if ($bySite.Count -eq 0) {
    Write-Warning "No encoded reject counts found. Verify the hot-profile firmware was rebuilt/flashed."
} else {
    $rank = @($bySite.Values | Sort-Object @{Expression='Hits';Descending=$true}, Site | Select-Object -First 24)
    foreach ($r in $rank) {
        $suffix = if ($r.Hits -ge 63) { "+" } else { "" }
        Write-Host ("{0,3}{1}  {2}  {3,-7} {4,-12} {5}" -f $r.Hits,$suffix,$r.Site,$r.Kind,$r.Status,$r.Code)
    }
}

Write-Host ""
Write-Host "NOTE: hits=63 is saturated and means 63 or more sightings."
Write-Host "Restore microdos_native_v2_phase3p_fix1.zip after collecting this diagnostic run."
exit 0
