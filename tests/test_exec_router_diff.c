/* Canonical vs the actual DOS system router, compared after every slice. */
#include "md_dos2_system.h"
#include "microdos/jit.h"
#include "microdos/ops.h"
#include "loop_recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",name,__LINE__,#x); ++failures; return; } } while (0)
static uint8_t a[MD_X86_ADDRESS_SPACE], b[MD_X86_ADDRESS_SPACE];
static MdDos2System sys;
static MdRuntime ref;
static MdJit jit;
static uint8_t *code;

static const uint8_t loop[]={0x49,0x75,0xfd,0xf4};
static const uint8_t checksum[]={0xad,0x01,0xc2,0xe2,0xfb,0xf4};
static const uint8_t callret[]={0xb9,0x20,0x00,0xe8,0x04,0x00,0x49,0x75,0xfa,0xf4,0x43,0xc3};
static const uint8_t intiret[]={0xcd,0x80,0x43,0xf4};
static const uint8_t unsupported[]={0xb9,0x00,0x20,0x9c,0x9f,0x9e,0x9d,0x49,0x75,0xf9,0xf4};
static const uint8_t selfmod[]={0xc6,0x06,0x09,0x01,0x02,0xb9,0x20,0x00,0xb0,0x01,0x49,0x75,0xfd,0xf4};
static const uint8_t memloop[]={0x8a,0x04,0x04,0x03,0x88,0x04,0x83,0xc6,0x61,0x81,0xce,0x00,0x80,0x49,0x75,0xf0,0xf4};
static const uint8_t branchmix[]={0x83,0xf0,0x01,0x83,0xf8,0x00,0x74,0x03,0x83,0xc3,0x03,0x83,0xcb,0x00,0x49,0x75,0xef,0xf4};
static const uint8_t direct[]={0xb8,1,0,0xbb,2,0,0xba,3,0,0xbe,4,0,0xbf,5,0,0xbd,6,0,0x90,0x90,0x90,0xf4};
static const uint8_t aotloop[]={0xb9,0xff,0xff,0x49,0x75,0xfd,0xf4};

static void init(const uint8_t *bytes,size_t size)
{
    unsigned i;
    memset(a,0,sizeof(a)); memset(b,0,sizeof(b));
    md_runtime_init(&ref,a,NULL);
    md_dos2_system_init(&sys,b,NULL);
    memset(&sys.runtime.hooks,0,sizeof(sys.runtime.hooks));
    sys.boot.dos_segment=0x5000u;
    md_jit_init(&jit,code,65536u); md_dos2_system_set_jit(&sys,&jit);
    ref.cpu.cs=ref.cpu.ds=ref.cpu.es=ref.cpu.ss=0x1000u;
    ref.cpu.ip=0x100u; ref.cpu.r[MD_X86_SP]=0xfffeu;
    ref.cpu.r[MD_X86_CX]=8192u; ref.cpu.r[MD_X86_SI]=0x8000u;
    memcpy(&sys.runtime.cpu,&ref.cpu,offsetof(MdX86,memory));
    memcpy(a+0x10100u,bytes,size);
    for(i=0;i<16384u;++i) a[0x18000u+i]=(uint8_t)(i*7u);
    /* Raw IVT handler for INT/IRET, independent of BIOS hooks. */
    a[0x200u]=0x00u; a[0x201u]=0x01u; a[0x202u]=0x00u; a[0x203u]=0x20u;
    a[0x20100u]=0x40u; a[0x20101u]=0xcfu;
    memcpy(b,a,sizeof(a));
}

static void equal(const char *name)
{
    CHECK(memcmp(ref.cpu.r,sys.runtime.cpu.r,sizeof(ref.cpu.r))==0);
    CHECK(ref.cpu.cs==sys.runtime.cpu.cs && ref.cpu.ip==sys.runtime.cpu.ip);
    CHECK(ref.cpu.ds==sys.runtime.cpu.ds && ref.cpu.es==sys.runtime.cpu.es && ref.cpu.ss==sys.runtime.cpu.ss);
    CHECK(md_x86_flags(&ref.cpu)==md_x86_flags(&sys.runtime.cpu));
    CHECK(ref.instructions==sys.runtime.instructions);
    CHECK(ref.stop_reason==sys.runtime.stop_reason);
    CHECK(memcmp(a,b,sizeof(a))==0);
}

