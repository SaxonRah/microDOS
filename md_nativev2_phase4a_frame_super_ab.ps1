param(
    [int]$CaptureSeconds = 300,
    [int]$SliceInstructions = 10000
)

$ErrorActionPreference = "Stop"

$Repo = (Get-Location).Path
$Interp = Join-Path $Repo "src\runtime\x86_interp.c"
$PicoMain = Join-Path $Repo "pico\microdos_pico.c"
$Bench = Join-Path $Repo "md_nativev2_phase3p_completion_splitbench.ps1"
$Picotool = "$HOME\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"

foreach ($p in @($Interp, $PicoMain, $Bench)) {
    if (-not (Test-Path $p)) { throw "Required file not found: $p" }
}
if ($SliceInstructions -lt 1000) { throw "SliceInstructions must be >= 1000." }

$InterpBytes = [IO.File]::ReadAllBytes($Interp)
$PicoBytes = [IO.File]::ReadAllBytes($PicoMain)
$InterpText = [Text.Encoding]::UTF8.GetString($InterpBytes)
$PicoText = [Text.Encoding]::UTF8.GetString($PicoBytes)
$Utf8NoBom = New-Object Text.UTF8Encoding($false)

function Replace-ExactlyOnce([string]$Text, [string]$Old, [string]$New, [string]$What) {
    $first = $Text.IndexOf($Old, [StringComparison]::Ordinal)
    if ($first -lt 0) { throw "Could not find marker for $What. No source files have been written." }
    $second = $Text.IndexOf($Old, $first + $Old.Length, [StringComparison]::Ordinal)
    if ($second -ge 0) { throw "Marker for $What was not unique. No source files have been written." }
    return $Text.Substring(0, $first) + $New + $Text.Substring($first + $Old.Length)
}

$globals = @'

/* Phase 4A diagnostic: measured DOS 2.0 frame superinstructions. */
static uint64_t g_md_super_frame_restore_hits;
static uint64_t g_md_super_frame_save_hits;
static uint64_t g_md_super_popret_hits;

uint64_t md_interp_super_frame_restore_hits(void) { return g_md_super_frame_restore_hits; }
uint64_t md_interp_super_frame_save_hits(void) { return g_md_super_frame_save_hits; }
uint64_t md_interp_super_popret_hits(void) { return g_md_super_popret_hits; }

void md_interp_super_profile_reset(void)
{
    g_md_super_frame_restore_hits = 0u;
    g_md_super_frame_save_hits = 0u;
    g_md_super_popret_hits = 0u;
}
'@

$InterpProfile = Replace-ExactlyOnce `
    $InterpText `
    "#include <string.h>" `
    ("#include <string.h>" + $globals) `
    "superinstruction counters"

