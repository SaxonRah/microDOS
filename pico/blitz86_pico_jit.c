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
/* Fast lookup table + SMC line map: touched by translated code on every
   indirect jump and store, so they must be in SRAM (not the PSRAM heap). */
static uint8_t __attribute__((aligned(64))) hotbuf[32u*1024u];
static B86Cpu cpu;

/* Exact byte arrays copied from microDOS Pico Native-3 benchmark. */
static const uint8_t wl_loop[]={0xB9,0,0x80,0x49,0x75,0xFD,0xF4};
static const uint8_t wl_regmix[]={0xB8,1,0,0xBB,3,0,0xB9,0,0x80,0x31,0xD2,0x03,0xC3,0x33,0xD8,0x03,0xD0,0x49,0x75,0xF7,0xF4};
static const uint8_t wl_callmix[]={0xB9,0,0x80,0xBB,0,0,0xE8,4,0,0x49,0x75,0xFA,0xF4,0x83,0xC3,3,0xC3};
/* blitz86 kernels (all data kept away from the code at 0x100):
   memrmw  : mov cx,8000h; mov bx,4000h; xor si,si; L: add [bx+si+4],ax; add si,2; and si,3FFEh; dec cx; jnz L
   stack   : mov cx,8000h; L: push ax; push bx; pop ax; pop bx; loop L
   lodsto  : mov cx,4000h; mov si,4000h; mov di,8000h; cld; L: lodsb; stosb; loop L
   repmov  : mov dx,200h; cld; O: mov cx,800h; mov si,4000h; mov di,8000h; rep movsw; dec dx; jnz O
   shiftadc: mov cx,8000h; L: shl ax,1; adc dx,0; shr bx,1; loop L
   mul     : mov cx,8000h; mov bx,7; L: mov ax,cx; mul bx; add si,ax; loop L                */
static const uint8_t wl_memrmw[]={0xB9,0,0x80,0xBB,0,0x40,0x31,0xF6,0x01,0x40,0x04,0x83,0xC6,0x02,0x81,0xE6,0xFE,0x3F,0x49,0x75,0xF3,0xF4};
static const uint8_t wl_stack[]={0xB9,0,0x80,0x50,0x53,0x58,0x5B,0xE2,0xFA,0xF4};
static const uint8_t wl_lodsto[]={0xB9,0,0x40,0xBE,0,0x40,0xBF,0,0x80,0xFC,0xAC,0xAA,0xE2,0xFC,0xF4};
static const uint8_t wl_repmov[]={0xBA,0,0x02,0xFC,0xB9,0,0x08,0xBE,0,0x40,0xBF,0,0x80,0xF3,0xA5,0x4A,0x75,0xF2,0xF4};
static const uint8_t wl_shiftadc[]={0xB9,0,0x80,0xD1,0xE0,0x83,0xD2,0x00,0xD1,0xEB,0xE2,0xF7,0xF4};
static const uint8_t wl_mul[]={0xB9,0,0x80,0xBB,7,0,0x89,0xC8,0xF7,0xE3,0x01,0xC6,0xE2,0xF8,0xF4};
struct Work { const char *name; const uint8_t *bytes; unsigned size; };
static const struct Work workloads[]={
 {"loop",wl_loop,sizeof wl_loop},{"regmix",wl_regmix,sizeof wl_regmix},
 {"callmix",wl_callmix,sizeof wl_callmix},{"memrmw",wl_memrmw,sizeof wl_memrmw},
 {"stack",wl_stack,sizeof wl_stack},{"lodsto",wl_lodsto,sizeof wl_lodsto},
 {"repmov",wl_repmov,sizeof wl_repmov},{"shiftadc",wl_shiftadc,sizeof wl_shiftadc},
 {"mul",wl_mul,sizeof wl_mul}
};
struct Snapshot {uint16_t reg[8],seg[4],ip,flags;uint32_t memhash;};
static uint32_t memhash(void) {uint32_t h=2166136261u;for(unsigned i=0;i<B86_MEM_BYTES;i++){h^=guest[i];h*=16777619u;}return h;}
static uint32_t normhash(void) {uint32_t h=2166136261u;for(unsigned i=0;i<65536u;i++){h^=guest[i];h*=16777619u;}return h;}
static void snapshot(struct Snapshot *s) {for(unsigned i=0;i<8;i++)s->reg[i]=(uint16_t)cpu.r[i];for(unsigned i=0;i<4;i++)s->seg[i]=(uint16_t)cpu.seg[i];s->ip=(uint16_t)cpu.ip;s->flags=b86_get_flags(&cpu);s->memhash=memhash();}
static uint32_t state32(const struct Snapshot *s){
 uint32_t h=2166136261u;uint16_t v[14];unsigned i;
 for(i=0;i<8;i++)v[i]=s->reg[i];
 for(i=0;i<4;i++)v[i+8]=s->seg[i];
 v[12]=s->ip;v[13]=(uint16_t)(s->flags & 0x08D7u);
 for(i=0;i<14;i++){h^=(uint8_t)v[i];h*=16777619u;h^=(uint8_t)(v[i]>>8);h*=16777619u;}
 return h;
}
static int equals(const struct Snapshot *a,const struct Snapshot *b){return memcmp(a,b,sizeof *a)==0;}
static void reset_cpu(const struct Work *w){
 memset(guest,0,sizeof guest);memcpy(guest+0x100,w->bytes,w->size);b86_init(&cpu,guest);
 for(unsigned i=0;i<4;i++)b86_set_seg(&cpu,(int)i,0);
 cpu.ip=0x100;cpu.r[B86_SP]=0xFFFEu;
}
static void emit(const char *work,const char *phase,int ok,uint64_t us,uint64_t retired,const struct Snapshot *got,const B86JitStats *st) {
 unsigned long long mips100=us?(unsigned long long)(retired*100ull/us):0;
 printf("[bench] arch=pico workload=%s engine=blitz86-jit phase=%s result=%s us=%llu retired_ref=%llu mips=%llu.%02llu hash=%08lx norm64=%08lx state32=%08lx blocks=%llu chains=%llu dispatches=%llu helpers=%llu\n",work,phase,ok?"PASS":"FAIL",(unsigned long long)us,(unsigned long long)retired,mips100/100,mips100%100,(unsigned long)got->memhash,(unsigned long)normhash(),(unsigned long)state32(got),(unsigned long long)st->blocks,(unsigned long long)st->chains,(unsigned long long)st->dispatches,(unsigned long long)st->helper_insns);
 stdio_flush();
}
static uint64_t now_us(void){return time_us_64();}

