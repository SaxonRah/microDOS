param(
    [int]$CaptureSeconds = 300,
    [int]$SliceInstructions = 10000
)

$ErrorActionPreference = "Stop"

$Repo = (Get-Location).Path
$Interp = Join-Path $Repo "src\runtime\x86_interp.c"
$RuntimeH = Join-Path $Repo "include\microdos\runtime.h"
$PicoMain = Join-Path $Repo "pico\microdos_pico.c"
$Bench = Join-Path $Repo "md_nativev2_phase3p_completion_splitbench.ps1"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

foreach ($p in @($Interp, $RuntimeH, $PicoMain, $Bench)) {
    if (-not (Test-Path $p)) {
        throw "Required file not found: $p"
    }
}
if ($SliceInstructions -lt 1000) {
    throw "SliceInstructions must be >= 1000."
}

$InterpBytes = [IO.File]::ReadAllBytes($Interp)
$RuntimeHBytes = [IO.File]::ReadAllBytes($RuntimeH)
$PicoBytes = [IO.File]::ReadAllBytes($PicoMain)

$Utf8NoBom = New-Object Text.UTF8Encoding($false)
$InterpText = [Text.Encoding]::UTF8.GetString($InterpBytes)
$RuntimeHText = [Text.Encoding]::UTF8.GetString($RuntimeHBytes)
$PicoText = [Text.Encoding]::UTF8.GetString($PicoBytes)

function Replace-ExactlyOnce([string]$Text, [string]$Old, [string]$New, [string]$What) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Could not find marker for $What. No source files have been written." }
    $second = $Text.IndexOf($Old, $first + $Old.Length, [StringComparison]::Ordinal)
    if ($second -ge 0) { throw "Marker for $What was not unique. No source files have been written." }
    return $Text.Substring(0, $first) + $New + $Text.Substring($first + $Old.Length)
}

$RuntimeHProfile = Replace-ExactlyOnce `
    $RuntimeHText `
    "#define MD_INTERP_OPCODE_PROFILE 0" `
    "#define MD_INTERP_OPCODE_PROFILE 1" `
    "MD_INTERP_OPCODE_PROFILE default"

$pcGlobals = @'
static uint32_t g_md_pc_profile_keys[4096];
static uint32_t g_md_pc_profile_counts[4096];
static uint8_t g_md_pc_profile_ops[4096];
static uint32_t g_md_pc_profile_used;
static uint32_t g_md_pc_profile_dropped;

const uint32_t *md_interp_pc_profile_keys(void) { return g_md_pc_profile_keys; }
const uint32_t *md_interp_pc_profile_counts(void) { return g_md_pc_profile_counts; }
const uint8_t *md_interp_pc_profile_ops(void) { return g_md_pc_profile_ops; }
uint32_t md_interp_pc_profile_slots(void) { return 4096u; }
uint32_t md_interp_pc_profile_used(void) { return g_md_pc_profile_used; }
uint32_t md_interp_pc_profile_dropped(void) { return g_md_pc_profile_dropped; }

void md_interp_pc_profile_reset(void)
{
    memset(g_md_pc_profile_keys, 0, sizeof(g_md_pc_profile_keys));
    memset(g_md_pc_profile_counts, 0, sizeof(g_md_pc_profile_counts));
    memset(g_md_pc_profile_ops, 0, sizeof(g_md_pc_profile_ops));
    g_md_pc_profile_used = 0u;
    g_md_pc_profile_dropped = 0u;
}

static inline void md_interp_pc_profile_hit(uint16_t cs, uint16_t ip, uint8_t opcode)
{
    const uint32_t key = (((uint32_t)cs << 16) | (uint32_t)ip) + 1u;
    uint32_t slot = ((key * 2654435761u) >> 20) & 4095u;
    uint32_t probe;

    for (probe = 0u; probe < 4096u; ++probe) {
        if (g_md_pc_profile_keys[slot] == key) {
            if (g_md_pc_profile_counts[slot] != UINT32_MAX) ++g_md_pc_profile_counts[slot];
            return;
        }
        if (g_md_pc_profile_keys[slot] == 0u) {
            g_md_pc_profile_keys[slot] = key;
            g_md_pc_profile_counts[slot] = 1u;
            g_md_pc_profile_ops[slot] = opcode;
            ++g_md_pc_profile_used;
            return;
        }
        slot = (slot + 1u) & 4095u;
    }
    ++g_md_pc_profile_dropped;
}

