param(
    [string]$Repo = "C:\microDOS",
    [string]$BundleZip = "$HOME\Downloads\nv2g_g2b1_bundle.zip",
    [switch]$SkipInstall,
    [switch]$SkipHost,
    [switch]$SkipDiff,
    [switch]$SkipCensus,
    [switch]$SkipSplitbench
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Section {
    param([string]$Title)

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

New-Item `
    -ItemType Directory `
    -Force `
    -Path $logDir |
    Out-Null

$log = Join-Path `
    $logDir `
    "nv2g-g2b1-validation-$stamp.txt"

Start-Transcript `
    -Path $log `
    -Force |
    Out-Null

try {

    Section "NV2-G G-2B1 VALIDATION"

    Write-Host "repo:       $Repo"
    Write-Host "bundle zip: $BundleZip"
    Write-Host "log:        $log"

    # ------------------------------------------------------------------
    # 1/5 INSTALL
    # ------------------------------------------------------------------

    if (-not $SkipInstall) {

        Section "1/5 INSTALL G-2B1"

        if (-not (Test-Path -LiteralPath $BundleZip)) {
            throw "Bundle zip not found: $BundleZip"
        }

        $extractRoot = Join-Path `
            $HOME `
            "Downloads\nv2g_g2b1_bundle"

        Remove-Item `
            -LiteralPath $extractRoot `
            -Recurse `
            -Force `
            -ErrorAction SilentlyContinue

        Expand-Archive `
            -LiteralPath $BundleZip `
            -DestinationPath $extractRoot `
            -Force

        $bundleDir = Join-Path `
            $extractRoot `
            "nv2g_g2b1"

        $installer = Join-Path `
            $bundleDir `
            "install_nv2g_g2b1.ps1"

        if (-not (Test-Path -LiteralPath $installer)) {
            throw "Installer not found: $installer"
        }

        Unblock-File `
            -LiteralPath $installer `
            -ErrorAction SilentlyContinue

        & $installer -Repo $Repo
    }
    else {
        Section "1/5 INSTALL G-2B1 - SKIPPED"
    }

    Set-Location $Repo

    # ------------------------------------------------------------------
    # 2/5 HOST
    # ------------------------------------------------------------------

    if (-not $SkipHost) {

        Section "2/5 HOST BUILD"

        & ".\md.bat" build host

        if ($LASTEXITCODE -ne 0) {
            throw "Host build failed with exit code $LASTEXITCODE"
        }

        Section "2/5 HOST CTEST"

        & ctest `
            --test-dir ".\build-host" `
            -C Release `
            --output-on-failure

        if ($LASTEXITCODE -ne 0) {
            throw "ctest failed with exit code $LASTEXITCODE"
        }
    }
    else {
        Section "2/5 HOST BUILD + CTEST - SKIPPED"
    }

    # ------------------------------------------------------------------
    # 3/5 QEMU DIFFERENTIAL
    # ------------------------------------------------------------------

    if (-not $SkipDiff) {

        Section "3/5 NV2-G QEMU DIFFERENTIAL"

        $diff = Join-Path `
            $Repo `
            "scripts\md_nativev2g_diff.ps1"

        if (-not (Test-Path -LiteralPath $diff)) {
            throw "Missing differential runner: $diff"
        }

        & $diff
    }
    else {
        Section "3/5 NV2-G QEMU DIFFERENTIAL - SKIPPED"
    }

    # ------------------------------------------------------------------
    # 4/5 REAL-TOOLS CENSUS
    # ------------------------------------------------------------------

    if (-not $SkipCensus) {

        Section "4/5 NV2-G REAL-TOOLS CENSUS"

        $census = Join-Path `
            $Repo `
            "scripts\md_nv2g_census.ps1"

        if (-not (Test-Path -LiteralPath $census)) {
            throw "Missing census runner: $census"
        }

        & $census
    }
    else {
        Section "4/5 NV2-G REAL-TOOLS CENSUS - SKIPPED"
    }

    # ------------------------------------------------------------------
    # 5/5 PICO SPLITBENCH
    # ------------------------------------------------------------------

    if (-not $SkipSplitbench) {

        Section "5/5 NV2-G PICO SPLITBENCH"

        $splitbench = Join-Path `
            $Repo `
            "scripts\md_nativev2g_splitbench.ps1"

        if (-not (Test-Path -LiteralPath $splitbench)) {
            throw "Missing splitbench runner: $splitbench"
        }

        & $splitbench
    }
    else {
        Section "5/5 NV2-G PICO SPLITBENCH - SKIPPED"
    }

    Section "G-2B1 VALIDATION COMPLETE"

    Write-Host "All requested stages completed."
    Write-Host ""
    Write-Host "Combined log:"
    Write-Host "  $log"
}
catch {

    Section "G-2B1 VALIDATION FAILED"

    Write-Host $_
    Write-Host ""
    Write-Host "Combined log:"
    Write-Host "  $log"

    throw
}
finally {

    try {
        Stop-Transcript | Out-Null
    }
    catch {
    }
}
