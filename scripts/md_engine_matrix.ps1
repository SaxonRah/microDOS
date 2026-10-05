param(
    [string]$Repo = "C:\microDOS",
    [int]$CaptureSeconds = 300
)

$ErrorActionPreference = "Stop"

$repoPath = (Resolve-Path $Repo).Path
$runner = Join-Path $repoPath "md_engine_splitbench.ps1"

if (-not (Test-Path $runner)) {
    throw "Missing runner: $runner"
}

$targets = @(
    @{ target = "microdos_pico_nokernel"; label = "interp" },
    @{ target = "microdos_pico";          label = "aot" },
    @{ target = "microdos_pico_jitkernel";label = "jit" },
    @{ target = "microdos_pico_jit";      label = "aot-jit" },
    @{ target = "microdos_pico_nativev2"; label = "native-v2" }
)

Push-Location $repoPath
try {
    $status = git status --porcelain
    if ($status) {
        Write-Warning "Working tree is not clean. Benchmarking anyway; no source files will be modified."
        git status --short
    }

    foreach ($t in $targets) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "ENGINE: $($t.label)"
        Write-Host "TARGET: $($t.target)"
        Write-Host "============================================================"

        cmake --build .\build-pico\out --target $t.target
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed: $($t.target)"
        }

        $uf2 = ".\build-pico\out\$($t.target).uf2"
        if (-not (Test-Path $uf2)) {
            throw "UF2 missing after build: $uf2"
        }

        & $runner `
            -Uf2 $uf2 `
            -Label $t.label `
            -CaptureSeconds $CaptureSeconds

        if ($LASTEXITCODE -ne 0) {
            throw "Benchmark failed: $($t.label)"
        }
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "ENGINE MATRIX COMPLETE"
    Write-Host "============================================================"
    Write-Host "Per-engine CSV files are under .\logs\engine-splitbench-*.csv"
    Write-Host ""
    Write-Host "Latest CSV files:"
    Get-ChildItem .\logs\engine-splitbench-*.csv |
        Sort-Object LastWriteTime |
        Select-Object -Last 5 |
        Format-Table Name, LastWriteTime -AutoSize
}
finally {
    Pop-Location
}