#define MD_PC_PROFILE_HIT(cs_, ip_, op_) \
    md_interp_pc_profile_hit((uint16_t)(cs_), (uint16_t)(ip_), (uint8_t)(op_))
'@

$InterpProfile = Replace-ExactlyOnce `
    $InterpText `
    "static uint32_t g_md_hot_modrm_profile[10u * 256u];" `
    ("static uint32_t g_md_hot_modrm_profile[10u * 256u];`r`n" + $pcGlobals) `
    "PC profiler globals"

$InterpProfile = Replace-ExactlyOnce `
    $InterpProfile `
    "#define MD_OPCODE_PROFILE_HIT(op) ((void)0)" `
    "#define MD_OPCODE_PROFILE_HIT(op) ((void)0)`r`n#define MD_PC_PROFILE_HIT(cs_, ip_, op_) ((void)0)" `
    "PC profiler disabled macro"

# The threaded dispatch lives inside the MD_NEXT() macro, so each source
# line ends in a C-preprocessor continuation backslash. Match that shape
# independent of CRLF/LF and indentation.
$dispatchPattern = '(?m)^([ \t]*)MD_OPCODE_PROFILE_HIT[ \t]*\([ \t]*opcode[ \t]*\)[ \t]*;[ \t]*\\[ \t]*\r?\n[ \t]*goto[ \t]+\*dispatch[ \t]*\[[ \t]*opcode[ \t]*\][ \t]*;[ \t]*\\[ \t]*$'
$dispatchMatches = [regex]::Matches($InterpProfile, $dispatchPattern)
if ($dispatchMatches.Count -ne 1) {
    throw "Could not uniquely locate threaded MD_NEXT() dispatch (found $($dispatchMatches.Count)). No source files have been written."
}
$dispatchReplacement = '$1MD_OPCODE_PROFILE_HIT(opcode); \' + "`r`n" +
                       '$1MD_PC_PROFILE_HIT(opcode_cpu->cs, ip_before, opcode); \' + "`r`n" +
                       '$1goto *dispatch[opcode]; \'
$InterpProfile = [regex]::Replace(
    $InterpProfile,
    $dispatchPattern,
    $dispatchReplacement,
    1
)

$PicoProfile = [regex]::Replace(
    $PicoText,
    '(?m)^#define\s+MD_SLICE\s+\d+u\s*$',
    "#define MD_SLICE $($SliceInstructions)u",
    1
)
if ($PicoProfile -eq $PicoText) {
    throw "Could not locate MD_SLICE in pico\microdos_pico.c."
}

$reportCode = @'
#if MD_INTERP_OPCODE_PROFILE
extern const uint32_t *md_interp_pc_profile_keys(void);
extern const uint32_t *md_interp_pc_profile_counts(void);
extern const uint8_t *md_interp_pc_profile_ops(void);
extern uint32_t md_interp_pc_profile_slots(void);
extern uint32_t md_interp_pc_profile_used(void);
extern uint32_t md_interp_pc_profile_dropped(void);
extern void md_interp_pc_profile_reset(void);

static void md_profile_insert_top(uint32_t count, uint32_t key, uint8_t op,
                                  uint32_t *top_count, uint32_t *top_key,
                                  uint8_t *top_op, unsigned n)
{
    unsigned i, j;
    if (count == 0u) return;
    for (i = 0u; i < n; ++i) {
        if (count > top_count[i]) {
            for (j = n - 1u; j > i; --j) {
                top_count[j] = top_count[j - 1u];
                top_key[j] = top_key[j - 1u];
                top_op[j] = top_op[j - 1u];
            }
            top_count[i] = count;
            top_key[i] = key;
            top_op[i] = op;
            return;
        }
    }
}