static void run(const char *name,const uint8_t *bytes,size_t size,unsigned budget,int prepare,int use_aot)
{
    unsigned slices=0u;
    uint64_t before;
    MdJitProbe probe;
    init(bytes,size);
    if (strcmp(name,"branchzero")==0 || strcmp(name,"memzero")==0)
        ref.cpu.r[MD_X86_CX]=sys.runtime.cpu.r[MD_X86_CX]=0u;
    if (budget <= 64u) ref.cpu.r[MD_X86_CX]=sys.runtime.cpu.r[MD_X86_CX]=64u;
    if (bytes==unsupported && budget <= 64u) {
        a[0x10101u]=b[0x10101u]=64u; a[0x10102u]=b[0x10102u]=0u;
    }
    if (bytes==memloop && strcmp(name,"memguard")!=0) ref.cpu.ds=sys.runtime.cpu.ds=0u;
    if (use_aot) {
        static const MdAotProgram *programs[]={&md_recomp_loop_program};
        md_dos2_system_set_aot(&sys,programs,1u,true);
        CHECK(md_recomp_loop_program.attach(&sys.runtime,sys.runtime.cpu.cs));
        ref.cpu.ip=sys.runtime.cpu.ip=0x103u;
    }
    if(prepare) {
        uint64_t epoch=sys.runtime.code_epoch;
        CHECK(md_jit_probe(&jit,&sys.runtime,sys.runtime.cpu.cs,sys.runtime.cpu.ip,&probe));
        CHECK(jit.code_used==0u && sys.runtime.code_epoch==epoch && sys.runtime.instructions==0u);
        CHECK(memcmp(a,b,sizeof(a))==0);
        CHECK(md_exec_router_accept_probe(&probe,bytes==direct));
        CHECK(md_jit_prepare_region(&jit,&sys.runtime));
        md_exec_router_promote(&sys.router,md_exec_router_lookup(&sys.router,sys.runtime.cpu.cs,sys.runtime.cpu.ip));
    }
    while(ref.stop_reason==MD_STOP_NONE && slices++<200000u) {
        before=sys.runtime.instructions;
        (void)md_interp_run(&ref,budget);
        if(ref.stop_reason==MD_STOP_BUDGET) ref.stop_reason=MD_STOP_NONE;
        (void)md_dos2_system_run(&sys,budget);
        CHECK(sys.runtime.instructions-before<=budget);
        equal(name); if(failures) return;
    }
    CHECK(ref.stop_reason==MD_STOP_HALT);
#if MD_EXEC_PROFILE
    if (budget==4096u && bytes==loop && !prepare) {
#if MD_EXEC_ENABLE_PROMOTION
        CHECK(sys.router.promotions>0u && sys.router.jit_instructions>0u);
#else
        CHECK(sys.router.promotions==0u && jit.code_used==0u);
#endif
    }
#if MD_EXEC_ENABLE_PROMOTION
    if(bytes==unsupported && budget>=4096u) CHECK(sys.router.rejections>0u && sys.router.promotions==0u);
#endif
    if(use_aot) CHECK(sys.aot_enters>0u && sys.router.jit_instructions==0u);
#endif
}

static void invalidation(void)
{
    const char *name="invalidation";
    MdJitRunResult result;
    unsigned i;
    init(loop,sizeof(loop));
    CHECK(md_jit_run_region(&jit,&sys.runtime,0u,&result)==MD_STOP_NONE);
    CHECK(result.retired==0u && sys.runtime.instructions==0u);
    for(i=0;i<4u;++i) {
        MdExecSite *site=md_exec_router_lookup(&sys.router,sys.runtime.cpu.cs,sys.runtime.cpu.ip);
        CHECK(md_jit_prepare_region(&jit,&sys.runtime));
        md_exec_router_promote(&sys.router,site);
        md_x86_write8(&sys.runtime.cpu,sys.runtime.cpu.cs,sys.runtime.cpu.ip,0x49u);
        CHECK(md_jit_run_region(&jit,&sys.runtime,64u,&result)==MD_STOP_NONE);
        CHECK(result.invalidated && result.retired==0u);
        md_exec_router_jit_feedback(&sys.router,site,&result);
    }
    CHECK(md_exec_router_lookup(&sys.router,0x1000u,0x100u)->mode==MD_EXEC_UNSTABLE);
}

static void probes(void)
{
    const char *name="probes";
    const uint8_t farcall[]={0x9a,0x00,0x01,0x00,0x20};
    const uint8_t farret[]={0xcb};
    MdJitProbe probe;
    init(farcall,sizeof(farcall));
    CHECK(md_jit_probe(&jit,&sys.runtime,0x1000u,0x100u,&probe));
    CHECK(probe.has_call && !md_exec_router_accept_probe(&probe,true));
    init(farret,sizeof(farret));
    CHECK(md_jit_probe(&jit,&sys.runtime,0x1000u,0x100u,&probe));
    CHECK(probe.has_return && !md_exec_router_accept_probe(&probe,true));
    init(unsupported,sizeof(unsupported));
    CHECK(md_jit_probe(&jit,&sys.runtime,0x1000u,0x103u,&probe));
    CHECK(probe.direct_ops==0u && !md_exec_router_accept_probe(&probe,true));
}

int main(void)
{
    static const unsigned budgets[]={1u,2u,3u,7u,16u,31u,64u,4096u,1000000u};
    unsigned i;
#if defined(__linux__)
    code=mmap(NULL,65536u,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(code==MAP_FAILED) code=NULL;
#endif
    if(!code) code=calloc(1u,65536u);
    if(!code) return 1;
    for(i=0;i<sizeof(budgets)/sizeof(budgets[0]);++i) {
#define RUN(x,p,aot) run(#x,x,sizeof(x),budgets[i],p,aot)
        RUN(loop,0,0); RUN(loop,1,0); RUN(checksum,1,0);
        RUN(memloop,1,0); RUN(branchmix,1,0); RUN(direct,1,0);
        run("memguard",memloop,sizeof(memloop),budgets[i],1,0);
        RUN(callret,0,0); RUN(intiret,0,0); RUN(unsupported,0,0); RUN(selfmod,0,0);
        RUN(aotloop,0,1);
#undef RUN
    }
    run("branchzero",branchmix,sizeof(branchmix),1000000u,1,0);
    run("memzero",memloop,sizeof(memloop),1000000u,1,0);
    invalidation();
    probes();
    printf("exec_router_diff: %s\n",failures?"FAIL":"ok");
    return failures?1:0;
}
