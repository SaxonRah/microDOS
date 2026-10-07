#include "microdos/native3.h"
#include "microdos/decode.h"
#include "microdos/hot_code.h"

#include <string.h>

_Static_assert(MD_N3_SITE_SLOTS && (MD_N3_SITE_SLOTS & (MD_N3_SITE_SLOTS - 1u)) == 0u,
               "MD_N3_SITE_SLOTS must be power-of-two");
_Static_assert(sizeof(MdN3Site) <= 24u, "Native-3 site grew");

#if MD_N3_PROFILE
#define N3STAT_INC(n3, f) do { ++(n3)->stats.f; } while (0)
#define N3STAT_ADD(n3, f, v) do { (n3)->stats.f += (uint64_t)(v); } while (0)
#else
#define N3STAT_INC(n3, f) do { (void)(n3); } while (0)
#define N3STAT_ADD(n3, f, v) do { (void)(n3); (void)(v); } while (0)
#endif

static unsigned n3_hash(uint16_t cs, uint16_t ip)
{
    uint32_t x = ((uint32_t)cs << 16) | ip;
    x ^= x >> 11; x *= 0x9e3779b1u; x ^= x >> 16;
    return (unsigned)(x & (MD_N3_SITE_SLOTS - 1u));
}

static uint32_t n3_fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t n3_signature(const MdRuntime *rt, uint16_t cs, uint16_t ip)
{
    uint8_t b[16];
    unsigned i;
    for (i = 0; i < sizeof(b); ++i)
        b[i] = md_x86_read8(&rt->cpu, cs, (uint16_t)(ip + i));
    return n3_fnv1a(b, sizeof(b));
}

static void n3_pages(MdN3Site *s, const MdRuntime *rt, uint16_t cs, uint16_t ip)
{
    uint32_t a0 = md_x86_linear(cs, ip);
    uint32_t a1 = md_x86_linear(cs, (uint16_t)(ip + 31u));
    s->page0 = (uint8_t)md_x86_code_page(a0);
    s->page1 = (uint8_t)md_x86_code_page(a1);
    s->page_count = s->page0 == s->page1 ? 1u : 2u;
    s->gen0 = rt->code_page_generation[s->page0];
    s->gen1 = rt->code_page_generation[s->page1];
}

static int n3_fresh(const MdN3Site *s, const MdRuntime *rt)
{
    if (s->state == MD_N3_SITE_EMPTY) return 0;
    if (s->page_count && rt->code_page_generation[s->page0] != s->gen0) return 0;
    if (s->page_count > 1u && rt->code_page_generation[s->page1] != s->gen1) return 0;
    return 1;
}

static MdN3Site *n3_site(MdNative3 *n3, MdRuntime *rt, uint16_t cs, uint16_t ip)
{
    MdN3Site *s = &n3->site[n3_hash(cs, ip)];
    N3STAT_INC(n3, lookups);
    if (s->state != MD_N3_SITE_EMPTY && s->cs == cs && s->ip == ip) {
        if (n3_fresh(s, rt)) { N3STAT_INC(n3, hits); return s; }
        memset(s, 0, sizeof(*s));
        N3STAT_INC(n3, invalidations);
    } else if (s->state != MD_N3_SITE_EMPTY) {
        memset(s, 0, sizeof(*s));
    }
    N3STAT_INC(n3, misses);
    s->cs = cs; s->ip = ip;
    s->signature = n3_signature(rt, cs, ip);
    n3_pages(s, rt, cs, ip);
    s->state = MD_N3_SITE_SEEN;
    return s;
}

typedef struct N3Probe {
    uint16_t next_ip, target;
    uint8_t opcode, flow, direct, backedge, call, ret, memory, store, decoded;
} N3Probe;

