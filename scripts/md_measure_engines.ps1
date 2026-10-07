param(
    # DOS firmwares to splitbench, in order. Label=target.
    [string[]]$DosTargets = @(
        "interp=microdos_pico_nokernel",          # pure threaded interpreter (floor)
        "blockcache=microdos_pico_region",        # decoded block cache ON
        "nv2=microdos_pico_nativev2",             # Native v2 (current winner)
        "n3flash=microdos_pico_native3",          # Native-3 production image (code in flash)
        "n3sram=microdos_pico_native3_profile",   # Native-3, code in SRAM, counters on
        "m25=microdos_pico_m25",                  # M25 translator alone
        "m25nv2=microdos_pico_m25_nv2",           # M25 + Native v2
        "aot=microdos_pico"                       # standard kernel/app AOT image (reference)
    ),

    # Microbenchmark firmwares (no DOS). Label=target.
    [string[]]$BenchTargets = @(
        "n3bench=microdos_pico_native3_bench",
        "trfast=microdos_pico_translate_diff_fast"
    ),

    [int]$SplitbenchSeconds = 420,
    [int]$BenchSeconds = 1200,
    [switch]$SkipBuild,
    [switch]$SkipBench
)

# microDOS engine measurement sweep.
#
#   cd C:\microDOS
#   .\scripts\md_measure_engines.ps1
#
# Produces logs\measure-<stamp>\SUMMARY.txt (paste this) and raw-logs.zip.
# Builds each target separately; a target that fails to build or capture is
# recorded and the sweep continues. Expect roughly 45-60 minutes.

$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $Repo

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$OutDir = Join-Path $Repo (Join-Path "logs" ("measure-" + $Stamp))
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Summary = Join-Path $OutDir "SUMMARY.txt"
$RawDir = Join-Path $OutDir "raw"
New-Item -ItemType Directory -Force -Path $RawDir | Out-Null

function Say([string]$text) {
    Write-Host $text
    Add-Content -Path $Summary -Value $text -Encoding UTF8
}

function Split-Spec([string]$spec) {
    $p = $spec.Split("=", 2)
    return @{ Label = $p[0]; Target = $p[1] }
}

function Build-Target([string]$target) {
    if ($SkipBuild) { return $true }
    Write-Host ""
    Write-Host "=== BUILD $target ==="
    # Out-Host keeps cmake's text out of this function's return value.
    & cmake --build ".\build-pico\out" --target $target | Out-Host
    return ($LASTEXITCODE -eq 0)
}

function Newest-Log([string]$pattern, [datetime]$since) {
    $f = Get-ChildItem -Path (Join-Path $Repo "logs") -Filter $pattern -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $since } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    return $f
}

# Keep per-interval engine lines; drop cumulative "since boot" blocks and
# per-slot listings so the summary stays readable.
function Digest-Splitbench([string]$path) {
    $out = New-Object System.Collections.Generic.List[string]
    $skip = $false
    $configShown = $false
    foreach ($line in Get-Content -Path $path) {
        if ($line -match '^=== (SPLIT INTERVAL|RUN):') { $out.Add($line); continue }
        if ($line -match 'since boot ---') { $skip = $true; continue }
        if ($line -match 'since previous') { $skip = $false; continue }
        if ($skip) { continue }
        if ($line -match '^\[perf\] config:') {
            if (-not $configShown) { $out.Add($line); $configShown = $true }
            continue
        }
        if ($line -match '^\[native-v2\] (reject-)?slot') { continue }
        if ($line -match '^\[(perf|m25|n3|N3|jit|JIT|native-v2|native3|cache|region|aot)') { $out.Add($line); continue }
        if ($line -match 'deterministic checksum|passed: \d+\s+failed: \d+|^\s+(guest|m25|config|aot):') { $out.Add($line) }
    }
    return $out
}

$commit = (& git rev-parse --short HEAD 2>$null)
$dirty = (& git status --porcelain 2>$null | Measure-Object).Count
Say "microDOS engine measurement sweep $Stamp"
Say ("commit {0}  dirty-files {1}" -f $commit, $dirty)
Say ""

