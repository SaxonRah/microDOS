#ifndef MICRODOS_EXEC_ROUTER_H
#define MICRODOS_EXEC_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#ifndef MD_EXEC_SITE_SLOTS
#define MD_EXEC_SITE_SLOTS 64u
#endif
#ifndef MD_EXEC_INTERP_QUANTUM
#define MD_EXEC_INTERP_QUANTUM 256u
#endif
#ifndef MD_EXEC_PROFILE
#define MD_EXEC_PROFILE 1
#endif
/* Staged rollout: benchmark the interpreter-first baseline before promotion. */
#ifndef MD_EXEC_ENABLE_PROMOTION
#define MD_EXEC_ENABLE_PROMOTION 0
#endif
#ifndef MD_EXEC_ENABLE_DIRECT
#define MD_EXEC_ENABLE_DIRECT 0
#endif

#define MD_EXEC_HOT_THRESHOLD 4u
#define MD_EXEC_MIN_DIRECT_OPS 8u
#define MD_EXEC_MIN_COVERAGE_PCT 80u
#define MD_EXEC_GOOD_NATIVE_RUN 64u
#define MD_EXEC_DEMOTE_PENALTY 3u
#define MD_EXEC_UNSTABLE_WRITES 4u

typedef enum MdExecMode {
    MD_EXEC_INTERP = 0,
    MD_EXEC_JIT_CANDIDATE,
    MD_EXEC_JIT_REGION,
    MD_EXEC_INTERP_COOLDOWN,
    MD_EXEC_UNSTABLE
} MdExecMode;

typedef struct MdExecSite {
    uint16_t cs, ip;
    uint8_t heat, penalty, mode, cooldown;
} MdExecSite;

typedef enum MdExecTier {
    MD_EXEC_TIER_NONE = 0,
    MD_EXEC_TIER_INTERP,
    MD_EXEC_TIER_AOT,
    MD_EXEC_TIER_JIT
} MdExecTier;

typedef struct MdExecRouter {
    MdExecSite site[MD_EXEC_SITE_SLOTS];
#if MD_EXEC_PROFILE
    uint64_t interp_instructions, aot_instructions, jit_instructions;
    uint32_t promotions, rejections, demotions, unstable_demotions;
    uint32_t tier_entries, tier_switches, jit_entries;
    uint8_t last_tier;
#endif
} MdExecRouter;

#ifdef __cplusplus
extern "C" {
#endif

struct MdJitProbe;
struct MdJitRunResult;

void md_exec_router_init(MdExecRouter *router);
MdExecSite *md_exec_router_lookup(MdExecRouter *router, uint16_t cs, uint16_t ip);
/* One observation per completed quantum (or CS boundary), never per opcode. */
void md_exec_router_sample(MdExecSite *site);
bool md_exec_router_should_probe(const MdExecSite *site);
bool md_exec_router_accept_probe(const struct MdJitProbe *probe, bool allow_direct);
void md_exec_router_promote(MdExecRouter *router, MdExecSite *site);
void md_exec_router_reject(MdExecRouter *router, MdExecSite *site);
void md_exec_router_jit_feedback(MdExecRouter *router, MdExecSite *site,
                                 const struct MdJitRunResult *result);
#if MD_EXEC_PROFILE
void md_exec_router_record(MdExecRouter *router, MdExecTier tier, uint64_t retired);
#else
static inline void md_exec_router_record(MdExecRouter *router, MdExecTier tier, uint64_t retired)
{
    (void)router; (void)tier; (void)retired;
}
#endif
const char *md_exec_mode_name(unsigned mode);

#ifdef __cplusplus
}
#endif

#endif