static void md_interp_profile_report_and_reset(void)
{
    const uint32_t *keys = md_interp_pc_profile_keys();
    const uint32_t *counts = md_interp_pc_profile_counts();
    const uint8_t *ops = md_interp_pc_profile_ops();
    const uint32_t *opcode_counts = md_interp_opcode_profile_counts();
    uint32_t top_count[32] = {0};
    uint32_t top_key[32] = {0};
    uint8_t top_op[32] = {0};
    uint32_t op_top_count[24] = {0};
    uint32_t op_top_key[24] = {0};
    uint64_t pc_total = 0u, op_total = 0u;
    uint32_t i;
    unsigned rank;

    for (i = 0u; i < md_interp_pc_profile_slots(); ++i) {
        if (keys[i] == 0u || counts[i] == 0u) continue;
        pc_total += counts[i];
        md_profile_insert_top(counts[i], keys[i] - 1u, ops[i],
                              top_count, top_key, top_op, 32u);
    }

    for (i = 0u; i < 256u; ++i) {
        unsigned j;
        op_total += opcode_counts[i];
        if (opcode_counts[i] == 0u) continue;
        for (j = 0u; j < 24u; ++j) {
            if (opcode_counts[i] > op_top_count[j]) {
                unsigned k;
                for (k = 23u; k > j; --k) {
                    op_top_count[k] = op_top_count[k - 1u];
                    op_top_key[k] = op_top_key[k - 1u];
                }
                op_top_count[j] = opcode_counts[i];
                op_top_key[j] = i;
                break;
            }
        }
    }

    md_say("[interp-pc] total=%llu used=%lu dropped=%lu\n",
           (unsigned long long)pc_total,
           (unsigned long)md_interp_pc_profile_used(),
           (unsigned long)md_interp_pc_profile_dropped());

    for (rank = 0u; rank < 32u && top_count[rank] != 0u; ++rank) {
        const uint32_t key = top_key[rank];
        const uint16_t cs = (uint16_t)(key >> 16);
        const uint16_t ip = (uint16_t)key;
        md_say("[interp-pc] %02u %04X:%04X op=%02X count=%lu\n",
               rank + 1u, cs, ip, top_op[rank],
               (unsigned long)top_count[rank]);
    }

    md_say("[interp-op] total=%llu\n", (unsigned long long)op_total);
    for (rank = 0u; rank < 24u && op_top_count[rank] != 0u; ++rank) {
        md_say("[interp-op] %02u op=%02lX count=%lu\n",
               rank + 1u,
               (unsigned long)op_top_key[rank],
               (unsigned long)op_top_count[rank]);
    }

    md_interp_opcode_profile_reset();
    md_interp_pc_profile_reset();
}
#endif

'@

$PicoProfile = Replace-ExactlyOnce `
    $PicoProfile `
    "static void md_stats(uint64_t start_us)" `
    ($reportCode + "static void md_stats(uint64_t start_us)") `
    "interpreter profile reporter"

$markReplacement = @'
#if MD_INTERP_OPCODE_PROFILE
    md_interp_profile_report_and_reset();
#endif
    g_perf_mark=now;
'@
$PicoProfile = Replace-ExactlyOnce `
    $PicoProfile `
    "    g_perf_mark=now;" `
    $markReplacement `
    "profile report call"

Write-Host "=== Phase 3P interpreted-PC/opcode profile ==="
Write-Host "Temporary scheduler slice: $SliceInstructions"
Write-Host "PC table: 4096 exact open-addressed entries"
Write-Host "Top 32 interpreted PCs + top 24 primary opcodes printed at each Ctrl+]"
Write-Host ""

$ProfileBuilt = $false
try {
    [IO.File]::WriteAllText($RuntimeH, $RuntimeHProfile, $Utf8NoBom)
    [IO.File]::WriteAllText($Interp, $InterpProfile, $Utf8NoBom)
    [IO.File]::WriteAllText($PicoMain, $PicoProfile, $Utf8NoBom)

    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Interpreter-profile firmware build failed with exit code $LASTEXITCODE."
    }
    $ProfileBuilt = $true
}
finally {
    [IO.File]::WriteAllBytes($RuntimeH, $RuntimeHBytes)
    [IO.File]::WriteAllBytes($Interp, $InterpBytes)
    [IO.File]::WriteAllBytes($PicoMain, $PicoBytes)
    Write-Host "Restored runtime.h, x86_interp.c, and microdos_pico.c byte-for-byte."
}

if (-not $ProfileBuilt) { exit 1 }

Write-Host ""
Write-Host "Running completion-boundary workload on the profiling firmware..."
& $Bench -CaptureSeconds $CaptureSeconds
$BenchExit = $LASTEXITCODE

Write-Host ""
Write-Host "Rebuilding production Phase 3P firmware from restored sources..."
& cmake --build .\build-pico\out --target microdos_pico_nativev2
if ($LASTEXITCODE -ne 0) {
    throw "Diagnostic run finished, but production firmware rebuild failed with exit code $LASTEXITCODE."
}

$Uf2 = Join-Path $Repo "build-pico\out\microdos_pico_nativev2.uf2"
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
Write-Host "=== Interpreted-PC/opcode profile complete ==="
Write-Host "Production sources restored and production Phase 3P rebuilt/reflashed."
Write-Host "Send back the log containing [interp-pc] and [interp-op] lines."
Write-Host ""

if ($BenchExit -ne 0) { exit $BenchExit }