$table = New-Object System.Collections.Generic.List[object]
$digests = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------- DOS images
foreach ($spec in $DosTargets) {
    $s = Split-Spec $spec
    $label = $s.Label; $target = $s.Target
    $uf2 = ".\build-pico\out\$target.uf2"

    if (-not (Build-Target $target)) {
        $table.Add([pscustomobject]@{ Firmware = $label; Interval = "BUILD FAILED"; MIPS = ""; Native = ""; Interp = ""; AOT = ""; JIT = "" })
        continue
    }
    if (-not (Test-Path $uf2)) {
        $table.Add([pscustomobject]@{ Firmware = $label; Interval = "NO UF2"; MIPS = ""; Native = ""; Interp = ""; AOT = ""; JIT = "" })
        continue
    }

    $t0 = Get-Date
    try {
        & (Join-Path $PSScriptRoot "md_engine_splitbench.ps1") -Uf2 $uf2 -Label ("measure-" + $label) -CaptureSeconds $SplitbenchSeconds
    } catch {
        Write-Host "splitbench error for ${label}: $_"
    }

    $csv = Newest-Log ("engine-splitbench-measure-" + $label + "-*.csv") $t0
    $txt = Newest-Log ("engine-splitbench-measure-" + $label + "-*.txt") $t0
    if ($txt) { Copy-Item $txt.FullName $RawDir }
    if ($csv) {
        Copy-Item $csv.FullName $RawDir
        foreach ($r in Import-Csv $csv.FullName) {
            $table.Add([pscustomobject]@{
                Firmware = $label; Interval = $r.Interval; MIPS = $r.MIPS
                Native = $r.NativePct; Interp = $r.InterpPct; AOT = $r.AOTPct; JIT = $r.JITPct })
        }
    } else {
        $table.Add([pscustomobject]@{ Firmware = $label; Interval = "NO RESULT"; MIPS = ""; Native = ""; Interp = ""; AOT = ""; JIT = "" })
    }
    if ($txt) {
        $digests.Add("")
        $digests.Add("################ $label ($target) ################")
        foreach ($l in (Digest-Splitbench $txt.FullName)) { $digests.Add($l) }
    }
}

# ---------------------------------------------------------------- microbenchmarks
$benchLines = New-Object System.Collections.Generic.List[string]
if (-not $SkipBench) {
    foreach ($spec in $BenchTargets) {
        $s = Split-Spec $spec
        $label = $s.Label; $target = $s.Target
        if (-not (Build-Target $target)) { $benchLines.Add("$label ($target): BUILD FAILED"); continue }
        $t0 = Get-Date
        try {
            & (Join-Path $PSScriptRoot "md_pico_flash_capture.ps1") -Uf2 ".\build-pico\out\$target.uf2" `
                -BuildTarget $target -Label ("measure-" + $label) -CaptureSeconds $BenchSeconds
        } catch {
            Write-Host "capture error for ${label}: $_"
        }
        $log = Newest-Log ("pico-measure-" + $label + "-*.txt") $t0
        if (-not $log) { $benchLines.Add("$label ($target): NO LOG"); continue }
        Copy-Item $log.FullName $RawDir
        $benchLines.Add("")
        $benchLines.Add("################ $label ($target) ################")
        foreach ($l in Get-Content $log.FullName) {
            if ($l -match '\[n3\]|\[translate-bench\]|\[translate-diff\] (eager|tiered|COMPLETE|clock)|\[translate-directed\]|GATE40|sys=') {
                $benchLines.Add($l)
            }
        }
    }
}

# ---------------------------------------------------------------- summary
Say "=== DOS SPLITBENCH (interval MIPS and tier shares; NV2 column = native engine share) ==="
Say ("{0,-11} {1,-14} {2,9} {3,8} {4,8} {5,7} {6,7}" -f "firmware", "interval", "MIPS", "native%", "interp%", "aot%", "jit%")
foreach ($r in $table) {
    Say ("{0,-11} {1,-14} {2,9} {3,8} {4,8} {5,7} {6,7}" -f $r.Firmware, $r.Interval, $r.MIPS, $r.Native, $r.Interp, $r.AOT, $r.JIT)
}
Say ""
Say "=== MICROBENCHMARKS ==="
foreach ($l in $benchLines) { Say $l }
Say ""
Say "=== ENGINE COUNTERS PER INTERVAL ==="
foreach ($l in $digests) { Say $l }

$zip = Join-Path $OutDir "raw-logs.zip"
Compress-Archive -Path (Join-Path $RawDir "*") -DestinationPath $zip -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============================================================"
Write-Host "Done. Paste this file:"
Write-Host "  $Summary"
Write-Host "Full logs (if more detail is needed):"
Write-Host "  $zip"
Write-Host "============================================================"
