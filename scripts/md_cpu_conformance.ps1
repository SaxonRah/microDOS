param(
    [ValidateSet("smoke","quick","full","hard")]
    [string]$Mode = "quick",

    [ValidateSet("8086","8088","both")]
    [string]$Cpu = "8086",

    [int]$RandomCases = 1000,

    [string]$Opcode = "",

    [switch]$ReportOnly,

    [switch]$IncludePrefetchConflicts,

    [string]$Repo = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Repo)) {
    $Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
} else {
    $Repo = (Resolve-Path $Repo).Path
}

$SourceDir = Join-Path $Repo "tests\conformance"
$BuildDir = Join-Path $Repo "build-conformance\host"
$CacheDir = Join-Path $Repo "build-conformance\vectors"
$ReportDir = Join-Path $Repo "build-conformance\reports"

foreach ($p in @(
    (Join-Path $SourceDir "CMakeLists.txt"),
    (Join-Path $SourceDir "test_cpu_silicon.c"),
    (Join-Path $SourceDir "test_random_router_diff.c"),
    (Join-Path $Repo "scripts\md_silicon_tests.py")
)) {
    if (-not (Test-Path $p -PathType Leaf)) {
        throw "Required conformance file missing: $p"
    }
}

New-Item -ItemType Directory -Force -Path $BuildDir, $CacheDir, $ReportDir | Out-Null

Write-Host "=== microDOS broad 8086 conformance v4 ==="
Write-Host "Repo:         $Repo"
Write-Host "Mode:         $Mode"
Write-Host "CPU vectors:  $Cpu"
Write-Host "Random cases: $RandomCases"
Write-Host "Prefetch-conflict vectors: $(if ($IncludePrefetchConflicts) {'included'} else {'filtered'})"
if ($Opcode) { Write-Host "Opcode only:  $Opcode" }
Write-Host ""

Write-Host "[1/5] Configuring isolated conformance build..."
& cmake -S $SourceDir -B $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Conformance CMake configure failed." }

Write-Host ""
Write-Host "[2/5] Building physical-silicon runner..."
& cmake --build $BuildDir --config Release --target microdos_silicon_runner
$SiliconBuildRc = $LASTEXITCODE
if ($SiliconBuildRc -ne 0) {
    throw "Silicon runner build failed with exit code $SiliconBuildRc."
}

$SiliconExe = Join-Path $BuildDir "microdos_silicon_runner.exe"
if (Test-Path (Join-Path $BuildDir "Release\microdos_silicon_runner.exe")) {
    $SiliconExe = Join-Path $BuildDir "Release\microdos_silicon_runner.exe"
}
if (-not (Test-Path $SiliconExe)) { throw "Silicon runner not found: $SiliconExe" }

$Python = $null
if (Get-Command py -ErrorAction SilentlyContinue) {
    $Python = @("py", "-3")
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $Python = @("python")
} elseif (Get-Command python3 -ErrorAction SilentlyContinue) {
    $Python = @("python3")
} else {
    throw "Python 3 was not found (tried py, python, python3)."
}

$Driver = Join-Path $Repo "scripts\md_silicon_tests.py"
$Report = Join-Path $ReportDir ("silicon-{0}-{1}.json" -f $Cpu, $Mode)

$DriverArgs = @(
    $Driver,
    "--runner", $SiliconExe,
    "--cache", $CacheDir,
    "--report", $Report,
    "--cpu", $Cpu,
    "--mode", $Mode
)
if ($Opcode) {
    $DriverArgs += @("--opcode", $Opcode)
}
if ($ReportOnly) {
    $DriverArgs += "--report-only"
}
if ($IncludePrefetchConflicts) {
    $DriverArgs += "--include-prefetch-conflicts"
}

Write-Host ""
Write-Host "[3/5] Running physical-silicon SingleStepTests vectors..."
if ($Python.Count -eq 2) {
    & $Python[0] $Python[1] @DriverArgs
} else {
    & $Python[0] @DriverArgs
}
$SiliconRc = $LASTEXITCODE

if ($SiliconRc -ne 0 -and -not $ReportOnly) {
    throw "Silicon conformance failed with exit code $SiliconRc."
}

Write-Host ""
Write-Host "[4/5] Building deterministic randomized router differential..."
& cmake --build $BuildDir --config Release --target microdos_random_router_diff
$RandomBuildRc = $LASTEXITCODE

$RandomRc = 0
if ($RandomBuildRc -eq 0) {
    $RandomExe = Join-Path $BuildDir "microdos_random_router_diff.exe"
    if (Test-Path (Join-Path $BuildDir "Release\microdos_random_router_diff.exe")) {
        $RandomExe = Join-Path $BuildDir "Release\microdos_random_router_diff.exe"
    }
    if (-not (Test-Path $RandomExe)) {
        throw "Random differential runner not found: $RandomExe"
    }

    Write-Host ""
    Write-Host "[5/5] Running deterministic randomized router differential..."
    & $RandomExe $RandomCases 0xC0FFEE11
    $RandomRc = $LASTEXITCODE
} else {
    Write-Warning "Random differential target build failed with exit code $RandomBuildRc."
    $RandomRc = $RandomBuildRc
    if (-not $ReportOnly) {
        throw "Random differential target build failed."
    }
}

Write-Host ""
Write-Host "=== CONFORMANCE COMPLETE ==="
Write-Host "silicon rc=$SiliconRc"
Write-Host "random-build rc=$RandomBuildRc"
Write-Host "random-diff rc=$RandomRc"
Write-Host "report: $Report"
Write-Host ""
Write-Host "Useful runs:"
Write-Host "  .\scripts\md_cpu_conformance.ps1 -Mode smoke -Cpu 8086 -ReportOnly"
Write-Host "  .\scripts\md_cpu_conformance.ps1 -Mode quick -Cpu 8086 -ReportOnly"
Write-Host "  .\scripts\md_cpu_conformance.ps1 -Mode full -Cpu 8086 -ReportOnly"
Write-Host "  .\scripts\md_cpu_conformance.ps1 -Mode hard -Cpu 8086 -ReportOnly"
Write-Host "  .\scripts\md_cpu_conformance.ps1 -Mode full -Cpu 8086 -Opcode F7 -ReportOnly"
Write-Host ""

if (($SiliconRc -ne 0 -or $RandomRc -ne 0) -and -not $ReportOnly) {
    exit 1
}
exit 0
