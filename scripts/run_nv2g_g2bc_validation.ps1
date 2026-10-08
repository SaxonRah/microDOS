param(
    [string]$Repo = "C:\microDOS",
    [switch]$SkipHost,
    [switch]$SkipFullDiff,
    [switch]$SkipFocusedDiff,
    [switch]$SkipCensus,
    [switch]$SkipSplitbench
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Section([string]$Title) {
    Write-Host ""
    Write-Host ("=" * 72)
    Write-Host $Title
    Write-Host ("=" * 72)
}

$Repo = [IO.Path]::GetFullPath($Repo)
if (-not (Test-Path -LiteralPath $Repo)) {
    throw "Repo not found: $Repo"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logDir = Join-Path $Repo "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "nv2g-g2bc-validation-$stamp.txt"

Start-Transcript -Path $log -Force | Out-Null

try {
    Set-Location $Repo

    Section "NV2-G G-2B2 + G-2C VALIDATION"
    Write-Host "repo: $Repo"
    Write-Host "log:  $log"

    if (-not $SkipHost) {
        Section "1/5 HOST BUILD"
        & ".\md.bat" build host
        if ($LASTEXITCODE -ne 0) {
            throw "host build failed with exit code $LASTEXITCODE"
        }

        Section "1/5 HOST CTEST"
        & ctest `
            --test-dir ".\build-host" `
            -C Release `
            --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "ctest failed with exit code $LASTEXITCODE"
        }
    } else {
        Section "1/5 HOST BUILD + CTEST - SKIPPED"
    }

    if (-not $SkipFullDiff) {
        Section "2/5 EXISTING NV2-G FULL QEMU DIFFERENTIAL"
        & ".\scripts\md_nativev2g_diff.ps1"
    } else {
        Section "2/5 FULL QEMU DIFFERENTIAL - SKIPPED"
    }

    if (-not $SkipFocusedDiff) {
        Section "3/5 G-2B2/G-2C FOCUSED QEMU DIFFERENTIAL"
        & ".\scripts\md_nativev2g_bc_diff.ps1"
    } else {
        Section "3/5 FOCUSED QEMU DIFFERENTIAL - SKIPPED"
    }

    if (-not $SkipCensus) {
        Section "4/5 REAL-TOOLS CENSUS"
        & ".\scripts\md_nv2g_census.ps1"
    } else {
        Section "4/5 REAL-TOOLS CENSUS - SKIPPED"
    }

    if (-not $SkipSplitbench) {
        Section "5/5 PICO SPLITBENCH"
        & ".\scripts\md_nativev2g_splitbench.ps1"
    } else {
        Section "5/5 PICO SPLITBENCH - SKIPPED"
    }

    Section "G-2B2 + G-2C VALIDATION COMPLETE"
    Write-Host "log: $log"
}
catch {
    Section "G-2B2 + G-2C VALIDATION FAILED"
    Write-Host $_
    Write-Host "log: $log"
    throw
}
finally {
    try { Stop-Transcript | Out-Null } catch {}
}