static int n3_probe(MdRuntime *rt, uint16_t cs, uint16_t ip, N3Probe *p)
{
    uint8_t win[96];
    uint16_t cur = ip;
    unsigned i;
    memset(p, 0, sizeof(*p));
    for (i = 0; i < sizeof(win); ++i)
        win[i] = md_x86_read8(&rt->cpu, cs, (uint16_t)(ip + i));

    for (i = 0; i < 24u; ++i) {
        MdDecodedInstruction in;
        size_t off = (size_t)(uint16_t)(cur - ip);
        if (off >= sizeof(win) || !md_decode_8086(win, sizeof(win), ip, cur, &in) || !in.valid_8086)
            return p->decoded != 0u;
        ++p->decoded;
        p->opcode = in.opcode; p->next_ip = in.next_ip; p->target = in.target; p->flow = in.flow;
        if (in.has_modrm && (in.modrm >> 6) != 3u) p->memory = 1u;
        if (in.opcode == 0x88u || in.opcode == 0x89u || in.opcode == 0xAAu ||
            in.opcode == 0xABu || in.opcode == 0xA2u || in.opcode == 0xA3u) p->store = 1u;
        if (in.flow == MD_DECODE_FLOW_CALL) { p->call = 1u; p->direct = !in.far_control; break; }
        if (in.flow == MD_DECODE_FLOW_RETURN) { p->ret = 1u; break; }
        if (in.flow == MD_DECODE_FLOW_CONDITIONAL || in.flow == MD_DECODE_FLOW_JUMP) {
            p->direct = !in.far_control; p->backedge = in.target <= in.ip; break;
        }
        if (in.flow != MD_DECODE_FLOW_FALLTHROUGH) break;
        cur = in.next_ip;
    }
    return p->decoded != 0u;
}

static void n3_shadow_push(MdNative3 *n3, uint16_t ret_cs, uint16_t ret_ip,
                           uint16_t target_cs, uint16_t target_ip)
{
    MdN3Shadow *e = &n3->shadow[n3->shadow_top];
    e->return_cs = ret_cs; e->return_ip = ret_ip;
    e->target_cs = target_cs; e->target_ip = target_ip; e->valid = 1u;
    n3->shadow_top = (uint8_t)((n3->shadow_top + 1u) % MD_N3_SHADOW_SLOTS);
    if (n3->shadow_count < MD_N3_SHADOW_SLOTS) ++n3->shadow_count;
    N3STAT_INC(n3, shadow_pushes);
}

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

static int n3_prepare_jit(MdNative3 *n3, MdRuntime *rt, MdN3Site *s)
{
    MdJitProbe probe;
    N3STAT_INC(n3, probes);
    if (!md_jit_probe(&n3->jit, rt, s->cs, s->ip, &probe) || !md_jit_prepare_region(&n3->jit, rt)) {
        ++s->penalty; N3STAT_INC(n3, rejects); return 0;
    }
#if defined(__aarch64__)
    s->backend = MD_N3_BACKEND_A64_JIT;
#elif defined(__arm__) || defined(__thumb__)
    s->backend = MD_N3_BACKEND_THUMB_JIT;
#else
    s->backend = MD_N3_BACKEND_PORTABLE;
#endif
    s->state = MD_N3_SITE_NATIVE; N3STAT_INC(n3, compiles); return 1;
}

static void n3_prefetch(MdNative3 *n3, MdRuntime *rt, const N3Probe *root)
{
    uint16_t save_cs = rt->cpu.cs, save_ip = rt->cpu.ip;
    uint16_t ip = root->next_ip;
    unsigned depth;
    if (!root->direct || root->backedge || root->call || root->ret) return;
    for (depth = 0; depth < MD_N3_CFG_PREFETCH; ++depth) {
        MdN3Site *s; N3Probe p;
        rt->cpu.cs = save_cs; rt->cpu.ip = ip;
        s = n3_site(n3, rt, save_cs, ip);
        if (s->state != MD_N3_SITE_NATIVE) (void)n3_prepare_jit(n3, rt, s);
        if (!n3_probe(rt, save_cs, ip, &p) || !p.direct || p.backedge || p.call || p.ret) break;
        ip = p.next_ip;
    }
    rt->cpu.cs = save_cs; rt->cpu.ip = save_ip;
}

static uint64_t n3_interp(MdNative3 *n3, MdRuntime *rt, uint64_t budget)
{
    uint64_t before = rt->instructions;
    uint64_t q = budget < MD_N3_INTERP_ESCAPE ? budget : MD_N3_INTERP_ESCAPE;
    MdStopReason st;
    if (!q) return 0;
    st = md_interp_run_until_cs_change(rt, q);
    if (st == MD_STOP_BUDGET) rt->stop_reason = MD_STOP_NONE;
    N3STAT_INC(n3, interp_escapes);
    N3STAT_ADD(n3, interp_retired, rt->instructions - before);
    return rt->instructions - before;
}

void md_native3_init(MdNative3 *n3, void *jit_code, size_t jit_code_bytes)
{
    if (!n3) return;
    memset(n3, 0, sizeof(*n3));
    n3->jit_code = (uint8_t *)jit_code; n3->jit_code_bytes = jit_code_bytes;
    n3->profile = MD_N3_PROFILE ? 1u : 0u;
    md_jit_init(&n3->jit, jit_code, jit_code_bytes);
    md_native_v2_runtime_init(&n3->nv2);
    n3->initialized = 1u;
}