static void sort7(uint64_t *x){for(unsigned i=1;i<7;i++){uint64_t v=x[i];unsigned j=i;while(j&&x[j-1]>v){x[j]=x[j-1];j--;}x[j]=v;}}
static int run(void){
 if(!psram_is_available()||!psram_check_address(&guest[0])||!psram_check_address(&guest[B86_MEM_BYTES-1]))return 0;
 uint64_t samples[7];
 for(unsigned w=0;w<sizeof workloads/sizeof workloads[0];w++){
  const struct Work *p=&workloads[w];struct Snapshot oracle,got;
  reset_cpu(p);uint64_t t=now_us();int ref=b86_run_interp(&cpu,20000000);uint64_t ref_us=now_us()-t;
  uint64_t retired=cpu.icount;snapshot(&oracle);
  if(ref!=B86_HALT)return 0;
  printf("[bench] arch=pico workload=%s engine=blitz86-interp phase=cold result=PASS us=%llu retired=%llu\n",p->name,(unsigned long long)ref_us,(unsigned long long)retired);stdio_flush();
  reset_cpu(p);b86_hw_alloc_reset();if(b86_jit_hot_bytes()>sizeof hotbuf)return 0;struct B86Jit *j=b86_jit_create_ex(&cpu,codebuf,sizeof codebuf,hotbuf,sizeof hotbuf);
  if(!j)return 0;
  b86_jit_set_max_block(j,48);  /* chaining + fast table ON (seed-51187 was a PUSH-SMC bug, fixed) */
  /* Preserve the post-creation CPU: it contains codemap/fast/JIT pointers.
     Reinitialize guest RAM directly, but do NOT b86_init() a live JIT CPU. */
  B86Cpu initial=cpu;
  for(unsigned pass=0;pass<11;pass++){
   const char *phase=pass==0?"cold":(pass<4?"warmup":"sample");
   memset(guest,0,sizeof guest);memcpy(guest+0x100,p->bytes,p->size);
   cpu=initial;
   uint64_t started=now_us();int rc=b86_jit_run(&cpu,500000);uint64_t elapsed=now_us()-started;
   snapshot(&got);
   int ok=rc==B86_HALT&&equals(&oracle,&got);
   const B86JitStats *st=b86_jit_stats(j);
   if(pass==0)emit(p->name,"cold",ok,elapsed,retired,&got,st);
   if(pass>=4){samples[pass-4]=elapsed;}
   if(!ok){
    printf("[bench] ERROR workload=%s phase=%s rc=%d\n",p->name,phase,rc);return 0;
   }
  }
  sort7(samples);
  emit(p->name,"warm-median-7",1,samples[3],retired,&got,b86_jit_stats(j));
 }
 return 1;
}

int main(void){
 stdio_init_all();set_sys_clock_khz(300000,false);
 if(psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,PICO_DEFAULT_PSRAM_MAX_SELECT,PICO_DEFAULT_PSRAM_MIN_DESELECT)!=0||psram_reinitialize()!=0){
  while(!stdio_usb_connected())sleep_ms(20);
  printf("[b86-jit] COMPLETE result=FAIL PSRAM initialization\n");for(;;)sleep_ms(1000);
 }
 while(!stdio_usb_connected())sleep_ms(20);
 sleep_ms(300);
 printf("[b86-jit] BEGIN warm median comparison (cold + 7 measured)\n");
 int ok=run();printf("[b86-jit] COMPLETE result=%s\n",ok?"PASS":"FAIL");
 stdio_flush();for(;;)sleep_ms(1000);
}
