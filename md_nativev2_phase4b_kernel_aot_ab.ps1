param(
    [int]$CaptureSeconds = 300,
    [int]$SliceInstructions = 10000
)

$ErrorActionPreference = "Stop"

$Repo = (Get-Location).Path
$CMake = Join-Path $Repo "pico\CMakeLists.txt"
$PicoMain = Join-Path $Repo "pico\microdos_pico.c"
$Bench = Join-Path $Repo "md_nativev2_phase3p_completion_splitbench.ps1"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
$Uf2 = Join-Path $Repo "build-pico\out\microdos_pico_nativev2.uf2"

foreach ($p in @($CMake, $PicoMain, $Bench)) {
    if (-not (Test-Path $p)) { throw "Required file not found: $p" }
}
if ($SliceInstructions -lt 1000) { throw "SliceInstructions must be >= 1000." }

$CMakeBytes = [IO.File]::ReadAllBytes($CMake)
$PicoBytes = [IO.File]::ReadAllBytes($PicoMain)
$CMakeText = [Text.Encoding]::UTF8.GetString($CMakeBytes)
$PicoText = [Text.Encoding]::UTF8.GetString($PicoBytes)
$Utf8NoBom = New-Object Text.UTF8Encoding($false)

if ($CMakeText -notmatch '(?m)\bmicrodos_pico_nativev2\b') {
    throw "microdos_pico_nativev2 target was not found in pico\CMakeLists.txt."
}

function Configure-Production {
    Write-Host "Configuring restored production tree..."
    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Production Pico reconfigure failed with exit code $LASTEXITCODE."
    }
}

# IMPORTANT: the prior v1 failure restored the source files but could leave
# build-pico\out configured with the temporary target properties. Normalize
# the build directory from the real source tree before doing anything else.
Configure-Production

$PicoHybrid = [regex]::Replace(
    $PicoText,
    '(?m)^#define\s+MD_SLICE\s+\d+u\s*$',
    "#define MD_SLICE $($SliceInstructions)u",
    1
)
if ($PicoHybrid -eq $PicoText) {
    throw "Could not locate MD_SLICE in pico\microdos_pico.c."
}

$HybridBlock = @'

# ---- TEMPORARY Phase 4B v2: kernel-AOT + Native-v2 A/B ------------------
if(TARGET microdos_pico_nativev2)
    get_target_property(_md_nv2_defs microdos_pico_nativev2 COMPILE_DEFINITIONS)
    if(NOT _md_nv2_defs)
        set(_md_nv2_defs "")
    endif()

    # The production Native-v2 target deliberately strips all translation/AOT
    # bookkeeping. Kernel AOT needs those fields and tracked writes back.
    list(REMOVE_ITEM _md_nv2_defs
        "MICRODOS_PICO_KERNEL_AOT=0"
        "MICRODOS_PICO_KERNEL_AOT=1"
        "MICRODOS_PICO_DOS2TEST_AOT=0"
        "MICRODOS_PICO_DOS2TEST_AOT=1"
        "MICRODOS_SYSTEM_ENABLE_AOT=0"
        "MICRODOS_SYSTEM_ENABLE_AOT=1"
        "MICRODOS_TRANSLATION_SUPPORT=0"
        "MICRODOS_TRANSLATION_SUPPORT=1"
        "MD_X86_TRACK_WRITES=0"
        "MD_X86_TRACK_WRITES=1")

    list(APPEND _md_nv2_defs
        "MICRODOS_PICO_KERNEL_AOT=1"
        "MICRODOS_PICO_DOS2TEST_AOT=0"
        "MICRODOS_SYSTEM_ENABLE_AOT=1"
        "MICRODOS_TRANSLATION_SUPPORT=1"
        "MD_X86_TRACK_WRITES=1")

    set_property(TARGET microdos_pico_nativev2
        PROPERTY COMPILE_DEFINITIONS "${_md_nv2_defs}")

    # Generated static-AOT fallback has a link-time dependency on the block
    # cache implementation even though the live runtime cache remains OFF.
    target_sources(microdos_pico_nativev2 PRIVATE
        "${MD_ROOT}/src/runtime/x86_block_cache.c"
        "${MD_GENERATED}/msdos2_recomp.c")
endif()
# ---- END TEMPORARY Phase 4B v2 -------------------------------------------
'@

$CMakeHybrid = $CMakeText + $HybridBlock

$HybridBuilt = $false
$HybridRan = $false
$BenchExit = 0

try {
    [IO.File]::WriteAllText($CMake, $CMakeHybrid, $Utf8NoBom)
    [IO.File]::WriteAllText($PicoMain, $PicoHybrid, $Utf8NoBom)

    Write-Host ""
    Write-Host "=== Phase 4B v2 kernel-AOT + Native-v2 A/B ==="
    Write-Host "Temporary scheduler slice: $SliceInstructions"
    Write-Host "MSDOS.SYS static AOT: ON"
    Write-Host "DOS2TEST static AOT: OFF"
    Write-Host "Native v2: ON"
    Write-Host "Translation support: ON (temporary)"
    Write-Host "Tracked writes/AOT invalidation: ON (temporary)"
    Write-Host ""

    & cmake -S .\pico -B .\build-pico\out
    if ($LASTEXITCODE -ne 0) {
        throw "Temporary Pico reconfigure failed with exit code $LASTEXITCODE."
    }

    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Phase 4B v2 hybrid firmware build failed with exit code $LASTEXITCODE."
    }

    if (-not (Test-Path $Uf2)) {
        throw "Hybrid build completed but UF2 was not found: $Uf2"
    }

    $HybridBuilt = $true
}
finally {
    # Restore source bytes no matter how configure/build ended.
    [IO.File]::WriteAllBytes($CMake, $CMakeBytes)
    [IO.File]::WriteAllBytes($PicoMain, $PicoBytes)
    Write-Host "Restored pico\CMakeLists.txt and pico\microdos_pico.c byte-for-byte."

    if (-not $HybridBuilt) {
        Write-Host "Hybrid did not build; restoring production CMake state now..."
        & cmake -S .\pico -B .\build-pico\out
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Source files are restored, but production CMake reconfigure failed."
        } else {
            & cmake --build .\build-pico\out --target microdos_pico_nativev2
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "Source/CMake state restored, but production firmware rebuild failed."
            } else {
                Write-Host "Production Phase 3P build state restored."
            }
        }
    }
}

if (-not $HybridBuilt) { exit 1 }

Write-Host ""
Write-Host "Running completion-boundary workload on kernel-AOT + Native-v2 firmware..."
& $Bench -CaptureSeconds $CaptureSeconds
$BenchExit = $LASTEXITCODE
$HybridRan = $true

Write-Host ""
Write-Host "Reconfiguring and rebuilding production Phase 3P from restored sources..."
Configure-Production

& cmake --build .\build-pico\out --target microdos_pico_nativev2
if ($LASTEXITCODE -ne 0) {
    throw "Production Phase 3P rebuild failed with exit code $LASTEXITCODE."
}

Write-Host "Reflashing production Phase 3P..."
try {
    & $Picotool reboot -f -u
    Start-Sleep -Milliseconds 1000
} catch {}

& $Picotool load -v -x $Uf2
if ($LASTEXITCODE -ne 0) {
    throw "Production firmware rebuild succeeded, but reflashing failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "=== Phase 4B v2 kernel-AOT hybrid A/B complete ==="
Write-Host "Production source/CMake restored byte-for-byte."
Write-Host "Production Phase 3P rebuilt and reflashed."
Write-Host "Send back the full console/log output."
Write-Host ""

if ($BenchExit -ne 0) { exit $BenchExit }