$prefixFast = @'
op_prefix: {
    /*
     * Phase 4A measured DOS-kernel superinstructions.
     *
     * These are semantic byte-pattern fusions, not absolute-address hooks.
     * They match the DOS 2.0 save/restore trampoline observed at 1000:0680
     * and 1000:0694. The displacement word must agree between the initial
     * CS:POP and the converged CS:PUSH. Guest retirement and scheduler budget
     * remain exact; if the current budget cannot contain the whole fused
     * sequence, execution falls through to the ordinary interpreter.
     */
    if (opcode == 0x2Eu && remaining >= 11u) {
        const uint16_t s = ip_before;
        const uint8_t next_opcode = md_x86_read8(opcode_cpu, opcode_cpu->cs,
                                                  (uint16_t)(s + 1u));

        if (next_opcode == 0x8Fu &&
            md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 2u)) == 0x06u) {
            const uint16_t disp = (uint16_t)(
                (uint16_t)md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 3u)) |
                ((uint16_t)md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 4u)) << 8));

            /* Restore:
               CS:POP [disp]; POP AX,BX,CX,DX,SI,DI,BP,DS,ES;
               CS:PUSH [disp]; RET
               = 12 guest instructions total. */
            if (md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 5u))  == 0x58u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 6u))  == 0x5Bu &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 7u))  == 0x59u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 8u))  == 0x5Au &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 9u))  == 0x5Eu &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 10u)) == 0x5Fu &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 11u)) == 0x5Du &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 12u)) == 0x1Fu &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 13u)) == 0x07u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 14u)) == 0x2Eu &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 15u)) == 0xFFu &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 16u)) == 0x36u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 17u)) == (uint8_t)disp &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 18u)) == (uint8_t)(disp >> 8) &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 19u)) == 0xC3u) {
                uint16_t value;

                MD_STACK_POP16_FAST(value);
                md_x86_write16(opcode_cpu, opcode_cpu->cs, disp, value);

                MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_AX]);
                MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_BX]);
                MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_CX]);
                MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_DX]);
                MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_SI]);
                MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_DI]);
                MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_BP]);

                MD_STACK_POP16_FAST(value);
                opcode_cpu->ds = value;
                MD_STACK_POP16_FAST(value);
                opcode_cpu->es = value;

                value = md_x86_read16(opcode_cpu, opcode_cpu->cs, disp);
                MD_STACK_PUSH16_FAST(value);
                MD_STACK_POP16_FAST(opcode_cpu->ip);

                remaining -= 11u;
                done += 11u;
                ++g_md_super_frame_restore_hits;
                MD_NEXT();
            }

            /*
             * Save:
             * CS:POP [disp]; PUSH ES,DS,BP,DI,SI,DX,CX,BX,AX;
             * JMP target; target = CS:PUSH [disp]; RET
             * = 13 guest instructions total.
             */
            if (remaining >= 12u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 5u))  == 0x06u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 6u))  == 0x1Eu &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 7u))  == 0x55u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 8u))  == 0x57u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 9u))  == 0x56u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 10u)) == 0x52u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 11u)) == 0x51u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 12u)) == 0x53u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 13u)) == 0x50u &&
                md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 14u)) == 0xEBu) {
                const int8_t rel =
                    (int8_t)md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(s + 15u));
                const uint16_t target = (uint16_t)(s + 16u + rel);

                if (md_x86_read8(opcode_cpu, opcode_cpu->cs, target) == 0x2Eu &&
                    md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(target + 1u)) == 0xFFu &&
                    md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(target + 2u)) == 0x36u &&
                    md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(target + 3u)) == (uint8_t)disp &&
                    md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(target + 4u)) == (uint8_t)(disp >> 8) &&
                    md_x86_read8(opcode_cpu, opcode_cpu->cs, (uint16_t)(target + 5u)) == 0xC3u) {
                    uint16_t value;

                    MD_STACK_POP16_FAST(value);
                    md_x86_write16(opcode_cpu, opcode_cpu->cs, disp, value);

                    MD_STACK_PUSH16_FAST(opcode_cpu->es);
                    MD_STACK_PUSH16_FAST(opcode_cpu->ds);
                    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_BP]);
                    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_DI]);
                    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_SI]);
                    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_DX]);
                    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_CX]);
                    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_BX]);
                    MD_STACK_PUSH16_FAST(opcode_cpu->r[MD_X86_AX]);

                    value = md_x86_read16(opcode_cpu, opcode_cpu->cs, disp);
                    MD_STACK_PUSH16_FAST(value);
                    MD_STACK_POP16_FAST(opcode_cpu->ip);

                    remaining -= 12u;
                    done += 12u;
                    ++g_md_super_frame_save_hits;
                    MD_NEXT();
                }
            }
        }
    }

'@

$InterpProfile = Replace-ExactlyOnce `
    $InterpProfile `
    "op_prefix: {" `
    $prefixFast `
    "op_prefix label"

# Generic POP r16 ; RET superinstruction. 0x58 has its own label; 59..5F use op_pop_r16.
$popAxOld = @'
op_pop_ax:
    MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_AX]);
    MD_NEXT();
'@
$popAxNew = @'
op_pop_ax:
    MD_STACK_POP16_FAST(opcode_cpu->r[MD_X86_AX]);
    if (remaining != 0u &&
        md_x86_read8(opcode_cpu, opcode_cpu->cs, opcode_cpu->ip) == 0xC3u) {
        MD_STACK_POP16_FAST(opcode_cpu->ip);
        --remaining;
        ++done;
        ++g_md_super_popret_hits;
        MD_NEXT();
    }
    MD_NEXT();
