param(
    [string]$HostConfig = "Release"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Get-LatestNative3Backup {
    $dirs = Get-ChildItem (Join-Path $repo "native3_backup") -Directory -ErrorAction Stop |
        Sort-Object Name -Descending
    foreach ($d in $dirs) {
        if ((Test-Path (Join-Path $d.FullName "src\system\md_dos2_system.c")) -and
            (Test-Path (Join-Path $d.FullName "src\system\md_dos2_system.h")) -and
            (Test-Path (Join-Path $d.FullName "CMakeLists.txt"))) {
            return $d.FullName
        }
    }
    throw "No Native-3 backup containing md_dos2_system.c/.h was found."
}

function Show-Fat12Root([string]$img) {
    Write-Host ""
    Write-Host "=== FAT12 ROOT DIRECTORY ==="

    if ([string]::IsNullOrWhiteSpace($img)) {
        throw "FAT12 image path is empty."
    }
    $img = $img.Trim()
    if ($img.Contains("`r") -or $img.Contains("`n")) {
        throw "FAT12 image path unexpectedly contains a newline: [$img]"
    }
    if (-not (Test-Path -LiteralPath $img)) {
        throw "FAT12 image does not exist: $img"
    }

    [byte[]]$b = [System.IO.File]::ReadAllBytes($img)
    if ($b.Length -lt 4096) { throw "Image is unexpectedly small: $($b.Length) bytes" }

    $bps = [BitConverter]::ToUInt16($b, 11)
    $reserved = [BitConverter]::ToUInt16($b, 14)
    $fats = $b[16]
    $rootEntries = [BitConverter]::ToUInt16($b, 17)
    $spf = [BitConverter]::ToUInt16($b, 22)
    $rootSector = $reserved + ($fats * $spf)
    $rootOffset = $rootSector * $bps

    $names = @()
    for ($i = 0; $i -lt $rootEntries; $i++) {
        $off = $rootOffset + ($i * 32)
        $first = $b[$off]
        if ($first -eq 0) { break }
        if ($first -eq 0xE5) { continue }
        $attr = $b[$off + 11]
        if (($attr -band 0x0F) -eq 0x0F) { continue }

        $base = [Text.Encoding]::ASCII.GetString($b, $off, 8).Trim()
        $ext = [Text.Encoding]::ASCII.GetString($b, $off + 8, 3).Trim()
        $name = if ($ext) { "$base.$ext" } else { $base }
        if ($name) {
            $names += $name
            Write-Host ("  {0,-12} attr=0x{1:X2}" -f $name, $attr)
        }
    }

    foreach ($need in @("COMMAND.COM","DOS2TEST.COM")) {
        if ($names -contains $need) {
            Write-Host "  REQUIRED $need : PASS"
        } else {
            Write-Host "  REQUIRED $need : FAIL"
        }
    }
}

function Build-E2E {
    & cmake -S . -B .\build-host
    if ($LASTEXITCODE -ne 0) { throw "Host configure failed: $LASTEXITCODE" }

    & cmake --build .\build-host --config $HostConfig --target `
        microdos_dos2_e2e `
        microdos_dos2_e2e_g128 `
        microdos_dos2_e2e_router_baseline `
        microdos_dos2_e2e_router_promote `
        microdos_dos2_e2e_router_lean `
        mkfat12
    if ($LASTEXITCODE -ne 0) { throw "E2E build failed: $LASTEXITCODE" }
}

function Make-Image {
    $mk = Join-Path $repo "build-host\$HostConfig\mkfat12.exe"
    $command = Join-Path $repo "third_party\msdos\v2.0\bin\COMMAND.COM"
    $testcom = Join-Path $repo "tests\dos2\DOS2TEST.COM"
    $img = Join-Path $repo "build-host\e2e_msdos2.img"

    if (-not (Test-Path $mk)) {
        throw "mkfat12 executable missing: $mk"
    }
    if (-not (Test-Path $command)) {
        throw "COMMAND.COM missing: $command"
    }
    if (-not (Test-Path $testcom)) {
        throw "DOS2TEST.COM missing: $testcom"
    }

    Remove-Item $img -Force -ErrorAction SilentlyContinue

    # IMPORTANT: capture the tool's success-stream output so it does not
    # become part of this PowerShell function's return value.
    $mkOutput = & $mk `
        --command $command `
        --add $testcom DOS2TEST.COM `
        --output $img 2>&1
    $rc = $LASTEXITCODE

    foreach ($line in $mkOutput) {
        Write-Host $line
    }

    if ($rc -ne 0) {
        throw "mkfat12 image creation failed: $rc"
    }
    if (-not (Test-Path $img)) {
        throw "mkfat12 reported success but image is missing: $img"
    }

    $resolved = (Resolve-Path $img).Path
    Write-Host "FAT12 image: $resolved"
    Write-Host "FAT12 bytes: $((Get-Item $resolved).Length)"

    # The ONLY success-stream object returned by this function is the path.
    return [string]$resolved
}

function Run-Direct([string]$tag, [string]$img) {
    $exe = Join-Path $repo "build-host\$HostConfig\microdos_dos2_e2e.exe"
    $kernel = Join-Path $repo "third_party\msdos\v2.0\bin\MSDOS.SYS"
    $log = Join-Path $repo "logs\native3-e2e-$tag.txt"
    New-Item (Split-Path $log) -ItemType Directory -Force | Out-Null

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "DIRECT E2E: $tag"
    Write-Host "============================================================"

    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $exe $kernel $img --no-aot --no-cache 2>&1 | Tee-Object -FilePath $log
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $old

    $txt = Get-Content $log -Raw
    foreach ($needle in @("Enter new date","Enter new time","A>","DOS2TEST","ALL TESTS PASSED","passed: 25")) {
        $hit = $txt.Contains($needle)
        Write-Host ("  {0,-20} {1}" -f $needle, ($(if($hit){"FOUND"}else{"MISSING"})))
    }
    Write-Host "  exit code: $rc"
    Write-Host "  log: $log"
}

Push-Location $repo
$work = Join-Path $repo "native3_e2e_isolate_work"
$backup = Get-LatestNative3Backup
$currentC = Join-Path $repo "src\system\md_dos2_system.c"
$currentH = Join-Path $repo "src\system\md_dos2_system.h"
$currentRootCMake = Join-Path $repo "CMakeLists.txt"
$baseC = Join-Path $backup "src\system\md_dos2_system.c"
$baseH = Join-Path $backup "src\system\md_dos2_system.h"
$baseRootCMake = Join-Path $backup "CMakeLists.txt"

try {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    New-Item $work -ItemType Directory -Force | Out-Null
    Copy-Item $currentC (Join-Path $work "current_md_dos2_system.c") -Force
    Copy-Item $currentH (Join-Path $work "current_md_dos2_system.h") -Force
    Copy-Item $currentRootCMake (Join-Path $work "current_CMakeLists.txt") -Force

    Write-Host "Native-3 backup used: $backup"
    Write-Host "Current C SHA256 : $((Get-FileHash $currentC -Algorithm SHA256).Hash)"
    Write-Host "Baseline C SHA256: $((Get-FileHash $baseC -Algorithm SHA256).Hash)"
    Write-Host "Current H SHA256 : $((Get-FileHash $currentH -Algorithm SHA256).Hash)"
    Write-Host "Baseline H SHA256: $((Get-FileHash $baseH -Algorithm SHA256).Hash)"
    Write-Host "Current root CMake SHA256 : $((Get-FileHash $currentRootCMake -Algorithm SHA256).Hash)"
    Write-Host "Baseline root CMake SHA256: $((Get-FileHash $baseRootCMake -Algorithm SHA256).Hash)"

    Write-Host ""
    Write-Host "=== CURRENT N3-INTEGRATED SYSTEM ==="
    Build-E2E
    $img = Make-Image
    Show-Fat12Root $img
    Run-Direct "current" $img

    Write-Host ""
    Write-Host "=== TEMPORARILY RESTORING PRE-N3 SHARED BUILD + SYSTEM FILES ==="
    Copy-Item $baseC $currentC -Force
    Copy-Item $baseH $currentH -Force
    Copy-Item $baseRootCMake $currentRootCMake -Force

    # Force CMake to regenerate the VS project from the pre-N3 source graph.
    & cmake -S . -B .\build-host
    if ($LASTEXITCODE -ne 0) { throw "Baseline host configure failed: $LASTEXITCODE" }

    Build-E2E
    $img = Make-Image
    Show-Fat12Root $img
    Run-Direct "baseline-shared" $img

    Write-Host ""
    Write-Host "=== BASELINE-SYSTEM CTEST SUBSET ==="
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & ctest --test-dir .\build-host -C $HostConfig `
        -R "^dos2_e2e_(aot|interp|nocache|lockstep|g128|router_baseline|router_promote|router_lean)$" `
        --output-on-failure
    $ctrc = $LASTEXITCODE
    $ErrorActionPreference = $old
    Write-Host "Baseline-system E2E subset exit code: $ctrc"
}
finally {
    Write-Host ""
    Write-Host "=== RESTORING CURRENT NATIVE-3 SYSTEM FILES ==="
    if (Test-Path (Join-Path $work "current_md_dos2_system.c")) {
        Copy-Item (Join-Path $work "current_md_dos2_system.c") $currentC -Force
    }
    if (Test-Path (Join-Path $work "current_md_dos2_system.h")) {
        Copy-Item (Join-Path $work "current_md_dos2_system.h") $currentH -Force
    }
    if (Test-Path (Join-Path $work "current_CMakeLists.txt")) {
        Copy-Item (Join-Path $work "current_CMakeLists.txt") $currentRootCMake -Force
    }

    # Put build-host back on the current Native-3 project graph too.
    & cmake -S . -B .\build-host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "WARNING: failed to regenerate current Native-3 build graph: $LASTEXITCODE"
    }
    Pop-Location
}

Write-Host ""
Write-Host "Isolation complete."
Write-Host "Compare:"
Write-Host "  logs\native3-e2e-current.txt"
Write-Host "  logs\native3-e2e-baseline-shared.txt"
