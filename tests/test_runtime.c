#include "microdos/runtime.h"
#include "hello_recomp.h"
#include "hybrid_recomp.h"
#include "loop_recomp.h"
#include "host_dos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures=0;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);++failures;}}while(0)
static const uint8_t hello[]={0xB4,0x09,0xBA,0x0C,0x01,0xCD,0x21,0xB8,0x00,0x4C,0xCD,0x21,'H','e','l','l','o',' ','f','r','o','m',' ','m','i','c','r','o','D','O','S','!',13,10,'$'};
static const uint8_t loop[]={0xB9,0xFF,0xFF,0x49,0x75,0xFD,0xF4};
static void address_wrap(uint8_t*m){MdX86 c;memset(&c,0,sizeof(c));c.memory=m;memset(m,0,MD_X86_ADDRESS_SPACE);md_x86_write8(&c,0xFFFFu,0x0010u,0xA5u);CHECK(m[0]==0xA5u);CHECK(md_x86_linear(0xFFFFu,0x0010u)==0u);}
static void reg_alias(uint8_t*m){MdX86 c;memset(&c,0,sizeof(c));c.memory=m;c.r[MD_X86_AX]=0x1234u;CHECK(md_x86_get_reg8(&c,0)==0x34u);CHECK(md_x86_get_reg8(&c,4)==0x12u);md_x86_set_reg8(&c,4,0xAB);CHECK(c.r[MD_X86_AX]==0xAB34u);}
static MdStopReason hello_i(uint8_t*m,char*out,size_t cap,uint64_t*n){MdRuntime r;MdHostDos d;MdHooks h;memset(m,0,MD_X86_ADDRESS_SPACE);md_host_dos_init(&d,out,cap);h.interrupt=md_host_dos_interrupt;h.in8=NULL;h.out8=NULL;h.user=&d;md_runtime_init(&r,m,&h);md_runtime_load_com(&r,hello,sizeof(hello),0x1000u);{MdStopReason s=md_interp_run(&r,1000);*n=r.instructions;CHECK(r.exit_code==0);return s;}}
static MdStopReason hello_a(uint8_t*m,char*out,size_t cap,uint64_t*n){MdRuntime r;MdHostDos d;MdHooks h;memset(m,0,MD_X86_ADDRESS_SPACE);md_host_dos_init(&d,out,cap);h.interrupt=md_host_dos_interrupt;h.in8=NULL;h.out8=NULL;h.user=&d;md_runtime_init(&r,m,&h);{MdStopReason s=md_recomp_hello(&r,0x1000u,1000);*n=r.instructions;CHECK(r.exit_code==0);return s;}}
static void interp_vs_aot(uint8_t*m){char a[256],b[256];uint64_t ni=0,na=0;CHECK(hello_i(m,a,sizeof(a),&ni)==MD_STOP_EXIT);CHECK(hello_a(m,b,sizeof(b),&na)==MD_STOP_EXIT);CHECK(strcmp(a,"Hello from microDOS!\r\n")==0);CHECK(strcmp(a,b)==0);CHECK(ni==5u);CHECK(na==5u);}
static void loop_cfg(uint8_t*m){MdRuntime i,a;MdHooks h={0};uint64_t n;memset(m,0,MD_X86_ADDRESS_SPACE);md_runtime_init(&i,m,&h);md_runtime_load_com(&i,loop,sizeof(loop),0x1000u);CHECK(md_interp_run(&i,200000u)==MD_STOP_HALT);n=i.instructions;CHECK(i.cpu.r[MD_X86_CX]==0);CHECK(n==131072u);memset(m,0,MD_X86_ADDRESS_SPACE);md_runtime_init(&a,m,&h);CHECK(md_recomp_loop(&a,0x1000u,200000u)==MD_STOP_HALT);CHECK(a.cpu.r[MD_X86_CX]==0);CHECK(a.instructions==n);}
static void hybrid(uint8_t*m){MdRuntime r;MdHooks h={0};memset(m,0,MD_X86_ADDRESS_SPACE);md_runtime_init(&r,m,&h);CHECK(md_recomp_hybrid(&r,0x1000u,100u)==MD_STOP_HALT);CHECK(r.cpu.r[MD_X86_AX]==0x1234u);CHECK(r.cpu.r[MD_X86_BX]==0x1234u);CHECK(r.instructions==5u);}
static void budget(uint8_t*m){MdRuntime r;MdHooks h={0};memset(m,0,MD_X86_ADDRESS_SPACE);md_runtime_init(&r,m,&h);CHECK(md_recomp_loop(&r,0x1000u,3u)==MD_STOP_BUDGET);CHECK(r.instructions==3u);}
static void ivt(uint8_t*m){MdRuntime r;MdHooks h={0};memset(m,0,MD_X86_ADDRESS_SPACE);md_runtime_init(&r,m,&h);r.cpu.cs=0x1234;r.cpu.ip=0x5678;r.cpu.ss=0x2000;r.cpu.r[MD_X86_SP]=0x1000;r.cpu.flags=MD_X86_FLAG_ALWAYS1|MD_X86_FLAG_IF|MD_X86_FLAG_TF;md_x86_write16_linear(&r.cpu,0x30u*4u,0x1111);md_x86_write16_linear(&r.cpu,0x30u*4u+2u,0x2222);CHECK(!md_runtime_interrupt(&r,0x30));CHECK(r.cpu.cs==0x2222);CHECK(r.cpu.ip==0x1111);CHECK(r.cpu.r[MD_X86_SP]==0x0FFA);CHECK(md_x86_read16(&r.cpu,0x2000,0x0FFA)==0x5678);}
int main(void){uint8_t*m=(uint8_t*)malloc(MD_X86_ADDRESS_SPACE);if(!m)return 2;address_wrap(m);reg_alias(m);interp_vs_aot(m);loop_cfg(m);hybrid(m);budget(m);ivt(m);free(m);if(failures){fprintf(stderr,"%d test(s) failed\n",failures);return 1;}puts("microDOS runtime + dosrecomp tests passed");return 0;}