'@
$InterpProfile = Replace-ExactlyOnce $InterpProfile $popAxOld $popAxNew "POP AX/RET fast path"

$popROld = @'
op_pop_r16:
    runtime->cpu.r[opcode & 7u] = md_x86_pop(&runtime->cpu);
    MD_NEXT();
'@
$popRNew = @'
op_pop_r16:
    runtime->cpu.r[opcode & 7u] = md_x86_pop(&runtime->cpu);
    if (remaining != 0u &&
        md_x86_read8(opcode_cpu, opcode_cpu->cs, opcode_cpu->ip) == 0xC3u) {
        MD_STACK_POP16_FAST(opcode_cpu->ip);
        --remaining;
        ++done;
        ++g_md_super_popret_hits;
        MD_NEXT();
    }
    MD_NEXT();
'@
$InterpProfile = Replace-ExactlyOnce $InterpProfile $popROld $popRNew "POP r16/RET fast path"

$PicoProfile = [regex]::Replace(
    $PicoText,
    '(?m)^#define\s+MD_SLICE\s+\d+u\s*$',
    "#define MD_SLICE $($SliceInstructions)u",
    1
)
if ($PicoProfile -eq $PicoText) { throw "Could not locate MD_SLICE in pico\microdos_pico.c." }

$report = @'
extern uint64_t md_interp_super_frame_restore_hits(void);
extern uint64_t md_interp_super_frame_save_hits(void);
extern uint64_t md_interp_super_popret_hits(void);
extern void md_interp_super_profile_reset(void);

static void md_interp_super_report_and_reset(void)
{
    md_say("[interp-super] frame-restore=%llu frame-save=%llu pop-ret=%llu\n",
           (unsigned long long)md_interp_super_frame_restore_hits(),
           (unsigned long long)md_interp_super_frame_save_hits(),
           (unsigned long long)md_interp_super_popret_hits());
    md_interp_super_profile_reset();
}

'@

$PicoProfile = Replace-ExactlyOnce `
    $PicoProfile `
    "static void md_stats(uint64_t start_us)" `
    ($report + "static void md_stats(uint64_t start_us)") `
    "superinstruction reporter"

$PicoProfile = Replace-ExactlyOnce `
    $PicoProfile `
    "    g_perf_mark=now;" `
    "    md_interp_super_report_and_reset();`r`n    g_perf_mark=now;" `
    "stats marker"

$Built = $false
try {
    [IO.File]::WriteAllText($Interp, $InterpProfile, $Utf8NoBom)
    [IO.File]::WriteAllText($PicoMain, $PicoProfile, $Utf8NoBom)

    Write-Host "=== Phase 4A DOS-frame superinstruction A/B ==="
    Write-Host "Temporary scheduler slice: $SliceInstructions"
    Write-Host "Fusions: DOS frame restore, DOS frame save, generic POP r16 + RET"
    Write-Host ""

    & cmake --build .\build-pico\out --target microdos_pico_nativev2
    if ($LASTEXITCODE -ne 0) {
        throw "Phase 4A diagnostic firmware build failed with exit code $LASTEXITCODE."
    }
    $Built = $true
}
finally {
    [IO.File]::WriteAllBytes($Interp, $InterpBytes)
    [IO.File]::WriteAllBytes($PicoMain, $PicoBytes)
    Write-Host "Restored x86_interp.c and microdos_pico.c byte-for-byte."
}

if (-not $Built) { exit 1 }

Write-Host ""
Write-Host "Running completion-boundary workload on Phase 4A diagnostic firmware..."
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
Write-Host "=== Phase 4A A/B complete ==="
Write-Host "Production sources restored and production Phase 3P rebuilt/reflashed."
Write-Host "Send back the log containing [interp-super] lines and the interval summary."
Write-Host ""

if ($BenchExit -ne 0) { exit $BenchExit }
