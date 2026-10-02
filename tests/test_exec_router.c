#include "microdos/exec_router.h"
#include "microdos/jit.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); ++failures; } } while (0)

int main(void)
{
    MdExecRouter router;
    MdExecSite *site;
    MdJitProbe probe;
    MdJitRunResult result;
    unsigned i;
    md_exec_router_init(&router);
    CHECK(sizeof(router) <= 1024u);
    CHECK(sizeof(MdExecSite) == 8u);
    site = md_exec_router_lookup(&router, 0u, 0u);
    CHECK(site->mode == MD_EXEC_INTERP);
    for (i=0; i<MD_EXEC_HOT_THRESHOLD-1u; ++i) md_exec_router_sample(site);
    CHECK(!md_exec_router_should_probe(site));
    md_exec_router_sample(site);
    CHECK(md_exec_router_should_probe(site));
    for (i=0; i<300u; ++i) md_exec_router_sample(site);
    CHECK(site->heat == 255u);
    memset(&probe,0,sizeof(probe));
    probe.decoded_ops = 10u; probe.direct_ops = 8u;
    CHECK(!md_exec_router_accept_probe(&probe,false));
    CHECK(md_exec_router_accept_probe(&probe,true));
    probe.direct_ops = 7u; CHECK(!md_exec_router_accept_probe(&probe,true));
    probe.resident_kind = MD_JIT_RESIDENT_DEC_JNZ;
    CHECK(md_exec_router_accept_probe(&probe,false));
    probe.has_call = 1u; CHECK(!md_exec_router_accept_probe(&probe,true));
    probe.has_call = 0u; probe.has_return = 1u;
    CHECK(!md_exec_router_accept_probe(&probe,true));
    probe.has_return = 0u; probe.unstable = 1u;
    CHECK(!md_exec_router_accept_probe(&probe,true));
    md_exec_router_reject(&router,site); CHECK(site->cooldown == 16u);
    for(i=0;i<16u;++i) { CHECK(!md_exec_router_should_probe(site)); md_exec_router_sample(site); }
    CHECK(site->mode == MD_EXEC_INTERP);
    md_exec_router_reject(&router,site); CHECK(site->cooldown == 64u);
    md_exec_router_reject(&router,site); CHECK(site->cooldown == 255u);
    md_exec_router_promote(&router,site);
    memset(&result,0,sizeof(result)); result.native=256u;
    md_exec_router_jit_feedback(&router,site,&result);
    CHECK(site->penalty == 2u);
    CHECK(site->mode == MD_EXEC_JIT_REGION);
    result.native=0u; result.budget_limited=1u;
    md_exec_router_jit_feedback(&router,site,&result);
    CHECK(site->penalty == 2u);
    result.budget_limited=0u; result.zero_exits=1u;
    md_exec_router_jit_feedback(&router,site,&result);
    CHECK(site->mode == MD_EXEC_INTERP_COOLDOWN);
    CHECK(site->penalty == 4u);
    site->penalty=0u;
    memset(&result,0,sizeof(result)); result.invalidated=1u;
    for(i=0;i<MD_EXEC_UNSTABLE_WRITES;++i) {
        md_exec_router_promote(&router,site);
        md_exec_router_jit_feedback(&router,site,&result);
    }
    CHECK(site->mode == MD_EXEC_UNSTABLE && site->cooldown == 255u);
    for(i=0;i<255u;++i) md_exec_router_sample(site);
    CHECK(site->mode == MD_EXEC_INTERP);
    /* Same direct-mapped index: collisions lose only advisory policy. */
    CHECK(md_exec_router_lookup(&router,0u,MD_EXEC_SITE_SLOTS) == site);
    CHECK(site->heat == 0u && site->penalty == 0u && site->mode == MD_EXEC_INTERP);
    site->penalty=255u; md_exec_router_reject(&router,site); CHECK(site->penalty==255u);
    md_exec_router_record(&router,MD_EXEC_TIER_INTERP,256u);
    md_exec_router_record(&router,MD_EXEC_TIER_JIT,512u);
#if MD_EXEC_PROFILE
    CHECK(router.tier_entries==2u && router.tier_switches==1u);
    CHECK(router.interp_instructions==256u && router.jit_instructions==512u);
#endif
    md_exec_router_init(&router);
    CHECK(router.site[0].penalty==0u && router.site[0].ip==0u);
    printf("exec_router: %s (%zu bytes)\n",failures?"FAIL":"ok",sizeof(router));
    return failures?1:0;
}
