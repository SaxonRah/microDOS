param(
    [int]$CaptureSeconds = 180,
    [switch]$NoBuild,
    [switch]$NoFlash,
    [switch]$SkipStress
)

$ErrorActionPreference = "Stop"

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $Repo "build-pico\out"
$Target = "microdos_pico_nativev2_profile"
$Uf2 = Join-Path $BuildDir "$Target.uf2"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

Push-Location $Repo

try {
    if (-not $NoBuild) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "BUILD NATIVE-V2 DOS PROFILE"
        Write-Host "============================================================"

        & cmake --build $BuildDir --target $Target
        if ($LASTEXITCODE -ne 0) {
            throw "Pico build failed: $LASTEXITCODE"
        }
    }

    if (-not (Test-Path $Uf2)) {
        throw "UF2 not found: $Uf2"
    }

    if (-not $NoFlash) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "FLASH NATIVE-V2 DOS PROFILE"
        Write-Host "============================================================"

        try {
            & $Picotool reboot -f -u 2>$null
        }
        catch {
            # It is okay if reboot fails because the board is already in
            # BOOTSEL / USB boot mode.
        }

        Start-Sleep -Milliseconds 500

        & $Picotool load -v -x $Uf2
        if ($LASTEXITCODE -ne 0) {
            throw "picotool load failed: $LASTEXITCODE"
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "WAITING FOR PICO USB CDC"
    Write-Host "============================================================"

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
        throw "Pico SDK CDC port not found"
    }

    Write-Host "Pico application port: $($port.DeviceID)"

    $LogDir = Join-Path $Repo "logs"
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

    $Log = Join-Path $LogDir (
        "pico-nativev2-dos-profile-" +
        (Get-Date -Format "yyyyMMdd-HHmmss") +
        ".txt"
    )

    $s = [System.IO.Ports.SerialPort]::new(
        $port.DeviceID,
        115200,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )

    $s.DtrEnable = $true
    $s.RtsEnable = $false
    $s.ReadTimeout = 50
    $s.WriteTimeout = 1000

    $w = [System.IO.StreamWriter]::new(
        $Log,
        $false,
        [System.Text.UTF8Encoding]::new($false)
    )
    $w.AutoFlush = $true

    $window = ""
    $stage = 0
    $lastAction = [DateTime]::MinValue
    $dateAnswered = $false
    $timeAnswered = $false
    $dos2Pass = $false
    $stressDone = $false
    $statsSeen = $false
    $promptPending = $false

    function Send-Dos([string]$cmd) {
        Write-Host ""
        Write-Host ">>> $cmd"

        $s.Write($cmd + "`r")

        $script:lastAction = Get-Date
        $script:promptPending = $false
        $script:window = ""
    }

    function Update-PromptState {
        $script:promptPending = [regex]::IsMatch(
            $script:window,
            '(?im)(^|\r|\n)[A-Z]:(?:\\[^>\r\n]*)?>\s*$|(^|\r|\n)[A-Z]>\s*$'
        )
    }

    Write-Host "log: $Log"
    Write-Host "auto: boot -> accept date/time -> DOS2TEST"

    if (-not $SkipStress) {
        Write-Host "      -> MDSTRESS"
    }

    Write-Host "      -> Ctrl+] Native-v2 statistics"
    Write-Host ""

    try {
        $s.Open()
        $end = (Get-Date).AddSeconds($CaptureSeconds)

        while ((Get-Date) -lt $end) {
            $chunk = ($s.ReadExisting() -replace "`0", "")

            if ($chunk.Length -gt 0) {
                Write-Host -NoNewline $chunk
                $w.Write($chunk)

                $window += $chunk

                if ($window.Length -gt 12000) {
                    $window = $window.Substring($window.Length - 12000)
                }

                if ($window -match 'ALL TESTS PASSED') {
                    $dos2Pass = $true
                }

                if ($window -match '\[native-v2\]') {
                    $statsSeen = $true
                }

                if (-not $dateAnswered -and
                    $window -match 'Enter new date:\s*$') {

                    Write-Host ""
                    Write-Host ">>> [accept current DOS date]"

                    $s.Write("`r")

                    $dateAnswered = $true
                    $window = ""
                    $promptPending = $false
                    $lastAction = Get-Date
                    continue
                }

                if (-not $timeAnswered -and
                    $window -match 'Enter new time:\s*$') {

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
            }

            # Act on DOS prompts outside the "new bytes arrived" block.
            # This avoids missing a prompt that arrived in the previous read.
            if ($promptPending) {
                $ageMs = ((Get-Date) - $lastAction).TotalMilliseconds

                if ($stage -eq 0 -and $ageMs -ge 250) {
                    Send-Dos "DOS2TEST"
                    $stage = 1
                }
                elseif ($stage -eq 1 -and $ageMs -ge 500) {
                    if (-not $dos2Pass) {
                        Write-Warning (
                            "DOS2TEST returned to DOS without " +
                            "ALL TESTS PASSED"
                        )
                    }

                    if ($SkipStress) {
                        Write-Host ""
                        Write-Host ">>> Ctrl+] Native-v2 statistics"

                        $s.Write([string][char]0x1D)

                        $stage = 3
                        $promptPending = $false
                        $window = ""
                        $lastAction = Get-Date
                    }
                    else {
                        Send-Dos "MDSTRESS"
                        $stage = 2
                    }
                }
                elseif ($stage -eq 2 -and $ageMs -ge 500) {
                    $stressDone = $true

                    Write-Host ""
                    Write-Host ">>> Ctrl+] Native-v2 statistics"

                    $s.Write([string][char]0x1D)

                    $stage = 3
                    $promptPending = $false
                    $window = ""
                    $lastAction = Get-Date
                }
            }

            if ($stage -eq 3 -and
                $statsSeen -and
                ((Get-Date) - $lastAction).TotalSeconds -gt 2) {

                break
            }

            Start-Sleep -Milliseconds 10
        }
    }
    finally {
        if ($s.IsOpen) {
            $s.Close()
        }

        $s.Dispose()
        $w.Dispose()
    }

    Write-Host ""
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "NATIVE-V2 DOS PROFILE RESULT"
    Write-Host "============================================================"

    # Windows PowerShell 5.1 compatible status formatting.
    if ($dos2Pass) {
        $dos2Status = "PASS"
    }
    else {
        $dos2Status = "NOT CONFIRMED"
    }

    if ($stressDone) {
        $stressStatus = "returned to DOS prompt"
    }
    else {
        $stressStatus = "NOT CONFIRMED"
    }

    if ($statsSeen) {
        $statsStatus = "CAPTURED"
    }
    else {
        $statsStatus = "NOT CAPTURED"
    }

    Write-Host "DOS2TEST: $dos2Status"

    if (-not $SkipStress) {
        Write-Host "MDSTRESS: $stressStatus"
    }

    Write-Host "NV2 stats: $statsStatus"
    Write-Host "saved: $Log"

    if (-not $dos2Pass -or -not $statsSeen) {
        exit 2
    }

    exit 0
}
finally {
    Pop-Location
}
