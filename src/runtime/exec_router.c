#include "microdos/exec_router.h"
#include "microdos/jit.h"
#include <string.h>

_Static_assert(MD_EXEC_SITE_SLOTS > 0u &&
               (MD_EXEC_SITE_SLOTS & (MD_EXEC_SITE_SLOTS - 1u)) == 0u,
               "router slots must be a power of two");
_Static_assert(MD_EXEC_INTERP_QUANTUM > 0u, "interpreter quantum must be positive");
_Static_assert(sizeof(MdExecSite) == 8u, "router sites must stay compact");
_Static_assert(sizeof(MdExecRouter) <= 1024u, "router exceeds SRAM budget");

#if MD_EXEC_PROFILE
#define MD_EXEC_STAT(expr) do { expr; } while (0)
#else
#define MD_EXEC_STAT(expr) do { (void)router; } while (0)
#endif

static uint8_t sat_add(uint8_t value, unsigned delta)
{
    return (uint8_t)(value + delta > 255u ? 255u : value + delta);
}

void md_exec_router_init(MdExecRouter *router)
{
    memset(router, 0, sizeof(*router));
}

MdExecSite *md_exec_router_lookup(MdExecRouter *router, uint16_t cs, uint16_t ip)
{
    MdExecSite *site = &router->site[((uint32_t)cs * 33u + ip) & (MD_EXEC_SITE_SLOTS - 1u)];
    if (site->cs != cs || site->ip != ip) {
        memset(site, 0, sizeof(*site));
        site->cs = cs;
        site->ip = ip;
    }
    return site;
}

void md_exec_router_sample(MdExecSite *site)
{
    if (site->cooldown != 0u) {
        if (--site->cooldown == 0u) site->mode = MD_EXEC_INTERP;
        return;
    }
    site->heat = sat_add(site->heat, 1u);
    if (site->mode == MD_EXEC_INTERP && site->heat >= MD_EXEC_HOT_THRESHOLD)
        site->mode = MD_EXEC_JIT_CANDIDATE;
}

bool md_exec_router_should_probe(const MdExecSite *site)
{
    return site->mode == MD_EXEC_JIT_CANDIDATE && site->cooldown == 0u;
}

bool md_exec_router_accept_probe(const MdJitProbe *probe, bool allow_direct)
{
    if (probe->unstable || probe->decoded_ops == 0u || probe->has_call || probe->has_return)
        return false;
    if (probe->resident_kind != MD_JIT_RESIDENT_NONE) return true;
    return allow_direct && probe->direct_ops >= MD_EXEC_MIN_DIRECT_OPS &&
           (unsigned)probe->direct_ops * 100u >=
               (unsigned)probe->decoded_ops * MD_EXEC_MIN_COVERAGE_PCT;
}

static void cooldown(MdExecSite *site)
{
    site->cooldown = site->penalty <= 1u ? 16u : site->penalty == 2u ? 64u : 255u;
    site->mode = MD_EXEC_INTERP_COOLDOWN;
    site->heat = 0u;
}

void md_exec_router_promote(MdExecRouter *router, MdExecSite *site)
{
    site->mode = MD_EXEC_JIT_REGION;
    site->cooldown = 0u;
    MD_EXEC_STAT(++router->promotions);
}

void md_exec_router_reject(MdExecRouter *router, MdExecSite *site)
{
    site->penalty = sat_add(site->penalty, 1u);
    cooldown(site);
    MD_EXEC_STAT(++router->rejections);
}

void md_exec_router_jit_feedback(MdExecRouter *router, MdExecSite *site,
                                 const MdJitRunResult *result)
{
    if (result->invalidated) {
        /* Penalty accumulates across re-admission; a full quiet cooldown is
           required before retry. Generation checks in the JIT guard safety. */
        site->penalty = sat_add(site->penalty, 1u);
        cooldown(site);
        if (site->penalty >= MD_EXEC_UNSTABLE_WRITES) {
            site->mode = MD_EXEC_UNSTABLE;
            MD_EXEC_STAT(++router->unstable_demotions);
        }
        MD_EXEC_STAT(++router->demotions);
        return;
    }
    /* Budget refusals are not evidence of poor native coverage. */
    if (result->budget_limited) return;
    if (result->zero_exits || result->native == 0u)
        site->penalty = sat_add(site->penalty, 2u);
    else if (result->native < 16u || result->fallback > result->native / 4u)
        site->penalty = sat_add(site->penalty, 1u);
    else if (result->native >= MD_EXEC_GOOD_NATIVE_RUN && site->penalty != 0u)
        --site->penalty;
    if (site->penalty >= MD_EXEC_DEMOTE_PENALTY) {
        cooldown(site);
        MD_EXEC_STAT(++router->demotions);
    }
}

#if MD_EXEC_PROFILE
void md_exec_router_record(MdExecRouter *router, MdExecTier tier, uint64_t retired)
{
    if (retired == 0u && tier != MD_EXEC_TIER_JIT) return;
    ++router->tier_entries;
    if (router->last_tier != MD_EXEC_TIER_NONE && router->last_tier != tier)
        ++router->tier_switches;
    router->last_tier = (uint8_t)tier;
    if (tier == MD_EXEC_TIER_AOT) router->aot_instructions += retired;
    else if (tier == MD_EXEC_TIER_JIT) {
        router->jit_instructions += retired;
        ++router->jit_entries;
    } else router->interp_instructions += retired;
}

#endif

const char *md_exec_mode_name(unsigned mode)
{
    switch (mode) {
        case MD_EXEC_JIT_CANDIDATE: return "candidate";
        case MD_EXEC_JIT_REGION: return "jit";
        case MD_EXEC_INTERP_COOLDOWN: return "cooldown";
        case MD_EXEC_UNSTABLE: return "unstable";
        default: return "interp";
    }
}
