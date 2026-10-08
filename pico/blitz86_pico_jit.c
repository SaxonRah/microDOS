/* Native Thumb-2 JIT entry on Pico Plus 2; separate firmware from interp probe. */
#include "b86.h"
#include "b86_hw_alloc.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/clocks.h"
#include "hardware/psram.h"
#include <stdio.h>
#include <string.h>
static uint8_t __uninitialized_psram("b86_jit_guest") __attribute__((aligned(64))) guest[B86_MEM_BYTES];
/* Executable JIT code must live in internal SRAM, not external PSRAM or XIP flash. */
static uint8_t __attribute__((aligned(64))) codebuf[96u*1024u];
static B86Cpu cpu;
static const uint8_t program[]={0xB8,1,0,0xB9,0,0x20,0x83,0xC0,3,0x49,0x75,0xFA,0xF4};
static void reset_cpu(void){memset(guest,0,sizeof guest);memcpy(guest+0x10100,program,sizeof program);b86_init(&cpu,guest);b86_set_seg(&cpu,B86_CS,0x1000);b86_set_seg(&cpu,B86_DS,0x1000);b86_set_seg(&cpu,B86_SS,0x1000);b86_set_seg(&cpu,B86_ES,0x1000);cpu.ip=0x100;}
static int run(void){
 if(!psram_is_available()||!psram_check_address(&guest[0])||!psram_check_address(&guest[B86_MEM_BYTES-1])){
  printf("[b86-jit] PSRAM FAIL\n"); return 0;
 }
 reset_cpu();
 uint64_t t=time_us_64(); int ref=b86_run_interp(&cpu,100000); uint64_t dt=time_us_64()-t;
 printf("[b86-jit] interp result=%d AX=%04lX CX=%04lX us=%llu\n",ref,(unsigned long)(cpu.r[B86_AX]&65535u),(unsigned long)(cpu.r[B86_CX]&65535u),(unsigned long long)dt);
 if(ref!=B86_HALT||(cpu.r[B86_AX]&65535u)!=0x6001u||(cpu.r[B86_CX]&65535u)!=0)return 0;
 static const struct { const char *name; unsigned fast, chain; } modes[]={
  {"baseline",0,0},{"fast-only",1,0},{"chain-only",0,1},{"full",1,1}
 };
 for(unsigned k=0;k<sizeof modes/sizeof modes[0];++k){
  const char *name=modes[k].name;
  reset_cpu(); b86_hw_alloc_reset();
  struct B86Jit *j=b86_jit_create(&cpu,codebuf,sizeof codebuf);
  if(!j){printf("[b86-jit] mode=%s ALLOC FAIL\n",name);return 0;}
  b86_jit_set_max_block(j,48);
  b86_jit_set_no_fast(j,!modes[k].fast);
  uint32_t calls=0; int rc=B86_BUDGET;
  t=time_us_64();
  while(calls<30000u){
   /* chain disabled: one C dispatch per call, so no patch_site survives.
      chain enabled: dispatcher may patch direct successor branches. */
   rc=b86_jit_run(&cpu,modes[k].chain?30000u:1u);
   calls++;
   if(rc==B86_HALT||rc==B86_EXIT||rc!=B86_BUDGET)break;
  }
  dt=time_us_64()-t;
  const B86JitStats *st=b86_jit_stats(j);
  int ok=rc==B86_HALT&&(cpu.r[B86_AX]&65535u)==0x6001u&&(cpu.r[B86_CX]&65535u)==0;
  printf("[b86-jit] mode=%s result=%s rc=%d AX=%04lX CX=%04lX calls=%lu us=%llu blocks=%llu chains=%llu lookups=%llu dispatches=%llu\n",name,ok?"PASS":"FAIL",rc,(unsigned long)(cpu.r[B86_AX]&65535u),(unsigned long)(cpu.r[B86_CX]&65535u),(unsigned long)calls,(unsigned long long)dt,(unsigned long long)st->blocks,(unsigned long long)st->chains,(unsigned long long)st->lookups,(unsigned long long)st->dispatches);
  if(!ok)return 0;
 }
 return 1;
}
int main(void){stdio_init_all();set_sys_clock_khz(300000,false);if(psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,PICO_DEFAULT_PSRAM_MAX_SELECT,PICO_DEFAULT_PSRAM_MIN_DESELECT)!=0||psram_reinitialize()!=0){while(!stdio_usb_connected())sleep_ms(20);printf("[b86-jit] COMPLETE result=FAIL PSRAM initialization\n");for(;;)sleep_ms(1000);}
 while(!stdio_usb_connected())sleep_ms(20);sleep_ms(300);printf("[b86-jit] BEGIN Thumb-2 control-flow matrix\n");int ok=run();printf("[b86-jit] COMPLETE result=%s\n",ok?"PASS":"FAIL");stdio_flush();for(;;)sleep_ms(1000);}