void md_native3_reset(MdNative3 *n3)
{
    uint8_t *code; size_t bytes; uint8_t profile;
    if (!n3) return;
    code=n3->jit_code; bytes=n3->jit_code_bytes; profile=n3->profile;
    memset(n3,0,sizeof(*n3)); n3->jit_code=code; n3->jit_code_bytes=bytes; n3->profile=profile;
    md_jit_init(&n3->jit,code,bytes); md_native_v2_runtime_init(&n3->nv2); n3->initialized=1u;
}

bool MD_EXEC_HOT_FUNC(md_native3_run)(MdNative3 *n3, MdRuntime *rt, uint64_t budget, MdN3RunResult *result)
{
    uint64_t total=0, native=0, interp=0;
    if (result) memset(result,0,sizeof(*result));
    if (!n3 || !rt || !n3->initialized || !budget || rt->stop_reason!=MD_STOP_NONE) return false;

    if (n3->seen_code_epoch != rt->code_epoch) {
        md_jit_reset(&n3->jit); md_native_v2_runtime_init(&n3->nv2);
        memset(n3->site,0,sizeof(n3->site)); memset(n3->shadow,0,sizeof(n3->shadow));
        n3->shadow_top=n3->shadow_count=0; n3->seen_code_epoch=rt->code_epoch;
    }
    N3STAT_INC(n3, entries);

    while (total < budget && rt->stop_reason == MD_STOP_NONE) {
        uint64_t left=budget-total;
#if defined(__arm__) || defined(__thumb__)
        {
            MdNativeV2RunResult nv; uint64_t before=rt->instructions;
            if (md_native_v2_runtime_try_execute(&n3->nv2,rt,left,&nv)) {
                uint64_t r=rt->instructions-before; total+=r; native+=r;
                N3STAT_INC(n3, nv2_entries); N3STAT_ADD(n3, nv2_retired, r);
                if (r) continue;
            }
        }
#endif
        {
            MdN3Site *s=n3_site(n3,rt,rt->cpu.cs,rt->cpu.ip); N3Probe p;
            if (!n3_probe(rt,rt->cpu.cs,rt->cpu.ip,&p)) {
                uint64_t r=n3_interp(n3,rt,left); total+=r; interp+=r; if(!r)break; continue;
            }
            if (p.call && p.direct) n3_shadow_push(n3,rt->cpu.cs,p.next_ip,rt->cpu.cs,p.target);
            else if (p.ret) (void)n3_shadow_pop(n3,rt->cpu.cs,rt->cpu.ip);

            if (s->state != MD_N3_SITE_NATIVE) {
                if (!n3_prepare_jit(n3,rt,s)) {
                    s->state=s->penalty>=3u?MD_N3_SITE_FALLBACK:MD_N3_SITE_SEEN;
                    { uint64_t r=n3_interp(n3,rt,left); total+=r; interp+=r; if(!r)break; continue; }
                }
                n3_prefetch(n3,rt,&p);
            }
            {
                MdJitRunResult jr; uint64_t before=rt->instructions;
                (void)md_jit_run_region(&n3->jit,rt,left,&jr);
                if (jr.retired) {
                    total+=jr.retired; native+=jr.native; interp+=jr.fallback;
                    N3STAT_INC(n3, jit_entries); N3STAT_ADD(n3, jit_retired, jr.native);
                    if (jr.invalidated) { s->state=MD_N3_SITE_SEEN; N3STAT_INC(n3, smc_rejects); }
                    continue;
                }
                if (rt->instructions==before) {
                    uint64_t r=n3_interp(n3,rt,left); total+=r; interp+=r; if(!r)break;
                }
            }
        }
    }
    N3STAT_ADD(n3, retired, total); N3STAT_ADD(n3, native_retired, native);
    if (result) {
        result->retired=total; result->native=native; result->interpreted=interp;
        result->entered=total!=0; result->invalidated=0; result->stop=(uint8_t)rt->stop_reason;
#if defined(__aarch64__)
        result->backend=MD_N3_BACKEND_A64_JIT;
#elif defined(__arm__) || defined(__thumb__)
        result->backend=MD_N3_BACKEND_THUMB_NV2;
#else
        result->backend=MD_N3_BACKEND_PORTABLE;
#endif
    }
    return total!=0;
}

