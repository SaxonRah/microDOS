/* Native-3 parity bench. Shares byte-identical workloads with blitz86. */
#include "md_n3_compare_shared.h"
#include "hardware/clocks.h"
#include "hardware/psram.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include <stdio.h>
#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 300000
#endif
#define CMP_GUEST_BYTES (1u<<20)
#define CMP_CODE_BYTES (128u*1024u)
static uint8_t __uninitialized_psram("n3_cmp_guest") guest[CMP_GUEST_BYTES];
static uint8_t code[CMP_CODE_BYTES] __attribute__((aligned(64)));
static MdRuntime rt;
static MdNative3 n3;
static MdHooks hooks;
static void reset(const CmpWork *w){
 /* Restore guest data including stack, without clearing the translated code cache. */
 memset(guest,0,sizeof guest);
 memcpy(guest+0x100,w->program,w->size);
 memset(rt.cpu.r,0,sizeof rt.cpu.r);
 rt.cpu.cs=0;rt.cpu.ds=0;rt.cpu.es=0;rt.cpu.ss=0;rt.cpu.ip=0x100;
 md_x86_set_flags(&rt.cpu, MD_X86_FLAG_ALWAYS1);rt.cpu.r[MD_X86_SP]=0xFFFE;
 rt.stop_reason=MD_STOP_NONE;
}
static int episode(uint64_t *ret){
 unsigned n;
 *ret=0;
 for(n=0;n<32&&rt.stop_reason==MD_STOP_NONE;n++){
  MdN3RunResult rr;
  if(!md_native3_run(&n3,&rt,2000000u,&rr))return 0;
  *ret+=rr.retired;
 }
 return rt.stop_reason==MD_STOP_HALT;
}
static uint32_t normhash(void) {uint32_t h=2166136261u;for(unsigned i=0;i<65536u;i++){h^=guest[i];h*=16777619u;}return h;}
static uint32_t state32(const CmpSnapshot *s){
 uint32_t h=2166136261u;uint16_t v[14];unsigned i;
 for(i=0;i<8;i++)v[i]=s->r[i];
 v[8]=s->es;v[9]=s->cs;v[10]=s->ss;v[11]=s->ds;
 v[12]=s->ip;v[13]=(uint16_t)(s->flags & 0x08D7u);
 for(i=0;i<14;i++){h^=(uint8_t)v[i];h*=16777619u;h^=(uint8_t)(v[i]>>8);h*=16777619u;}
 return h;
}
static int one(const CmpWork *w){
 CmpSnapshot ref,s;uint64_t t,cold,warm[7],ret;unsigned i;int ok=1;
 memset(&hooks,0,sizeof hooks);
 memset(guest,0,sizeof guest);memcpy(guest+0x100,w->program,w->size);
 md_runtime_init(&rt,guest,&hooks);
 md_native3_init(&n3,code,sizeof code);
 reset(w);
 t=time_us_64();ok=episode(&ret);cold=time_us_64()-t;
 cmp_snapshot(&ref,&rt,guest,sizeof guest);
 ok=ok&&ret==w->retired;
 printf("[compare] arch=pico engine=microdos-native3 workload=%s phase=cold result=%s us=%llu retired=%llu native=%llu interp=%llu hash=%08lx norm64=%08lx state32=%08lx\n",w->name,ok?"PASS":"FAIL",(unsigned long long)cold,(unsigned long long)ret,(unsigned long long)n3.stats.native_retired,(unsigned long long)n3.stats.interp_retired,(unsigned long)ref.memhash,(unsigned long)normhash(),(unsigned long)state32(&ref));
 if(!ok)return 0;
 for(i=0;i<10;i++){
  reset(w);
  t=time_us_64();ok=episode(&ret);uint64_t dt=time_us_64()-t;
  cmp_snapshot(&s,&rt,guest,sizeof guest);
  if(!ok||ret!=w->retired||!cmp_equal(&ref,&s)){
   printf("[compare] arch=pico engine=microdos-native3 workload=%s phase=%s-%u result=FAIL retired=%llu expected=%lu hash=%08lx expected_hash=%08lx norm64=%08lx state32=%08lx\n",w->name,i<3?"warmup":"warm",i<3?i:i-3,(unsigned long long)ret,(unsigned long)w->retired,(unsigned long)s.memhash,(unsigned long)ref.memhash,(unsigned long)normhash(),(unsigned long)state32(&ref));
   return 0;
  }
  if(i>=3)warm[i-3]=dt;
 }
 printf("[compare] arch=pico engine=microdos-native3 workload=%s phase=warm-median-7 result=PASS us=%llu retired=%lu native_total=%llu interp_total=%llu nv2_total=%llu lookups=%llu hits=%llu compiles=%llu invalidations=%llu hash=%08lx norm64=%08lx state32=%08lx\n",w->name,(unsigned long long)cmp_median7(warm),(unsigned long)w->retired,(unsigned long long)n3.stats.native_retired,(unsigned long long)n3.stats.interp_retired,(unsigned long long)n3.stats.nv2_retired,(unsigned long long)n3.stats.lookups,(unsigned long long)n3.stats.hits,(unsigned long long)n3.stats.compiles,(unsigned long long)n3.stats.invalidations,(unsigned long)ref.memhash,(unsigned long)normhash(),(unsigned long)state32(&ref));
 stdio_flush();return 1;
}
int main(void){unsigned i;int ok=1;stdio_init_all();
 if(!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ,false)||psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,PICO_DEFAULT_PSRAM_MAX_SELECT,PICO_DEFAULT_PSRAM_MIN_DESELECT)!=0||psram_reinitialize()!=0){
  while(!stdio_usb_connected())sleep_ms(20);
  printf("[n3-compare] COMPLETE result=FAIL reason=hardware\n");for(;;)sleep_ms(1000);
 }
 while(!stdio_usb_connected())sleep_ms(20);
 sleep_ms(250);printf("[n3-compare] BEGIN Pico Native-3 matched parity\n");
 for(i=0;i<sizeof cmp_work/sizeof cmp_work[0];i++){if(!one(&cmp_work[i])){ok=0;break;}}
 printf("[n3-compare] COMPLETE result=%s\n",ok?"PASS":"FAIL");stdio_flush();for(;;)sleep_ms(1000);
}
