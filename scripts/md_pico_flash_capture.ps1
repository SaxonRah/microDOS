param(
    [string]$Uf2 = ".\build-pico\out\microdos_pico_native3_bench.uf2",
    [string]$BuildTarget = "microdos_pico_native3_bench",
    [string]$Label = "native3",
    [int]$CaptureSeconds = 90,
    [switch]$Build,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

function Invoke-Checked {
    param(
        [Parameter(Mandatory=$true)][string]$Label,
        [Parameter(Mandatory=$true)][scriptblock]$Command
    )

    Write-Host ""
    Write-Host "============================================================"
    Write-Host $Label
    Write-Host "============================================================"

    & $Command

    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

Push-Location $Repo

try {
    if ($Build) {
        Invoke-Checked "PICO BUILD: $BuildTarget" {
            & cmake --build ".\build-pico\out" --target $BuildTarget
        }
    }

    $Uf2Path = $Uf2

    if (-not [IO.Path]::IsPathRooted($Uf2Path)) {
        $Uf2Path = Join-Path $Repo $Uf2Path
    }

    $Uf2Path = [IO.Path]::GetFullPath($Uf2Path)

    if (-not (Test-Path $Uf2Path)) {
        throw "UF2 not found: $Uf2Path"
    }

    if (-not $NoFlash) {
        if (-not (Test-Path $Picotool)) {
            throw "picotool not found: $Picotool"
        }

        Invoke-Checked "PICO FLASH: $Label" {
            try {
                & $Picotool reboot -f -u 2>$null
            }
            catch {
                # It is okay if reboot fails because the board is already
                # in BOOTSEL / USB boot mode.
            }

            Start-Sleep -Milliseconds 500

            & $Picotool load -v -x $Uf2Path
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "WAITING FOR PICO USB CDC"
    Write-Host "============================================================"

    # The Pico SDK TinyUSB CDC device normally appears as VID_2E8A and
    # PID_0009/000A. Poll aggressively so we open the port before short
    # benchmark firmware prints its startup results.
    $deadline = (Get-Date).AddSeconds(20)
    $port = $null

    do {
        $port = @(
            Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
        ) |
        Where-Object {
            $_.PNPDeviceID -match 'VID_2E8A' -and
            $_.PNPDeviceID -match 'PID_0009|PID_000A'
        } |
        Select-Object -First 1

        if (-not $port) {
            Start-Sleep -Milliseconds 50
        }
    }
    while (-not $port -and (Get-Date) -lt $deadline)

    if (-not $port) {
        Write-Host ""
        Write-Host "Available serial ports:"

        Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
            Format-Table DeviceID, Name, PNPDeviceID -AutoSize

        throw "Pico SDK CDC port not found"
    }

    Write-Host "Pico application port: $($port.DeviceID)"

    $LogDir = Join-Path $Repo "logs"
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

    $SafeLabel = $Label -replace '[^A-Za-z0-9_.-]', '_'
    $Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $Log = Join-Path $LogDir "pico-$SafeLabel-$Stamp.txt"

    $s = [System.IO.Ports.SerialPort]::new(
        $port.DeviceID,
        115200,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )

    # TinyUSB stdio waits for DTR on many of our Pico firmware builds.
    $s.DtrEnable = $true
    $s.RtsEnable = $false
    $s.ReadTimeout = 50
    $s.WriteTimeout = 1000

    $writer = [System.IO.StreamWriter]::new(
        $Log,
        $false,
        [System.Text.UTF8Encoding]::new($false)
    )
    $writer.AutoFlush = $true

    Write-Host "log: $Log"
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "LIVE PICO OUTPUT"
    Write-Host "============================================================"

    $pending = ""
    $done = $false
    $gateCount = 0
    $deadline = (Get-Date).AddSeconds($CaptureSeconds)

    try {
        $s.Open()

        while ((Get-Date) -lt $deadline -and -not $done) {
            $chunk = ""

            try {
                $chunk = $s.ReadExisting()
            }
            catch {
                $chunk = ""
            }

            if ($chunk.Length -eq 0) {
                Start-Sleep -Milliseconds 20
                continue
            }

            $pending += ($chunk -replace "`r", "")

            while ($pending.Contains("`n")) {
                $i = $pending.IndexOf("`n")
                $line = $pending.Substring(0, $i)
                $pending = $pending.Substring($i + 1)

                Write-Host $line
                $writer.WriteLine($line)

                # Current scaling diagnostic.
                if ($line -match '\[sweep\] ALL CASES COMPLETE') {
                    $done = $true
                    break
                }

                # Normal Native-3 benchmark: loop/regmix/callmix.
                if ($line -match 'GATE40=(PASS|FAIL)') {
                    $gateCount++

                    if ($gateCount -ge 3) {
                        $done = $true
                        break
                    }
                }

                # Other common completion markers used by microDOS Pico tests.
                if ($line -match 'COMPLETE result=(PASS|FAIL)') {
                    $done = $true
                    break
                }

                if ($line -match '\[e2e\] PASS') {
                    $done = $true
                    break
                }
            }
        }
    }
    finally {
        if ($pending.Length -gt 0) {
            Write-Host $pending
            $writer.WriteLine($pending)
        }

        if ($s.IsOpen) {
            $s.Close()
        }

        $writer.Close()
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "CAPTURE RESULT"
    Write-Host "============================================================"

    if ($done) {
        Write-Host "Completion marker received."
        Write-Host "GATE40 lines: $gateCount"
        Write-Host "saved: $Log"
        exit 0
    }

    Write-Host "No completion marker within $CaptureSeconds seconds."
    Write-Host "GATE40 lines: $gateCount"
    Write-Host "saved: $Log"
    exit 2
}
finally {
    Pop-Location
}