bool md_native3_prepare(MdNative3 *n3, MdRuntime *rt, uint16_t cs, uint16_t ip)
{
    uint16_t sc,si; MdN3Site *s; int ok;
    if(!n3||!rt||!n3->initialized)return false;
    sc=rt->cpu.cs;si=rt->cpu.ip;rt->cpu.cs=cs;rt->cpu.ip=ip;
    s=n3_site(n3,rt,cs,ip);ok=s->state==MD_N3_SITE_NATIVE||n3_prepare_jit(n3,rt,s);
    rt->cpu.cs=sc;rt->cpu.ip=si;return ok!=0;
}

unsigned md_native3_prewarm(MdNative3 *n3, MdRuntime *rt, const MdN3Prewarm *entries, unsigned count)
{
    unsigned i,ok=0;if(!n3||!rt||!entries)return 0;if(count>MD_N3_PREWARM_MAX)count=MD_N3_PREWARM_MAX;
    for(i=0;i<count;i++){N3STAT_INC(n3, prewarm_requests);if(md_native3_prepare(n3,rt,entries[i].cs,entries[i].ip)){++ok;N3STAT_INC(n3, prewarm_success);}}
    return ok;
}

static uint32_t n3_cache_sum(const uint8_t *p,size_t n){return n3_fnv1a(p,n);}

size_t md_native3_cache_export(const MdNative3 *n3, void *dst, size_t cap)
{
    MdN3CacheHeader *h;MdN3CacheRecord *r;unsigned i,c=0;size_t need;
    if (!n3) return 0;
    for (i = 0; i < MD_N3_SITE_SLOTS; ++i)
        if (n3->site[i].state == MD_N3_SITE_NATIVE) ++c;
    need=sizeof(MdN3CacheHeader)+(size_t)c*sizeof(MdN3CacheRecord);if(!dst)return need;if(cap<need)return 0;
    memset(dst,0,need);h=(MdN3CacheHeader*)dst;r=(MdN3CacheRecord*)(h+1);
    h->magic=MD_N3_CACHE_MAGIC;h->version=MD_N3_CACHE_VERSION;h->record_size=sizeof(*r);h->record_count=c;h->guest_address_bits=MD_X86_ADDRESS_BITS;h->feature_bits=1u;
    c=0;for(i=0;i<MD_N3_SITE_SLOTS;i++){const MdN3Site*s=&n3->site[i];if(s->state!=MD_N3_SITE_NATIVE)continue;r[c].cs=s->cs;r[c].ip=s->ip;r[c].signature=s->signature;r[c].gen0=s->gen0;r[c].gen1=s->gen1;r[c].page0=s->page0;r[c].page1=s->page1;r[c].page_count=s->page_count;r[c].backend_hint=s->backend;r[c].heat=s->heat;++c;}
    h->checksum=n3_cache_sum((const uint8_t*)r,(size_t)h->record_count*sizeof(*r));return need;
}

bool md_native3_cache_import(MdNative3 *n3, MdRuntime *rt, const void *src, size_t bytes)
{
    const MdN3CacheHeader*h=(const MdN3CacheHeader*)src;const MdN3CacheRecord*r;unsigned i;
    if(!n3||!rt||!src||bytes<sizeof(*h)||h->magic!=MD_N3_CACHE_MAGIC||h->version!=MD_N3_CACHE_VERSION||h->record_size!=sizeof(MdN3CacheRecord)||h->guest_address_bits!=MD_X86_ADDRESS_BITS)return false;
    if (sizeof(*h) + (size_t)h->record_count * sizeof(MdN3CacheRecord) > bytes)
        return false;
    r = (const MdN3CacheRecord *)(h + 1);
    if(n3_cache_sum((const uint8_t*)r,(size_t)h->record_count*sizeof(*r))!=h->checksum)return false;
    for(i=0;i<h->record_count;i++){if(n3_signature(rt,r[i].cs,r[i].ip)!=r[i].signature)continue;if(md_native3_prepare(n3,rt,r[i].cs,r[i].ip))N3STAT_INC(n3, cache_imported);}
    return true;
}

const MdN3Stats *md_native3_stats(const MdNative3 *n3){return n3?&n3->stats:NULL;}
const char *md_native3_backend_name(unsigned b){switch(b){case MD_N3_BACKEND_THUMB_NV2:return"thumb-nv2";case MD_N3_BACKEND_THUMB_JIT:return"thumb-jit";case MD_N3_BACKEND_A64_JIT:return"a64-jit";case MD_N3_BACKEND_PORTABLE:return"portable";default:return"none";}}
