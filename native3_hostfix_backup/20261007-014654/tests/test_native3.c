#include "microdos/native3.h"
#include <stdio.h>
#include <string.h>

static uint8_t mem[1u<<20];
static uint8_t code[64u*1024u];

static int exact_run(void)
{
    static const uint8_t p[]={
        0xB8,0x01,0x00,0xBB,0x03,0x00,0xB9,0x90,0x01,
        0x03,0xC3,0x33,0xD8,0x49,0x75,0xF9,0xF4
    };
    MdRuntime a,b; MdNative3 n3; MdHooks h; MdN3RunResult rr;
    uint8_t mem2[1u<<20]; unsigned guard=0;
    memset(&h,0,sizeof(h)); memset(mem,0,sizeof(mem)); memset(mem2,0,sizeof(mem2));
    memcpy(mem+0x100,p,sizeof(p)); memcpy(mem2+0x100,p,sizeof(p));
    md_runtime_init(&a,mem,&h); md_runtime_init(&b,mem2,&h);
    a.cpu.cs=b.cpu.cs=0; a.cpu.ip=b.cpu.ip=0x100;
    md_native3_init(&n3,code,sizeof(code));
    while(a.stop_reason==MD_STOP_NONE && guard++<1000u) {
        if(!md_native3_run(&n3,&a,4096,&rr)) break;
    }
    guard=0; while(b.stop_reason==MD_STOP_NONE && guard++<100000u) (void)md_interp_step(&b);
    if(a.cpu.r[0]!=b.cpu.r[0]||a.cpu.r[1]!=b.cpu.r[1]||a.cpu.r[3]!=b.cpu.r[3]||a.cpu.ip!=b.cpu.ip||md_x86_flags(&a.cpu)!=md_x86_flags(&b.cpu)) {
        printf("native3 exact FAIL AX=%04x/%04x BX=%04x/%04x CX=%04x/%04x IP=%04x/%04x\n",
               a.cpu.r[0],b.cpu.r[0],a.cpu.r[3],b.cpu.r[3],a.cpu.r[1],b.cpu.r[1],a.cpu.ip,b.cpu.ip);
        return 0;
    }
    puts("native3 exact PASS"); return 1;
}

static int cache_roundtrip(void)
{
    MdRuntime rt; MdNative3 a,b; MdHooks h; MdN3Prewarm e; uint8_t blob[8192]; size_t n;
    memset(&h,0,sizeof(h)); memset(mem,0,sizeof(mem));
    mem[0x100]=0xB9;mem[0x101]=10;mem[0x102]=0;mem[0x103]=0x49;mem[0x104]=0x75;mem[0x105]=0xFD;mem[0x106]=0xF4;
    md_runtime_init(&rt,mem,&h);rt.cpu.cs=0;rt.cpu.ip=0x100;
    md_native3_init(&a,code,sizeof(code));e.cs=0;e.ip=0x100;(void)md_native3_prewarm(&a,&rt,&e,1);
    n=md_native3_cache_export(&a,blob,sizeof(blob)); if(!n){puts("native3 cache export FAIL");return 0;}
    md_native3_init(&b,code,sizeof(code));if(!md_native3_cache_import(&b,&rt,blob,n)){puts("native3 cache import FAIL");return 0;}
    puts("native3 cache PASS");return 1;
}

int main(void){int ok=exact_run()&cache_roundtrip();printf("native3 tests %s\n",ok?"PASS":"FAIL");return ok?0:1;}
