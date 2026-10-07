#include "microdos/native3.h"
#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 300000
#endif
#define GUEST_BYTES (1u<<20)
#define CODE_BYTES (128u*1024u)
static uint8_t __uninitialized_psram("native3_guest") guest[GUEST_BYTES];
static uint8_t code[CODE_BYTES] __attribute__((aligned(64)));
static MdRuntime rt; static MdNative3 n3;
static const uint8_t loop_code[]={0xB9,0x00,0x80,0x49,0x75,0xFD,0xF4};
static const uint8_t regmix[]={0xB8,1,0,0xBB,3,0,0xB9,0,0x80,0x31,0xD2,0x03,0xC3,0x33,0xD8,0x03,0xD0,0x49,0x75,0xF7,0xF4};
static const uint8_t callmix[]={0xB9,0,0x80,0xBB,0,0,0xE8,4,0,0x49,0x75,0xFA,0xF4,0x83,0xC3,3,0xC3};
static void f3(uint64_t x){printf("%llu.%03llu",(unsigned long long)(x/1000u),(unsigned long long)(x%1000u));}
static int hw(void){if(!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ,false))return 0;if(psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,PICO_DEFAULT_PSRAM_MAX_SELECT,PICO_DEFAULT_PSRAM_MIN_DESELECT)!=0)return 0;return psram_reinitialize()==0;}
static uint64_t one(const char*n,const uint8_t*p,size_t z,unsigned runs){MdHooks h;uint64_t t0,us,ret=0;unsigned i;memset(&h,0,sizeof(h));memset(guest,0,GUEST_BYTES);memcpy(guest+0x100,p,z);md_runtime_init(&rt,guest,&h);md_native3_init(&n3,code,sizeof(code));for(i=0;i<4;i++){MdN3RunResult rr;rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[MD_X86_SP]=0xFFFE;rt.stop_reason=MD_STOP_NONE;(void)md_native3_run(&n3,&rt,2000000,&rr);}t0=time_us_64();for(i=0;i<runs;i++){MdN3RunResult rr;memset(rt.cpu.r,0,sizeof(rt.cpu.r));rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[MD_X86_SP]=0xFFFE;rt.stop_reason=MD_STOP_NONE;while(rt.stop_reason==MD_STOP_NONE){if(!md_native3_run(&n3,&rt,2000000,&rr))break;ret+=rr.retired;}}us=time_us_64()-t0;{uint64_t m=us?ret*1000u/us:0;uint64_t c=m?(uint64_t)MICRODOS_PICO_SYS_KHZ*1000u/m:0;printf("[n3] %-8s ",n);f3(m);printf(" MIPS ");f3(c);printf(" cyc/guest GATE40=%s native=%llu interp=%llu shadow=%llu/%llu/%llu\n",m>=40000u?"PASS":"FAIL",(unsigned long long)n3.stats.native_retired,(unsigned long long)n3.stats.interp_retired,(unsigned long long)n3.stats.shadow_pushes,(unsigned long long)n3.stats.shadow_hits,(unsigned long long)n3.stats.shadow_misses);stdio_flush();return m;}}
int main(void){stdio_init_all();sleep_ms(1200);printf("=== Native-3 RP2350 40-MIPS gate ===\n");printf("[n3] hw=%s sys=%u kHz\n",hw()?"ok":"FAIL",MICRODOS_PICO_SYS_KHZ);(void)one("loop",loop_code,sizeof(loop_code),128);(void)one("regmix",regmix,sizeof(regmix),64);(void)one("callmix",callmix,sizeof(callmix),32);while(1)tight_loop_contents();}
