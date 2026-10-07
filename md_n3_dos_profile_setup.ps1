param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"

function Read-Normalized([string]$Path) {
    return ([IO.File]::ReadAllText($Path) -replace "`r`n", "`n")
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    $enc = [System.Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($Path, $Text, $enc)
}

function Replace-Once(
    [string]$Text,
    [string]$Old,
    [string]$New,
    [string]$Label
) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "anchor not found: $Label" }

    $second = $Text.IndexOf(
        $Old, $first + $Old.Length, [StringComparison]::Ordinal)

    if ($second -ge 0) { throw "anchor is not unique: $Label" }

    return $Text.Substring(0, $first) +
        $New +
        $Text.Substring($first + $Old.Length)
}

$CMake = Join-Path $Repo "pico\CMakeLists.txt"
$PicoC = Join-Path $Repo "pico\microdos_pico.c"
$Runner = Join-Path $Repo "scripts\md_native3_dos_profile.ps1"

foreach ($p in @($CMake, $PicoC)) {
    if (-not (Test-Path $p)) { throw "missing file: $p" }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backup = Join-Path $Repo "native3_dos_profile_backup\$stamp"
New-Item -ItemType Directory -Force -Path $backup | Out-Null
Copy-Item $CMake (Join-Path $backup "CMakeLists.txt") -Force
Copy-Item $PicoC (Join-Path $backup "microdos_pico.c") -Force

Write-Host "backup: $backup"

# ---------------------------------------------------------------------------
# pico/CMakeLists.txt
# Add an instrumented DOS target parallel to production microdos_pico_native3.
# ---------------------------------------------------------------------------

$c = Read-Normalized $CMake

$anchor = @'
pico_set_program_name(microdos_pico_native3 "microDOS Native-3 DOS")

add_executable(microdos_pico_native3_bench
'@

$replacement = @'
pico_set_program_name(microdos_pico_native3 "microDOS Native-3 DOS")

# Native-3 DOS profiling image. Same engine/policy as the production Native-3
# DOS target, but enables Native-3/JIT counters so Ctrl+] can prove ownership,
# backend split, CALL/RET shadow locality and Native-v2 graph use on real DOS.
md_pico_dos(microdos_pico_native3_profile 0 0 300000 0 0 0 0 0 0)
target_sources(microdos_pico_native3_profile PRIVATE
    "${MD_ROOT}/src/runtime/native3.c"
    "${MD_ROOT}/src/runtime/native_v2.c"
    "${MD_ROOT}/src/runtime/native_v2_runtime.c"
    "${MD_ROOT}/src/runtime/jit_core.c"
    "${MD_ROOT}/src/decode/x86_decode.c")
target_compile_definitions(microdos_pico_native3_profile PRIVATE
    MICRODOS_ENABLE_NATIVE3=1
    MD_N3_PROFILE=1
    MD_JIT_PROFILE=1
    MD_JIT_LEGACY_HOTNESS=0
    MD_JIT_BLOCK_SLOTS=256
    MD_X86_TRACK_WRITES=1
    "MICRODOS_NATIVE3_CODE_BYTES=(128u*1024u)")
pico_set_program_name(
    microdos_pico_native3_profile
    "microDOS Native-3 DOS profile")

add_executable(microdos_pico_native3_bench
'@

$c = Replace-Once $c $anchor $replacement "add Native-3 DOS profile target"
Write-Utf8NoBom $CMake $c

# ---------------------------------------------------------------------------
# pico/microdos_pico.c
# Add Native-3 Ctrl+] telemetry without changing execution policy.
# ---------------------------------------------------------------------------

$p = Read-Normalized $PicoC

$anchor = @'
#if MICRODOS_PICO_NATIVE_V2
    {
        const MdNativeV2Runtime *nv = &g_sys.native_v2;
'@

$n3block = @'
#if defined(MICRODOS_ENABLE_NATIVE3) && MD_N3_PROFILE
    {
        const MdN3Stats *n3 = md_native3_stats(&g_sys.native3);
        const MdNativeV2Runtime *nv = &g_sys.native3.nv2;

        md_say("[native3] owned=%llu retired=%llu native=%llu interp=%llu "
               "entries=%llu jit=%llu/%llu nv2=%llu/%llu\n",
               (unsigned long long)g_sys.native3_instructions,
               (unsigned long long)(n3 ? n3->retired : 0u),
               (unsigned long long)(n3 ? n3->native_retired : 0u),
               (unsigned long long)(n3 ? n3->interp_retired : 0u),
               (unsigned long long)(n3 ? n3->entries : 0u),
               (unsigned long long)(n3 ? n3->jit_entries : 0u),
               (unsigned long long)(n3 ? n3->jit_retired : 0u),
               (unsigned long long)(n3 ? n3->nv2_entries : 0u),
               (unsigned long long)(n3 ? n3->nv2_retired : 0u));

        md_say("[native3] lookup hit/miss=%llu/%llu compiles=%llu "
               "reject=%llu invalid=%llu shadow=%llu/%llu/%llu "
               "smc=%llu\n",
               (unsigned long long)(n3 ? n3->hits : 0u),
               (unsigned long long)(n3 ? n3->misses : 0u),
               (unsigned long long)(n3 ? n3->compiles : 0u),
               (unsigned long long)(n3 ? n3->rejects : 0u),
               (unsigned long long)(n3 ? n3->invalidations : 0u),
               (unsigned long long)(n3 ? n3->shadow_pushes : 0u),
               (unsigned long long)(n3 ? n3->shadow_hits : 0u),
               (unsigned long long)(n3 ? n3->shadow_misses : 0u),
               (unsigned long long)(n3 ? n3->smc_rejects : 0u));

        md_say("[native3-nv2] retired=%llu entries=%llu lookups=%llu "
               "hit/miss=%llu/%llu probes=%llu compiles=%llu "
               "compile-reject=%llu stack-guard=%llu budget=%llu short=%llu\n",
               (unsigned long long)nv->retired,
               (unsigned long long)nv->entries,
               (unsigned long long)nv->lookups,
               (unsigned long long)nv->cache_hits,
               (unsigned long long)nv->cache_misses,
               (unsigned long long)nv->probes,
               (unsigned long long)nv->compiles,
               (unsigned long long)nv->compile_rejects,
               (unsigned long long)nv->stack_guard_rejects,
               (unsigned long long)nv->budget_rejects,
               (unsigned long long)nv->short_rejects);
    }
#endif

#if MICRODOS_PICO_NATIVE_V2
    {
        const MdNativeV2Runtime *nv = &g_sys.native_v2;
'@

$p = Replace-Once $p $anchor $n3block "add Native-3 Ctrl+] telemetry"

$anchor = @'
    md_say("  keys:    Ctrl+] -> tier/native statistics (boot + interval)\n");
'@

$replacement = @'
#if defined(MICRODOS_ENABLE_NATIVE3)
    md_say("  n3:      Native-3 ON%s\n",
#if MD_N3_PROFILE
           " (profile counters ON)"
#else
           ""
#endif
    );
#endif
    md_say("  keys:    Ctrl+] -> tier/native statistics (boot + interval)\n");
'@

$p = Replace-Once $p $anchor $replacement "add Native-3 boot banner"
Write-Utf8NoBom $PicoC $p

# ---------------------------------------------------------------------------
# scripts/md_native3_dos_profile.ps1
# Build + flash + automate DOS2TEST/MDSTRESS + Ctrl+] stats in one command.
# ---------------------------------------------------------------------------

$runnerText = @'
param(
    [int]$CaptureSeconds = 180,
    [switch]$NoBuild,
    [switch]$NoFlash,
    [switch]$SkipStress
)

$ErrorActionPreference = "Stop"

$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $Repo "build-pico\out"
$Target = "microdos_pico_native3_profile"
$Uf2 = Join-Path $BuildDir "$Target.uf2"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

Push-Location $Repo

try {
    if (-not $NoBuild) {
        Write-Host ""
        Write-Host "============================================================"
        Write-Host "BUILD NATIVE-3 DOS PROFILE"
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
        Write-Host "FLASH NATIVE-3 DOS PROFILE"
        Write-Host "============================================================"

        try {
            & $Picotool reboot -f -u 2>$null
        } catch {}

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
        "pico-native3-dos-profile-" +
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
    Write-Host "      -> Ctrl+] Native-3 statistics"
    Write-Host ""

    try {
        $s.Open()
        $end = (Get-Date).AddSeconds($CaptureSeconds)

        while ((Get-Date) -lt $end) {
            $chunk = ($s.ReadExisting() -replace "`0","")

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

                if ($window -match '\[native3\] owned=') {
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

            # Act on prompts outside the "new bytes" block. This avoids
            # missing a DOS prompt that arrived in the previous read.
            if ($promptPending) {
                $ageMs = ((Get-Date) - $lastAction).TotalMilliseconds

                if ($stage -eq 0 -and $ageMs -ge 250) {
                    Send-Dos "DOS2TEST"
                    $stage = 1
                }
                elseif ($stage -eq 1 -and $ageMs -ge 500) {
                    if (-not $dos2Pass) {
                        Write-Warning "DOS2TEST returned to DOS without ALL TESTS PASSED"
                    }

                    if ($SkipStress) {
                        Write-Host ""
                        Write-Host ">>> Ctrl+] Native-3 statistics"
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
                    Write-Host ">>> Ctrl+] Native-3 statistics"
                    $s.Write([string][char]0x1D)
                    $stage = 3
                    $promptPending = $false
                    $window = ""
                    $lastAction = Get-Date
                }
            }

            if ($stage -eq 3 -and $statsSeen -and
                ((Get-Date) - $lastAction).TotalSeconds -gt 2) {
                break
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
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "NATIVE-3 DOS PROFILE RESULT"
    Write-Host "============================================================"
    Write-Host "DOS2TEST: $($dos2Pass ? 'PASS' : 'NOT CONFIRMED')"

    if (-not $SkipStress) {
        Write-Host "MDSTRESS: $($stressDone ? 'returned to DOS prompt' : 'NOT CONFIRMED')"
    }

    Write-Host "N3 stats: $($statsSeen ? 'CAPTURED' : 'NOT CAPTURED')"
    Write-Host "saved: $Log"

    if (-not $dos2Pass -or -not $statsSeen) {
        exit 2
    }

    exit 0
}
finally {
    Pop-Location
}
'@

New-Item -ItemType Directory -Force -Path (Split-Path $Runner) | Out-Null
Write-Utf8NoBom $Runner $runnerText

Write-Host ""
Write-Host "Applied Native-3 DOS profiling support."
Write-Host "Changed:"
Write-Host "  pico\CMakeLists.txt"
Write-Host "  pico\microdos_pico.c"
Write-Host "Created:"
Write-Host "  scripts\md_native3_dos_profile.ps1"
Write-Host ""
Write-Host "Next:"
Write-Host '  ctest --test-dir .\build-host -C Release --output-on-failure'
Write-Host '  .\scripts\md_native3_dos_profile.ps1'
