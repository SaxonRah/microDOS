param(
    [string]$Repo = "C:\microDOS"
)

$ErrorActionPreference = "Stop"

function Read-Normalized([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    return ($text -replace "`r`n", "`n")
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
    if ($first -lt 0) {
        throw "anchor not found: $Label"
    }

    $second = $Text.IndexOf(
        $Old,
        $first + $Old.Length,
        [StringComparison]::Ordinal
    )

    if ($second -ge 0) {
        throw "anchor is not unique: $Label"
    }

    return $Text.Substring(0, $first) +
        $New +
        $Text.Substring($first + $Old.Length)
}

$Native3 = Join-Path $Repo "src\runtime\native3.c"
$TestN3 = Join-Path $Repo "tests\test_native3.c"
$Bench = Join-Path $Repo "pico\microdos_native3_bench.c"

foreach ($p in @($Native3, $TestN3, $Bench)) {
    if (-not (Test-Path $p)) {
        throw "missing file: $p"
    }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backup = Join-Path $Repo "native3_shadow_backup\$stamp"
New-Item -ItemType Directory -Force -Path $backup | Out-Null

Copy-Item $Native3 (Join-Path $backup "native3.c") -Force
Copy-Item $TestN3 (Join-Path $backup "test_native3.c") -Force
Copy-Item $Bench (Join-Path $backup "microdos_native3_bench.c") -Force

Write-Host "backup: $backup"

# ---------------------------------------------------------------------------
# src/runtime/native3.c
# ---------------------------------------------------------------------------

$n3 = Read-Normalized $Native3

$old = @'
static int n3_shadow_pop(MdNative3 *n3, uint16_t cs, uint16_t ip)
{
    if (n3->shadow_count) {
        unsigned idx = (unsigned)((n3->shadow_top + MD_N3_SHADOW_SLOTS - 1u) % MD_N3_SHADOW_SLOTS);
        MdN3Shadow *e = &n3->shadow[idx];
        if (e->valid && e->return_cs == cs && e->return_ip == ip) {
            e->valid = 0u; n3->shadow_top = (uint8_t)idx; --n3->shadow_count;
            N3STAT_INC(n3, shadow_hits); return 1;
        }
    }
    N3STAT_INC(n3, shadow_misses); return 0;
}
'@

$new = @'
static int n3_shadow_pop(MdNative3 *n3, uint16_t cs, uint16_t ip)
{
    if (n3->shadow_count) {
        unsigned idx = (unsigned)((n3->shadow_top + MD_N3_SHADOW_SLOTS - 1u) % MD_N3_SHADOW_SLOTS);
        MdN3Shadow *e = &n3->shadow[idx];
        if (e->valid && e->return_cs == cs && e->return_ip == ip) {
            e->valid = 0u; n3->shadow_top = (uint8_t)idx; --n3->shadow_count;
            N3STAT_INC(n3, shadow_hits); return 1;
        }
    }
    N3STAT_INC(n3, shadow_misses); return 0;
}

/*
 * N3.2 shadow metadata must describe control transfers that actually retired,
 * never transfers merely discovered by look-ahead probing.
 *
 * n3_probe() may decode several fallthrough instructions before reaching the
 * first CALL/RET. A scheduler budget can stop before that control instruction,
 * so committing at probe time creates phantom calls. Only commit when the
 * execution episode retired exactly through the probed control boundary.
 *
 * RET matching uses the architectural post-RET CS:IP, which is the predicted
 * return address stored by the corresponding CALL.
 */
static void n3_shadow_commit(MdNative3 *n3, MdRuntime *rt,
                             const N3Probe *p, uint64_t retired)
{
    if (n3 == NULL || rt == NULL || p == NULL)
        return;

    if (!p->call && !p->ret)
        return;

    if (retired != (uint64_t)p->decoded)
        return;

    if (p->call && p->direct) {
        n3_shadow_push(
            n3,
            rt->cpu.cs,
            p->next_ip,
            rt->cpu.cs,
            p->target);
    } else if (p->ret) {
        (void)n3_shadow_pop(n3, rt->cpu.cs, rt->cpu.ip);
    }
}
'@

$n3 = Replace-Once $n3 $old $new "insert n3_shadow_commit"

$old = @'
            if (p.call && p.direct) n3_shadow_push(n3,rt->cpu.cs,p.next_ip,rt->cpu.cs,p.target);
            else if (p.ret) (void)n3_shadow_pop(n3,rt->cpu.cs,rt->cpu.ip);

'@

$n3 = Replace-Once $n3 $old "" "remove probe-time shadow mutation"

$old = @'
                    { uint64_t r=n3_interp(n3,rt,left); total+=r; interp+=r; if(!r)break; continue; }
'@

$new = @'
                    {
                        uint64_t r=n3_interp(n3,rt,left);
                        n3_shadow_commit(n3,rt,&p,r);
                        total+=r; interp+=r; if(!r)break; continue;
                    }
'@

$n3 = Replace-Once $n3 $old $new "compile-reject shadow commit"

$old = @'
                uint64_t r = n3_interp(n3, rt, left);
                total += r;
                interp += r;
                if (!r) break;
                continue;
'@

$new = @'
                uint64_t r = n3_interp(n3, rt, left);
                n3_shadow_commit(n3,rt,&p,r);
                total += r;
                interp += r;
                if (!r) break;
                continue;
'@

$n3 = Replace-Once $n3 $old $new "portable shadow commit"

$old = @'
                if (jr.retired) {
                    total+=jr.retired; native+=jr.native; interp+=jr.fallback;
                    N3STAT_INC(n3, jit_entries); N3STAT_ADD(n3, jit_retired, jr.native);
'@

$new = @'
                if (jr.retired) {
                    n3_shadow_commit(n3,rt,&p,jr.retired);
                    total+=jr.retired; native+=jr.native; interp+=jr.fallback;
                    N3STAT_INC(n3, jit_entries); N3STAT_ADD(n3, jit_retired, jr.native);
'@

$n3 = Replace-Once $n3 $old $new "JIT shadow commit"

$old = @'
                if (rt->instructions==before) {
                    uint64_t r=n3_interp(n3,rt,left); total+=r; interp+=r; if(!r)break;
                }
'@

$new = @'
                if (rt->instructions==before) {
                    uint64_t r=n3_interp(n3,rt,left);
                    n3_shadow_commit(n3,rt,&p,r);
                    total+=r; interp+=r; if(!r)break;
                }
'@

$n3 = Replace-Once $n3 $old $new "zero-progress shadow commit"

Write-Utf8NoBom $Native3 $n3

# ---------------------------------------------------------------------------
# tests/test_native3.c
# ---------------------------------------------------------------------------

$t = Read-Normalized $TestN3

$marker = @'
static int cache_roundtrip(void)
'@

$shadowTest = @'
static int shadow_budget_run(void)
{
    static const uint8_t p[] = {
        0xB9,0x01,0x00,             /* mov cx,1 */
        0xBB,0x00,0x00,             /* mov bx,0 */
        0xE8,0x04,0x00,             /* call 010d */
        0x49,                       /* dec cx */
        0x75,0xFA,                  /* jnz 0106 */
        0xF4,                       /* hlt */
        0x83,0xC3,0x03,             /* add bx,3 */
        0xC3                        /* ret */
    };
    MdHooks h;
    MdN3RunResult rr;
    const MdN3Stats *st;
    unsigned guard = 0u;

    puts("native3 shadow BEGIN");

    memset(&h, 0, sizeof(h));
    memset(g_mem_a, 0, sizeof(g_mem_a));
    memset(&g_rt_a, 0, sizeof(g_rt_a));
    memset(&g_n3_a, 0, sizeof(g_n3_a));

    memcpy(g_mem_a + 0x100, p, sizeof(p));

    md_runtime_init(&g_rt_a, g_mem_a, &h);
    g_rt_a.cpu.cs = 0u;
    g_rt_a.cpu.ip = 0x100u;
    g_rt_a.cpu.ss = 0u;
    g_rt_a.cpu.r[MD_X86_SP] = 0xFFFEu;

    md_native3_init(&g_n3_a, g_code_a, sizeof(g_code_a));

    while (g_rt_a.stop_reason == MD_STOP_NONE && guard++ < 32u) {
        if (!md_native3_run(&g_n3_a, &g_rt_a, 1u, &rr))
            break;
    }

    st = md_native3_stats(&g_n3_a);

    if (g_rt_a.stop_reason != MD_STOP_HALT ||
        g_rt_a.cpu.r[MD_X86_BX] != 3u ||
        g_rt_a.cpu.r[MD_X86_SP] != 0xFFFEu ||
        st == NULL ||
        st->shadow_pushes != 1u ||
        st->shadow_hits != 1u ||
        st->shadow_misses != 0u) {
        printf(
            "native3 shadow FAIL stop=%u BX=%04x SP=%04x "
            "push=%llu hit=%llu miss=%llu\n",
            (unsigned)g_rt_a.stop_reason,
            g_rt_a.cpu.r[MD_X86_BX],
            g_rt_a.cpu.r[MD_X86_SP],
            (unsigned long long)(st ? st->shadow_pushes : 0u),
            (unsigned long long)(st ? st->shadow_hits : 0u),
            (unsigned long long)(st ? st->shadow_misses : 0u));
        return 0;
    }

    puts("native3 shadow PASS");
    return 1;
}

static int cache_roundtrip(void)
'@

$t = Replace-Once $t $marker $shadowTest "insert shadow budget regression"

$old = @'
    ok &= exact_run();
    ok &= cache_roundtrip();
'@

$new = @'
    ok &= exact_run();
    ok &= shadow_budget_run();
    ok &= cache_roundtrip();
'@

$t = Replace-Once $t $old $new "run shadow budget regression"

Write-Utf8NoBom $TestN3 $t

# ---------------------------------------------------------------------------
# pico/microdos_native3_bench.c
# Add shadow observability to the existing gate line. No benchmark semantics
# or workload sizes change.
# ---------------------------------------------------------------------------

$b = Read-Normalized $Bench

$old = 'printf(" cyc/guest GATE40=%s native=%llu interp=%llu\n",m>=40000u?"PASS":"FAIL",(unsigned long long)n3.stats.native_retired,(unsigned long long)n3.stats.interp_retired);'

$new = 'printf(" cyc/guest GATE40=%s native=%llu interp=%llu shadow=%llu/%llu/%llu\n",m>=40000u?"PASS":"FAIL",(unsigned long long)n3.stats.native_retired,(unsigned long long)n3.stats.interp_retired,(unsigned long long)n3.stats.shadow_pushes,(unsigned long long)n3.stats.shadow_hits,(unsigned long long)n3.stats.shadow_misses);'

$b = Replace-Once $b $old $new "add Pico shadow stats"

Write-Utf8NoBom $Bench $b

Write-Host ""
Write-Host "Applied Native-3 shadow-retirement fix."
Write-Host "Changed:"
Write-Host "  src\runtime\native3.c"
Write-Host "  tests\test_native3.c"
Write-Host "  pico\microdos_native3_bench.c"
Write-Host ""
Write-Host "Next:"
Write-Host '  .\md.bat build host'
Write-Host '  ctest --test-dir .\build-host -C Release -R "^native3$" -V'
Write-Host '  .\scripts\md_pico_flash_capture.ps1 -Build -Label "native3-shadow-fixed" -CaptureSeconds 90'
