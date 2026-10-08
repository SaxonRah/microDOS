/* Independent AArch64 JIT smoke test; reuse microDOS bare-metal boot + HID. */
#include "b86.h"
#include "b86_hw_alloc.h"
#include <stdint.h>
#include <string.h>
#define PB 0x3F000000UL
#define GPIO (PB+0x00200000UL)
#define AUX (PB+0x00215000UL)
#define GPFSEL1 (GPIO+0x04UL)
#define GPPUD (GPIO+0x94UL)
#define GPPUDCLK0 (GPIO+0x98UL)
#define AUX_ENABLES (AUX+0x04UL)
#define IO (AUX+0x40UL)
#define IER (AUX+0x44UL)
#define IIR (AUX+0x48UL)
#define LCR (AUX+0x4CUL)
#define MCR (AUX+0x50UL)
#define LSR (AUX+0x54UL)
#define CNTL (AUX+0x60UL)
#define BAUD (AUX+0x68UL)
static uint8_t guest[B86_MEM_BYTES] __attribute__((aligned(64)));
static uint8_t codebuf[128u*1024u] __attribute__((aligned(64)));
static B86Cpu cpu;
static inline void wr(uintptr_t a,uint32_t v){*(volatile uint32_t*)a=v;}
static inline uint32_t rd(uintptr_t a){return *(volatile uint32_t*)a;}
static void delay(unsigned n){while(n--)__asm__ volatile("nop");}
static void ui(void){uint32_t r;wr(AUX_ENABLES,rd(AUX_ENABLES)|1u);wr(CNTL,0);wr(IER,0);wr(LCR,3);wr(MCR,0);wr(IIR,0xC6);wr(BAUD,270);r=rd(GPFSEL1);r&=~((7u<<12)|(7u<<15));r|=(2u<<12)|(2u<<15);wr(GPFSEL1,r);wr(GPPUD,0);delay(150);wr(GPPUDCLK0,(1u<<14)|(1u<<15));delay(150);wr(GPPUDCLK0,0);wr(CNTL,3);}
static void ch(char c){if(c=='\n')ch('\r');while(!(rd(LSR)&0x20)){}wr(IO,(uint32_t)c);}
static void str(const char *s){while(*s)ch(*s++);}
static void num(uint64_t v){char b[24];unsigned i=0;if(!v){ch('0');return;}while(v){b[i++]=(char)('0'+v%10);v/=10;}while(i)ch(b[--i]);}
static uint64_t ticks(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0,CNTPCT_EL0":"=r"(v)::"memory");return v;}
static uint64_t freq(void){uint64_t v;__asm__ volatile("mrs %0,CNTFRQ_EL0":"=r"(v));return v;}
static const uint8_t program[]={0xB8,1,0,0xB9,0,0x20,0x83,0xC0,3,0x49,0x75,0xFA,0xF4};
static void reset_cpu(void){memset(guest,0,sizeof guest);memcpy(guest+0x10100,program,sizeof program);b86_init(&cpu,guest);b86_set_seg(&cpu,B86_CS,0x1000);b86_set_seg(&cpu,B86_DS,0x1000);b86_set_seg(&cpu,B86_SS,0x1000);b86_set_seg(&cpu,B86_ES,0x1000);cpu.ip=0x100;}
void kernel_main(void){
 ui();str("[b86-pi-jit] BEGIN AArch64 control-flow matrix\n");reset_cpu();
 uint64_t t=ticks();int ref=b86_run_interp(&cpu,100000);uint64_t dt=ticks()-t;
 str("[b86-pi-jit] interp result=");num((unsigned)ref);str(" AX=");num(cpu.r[B86_AX]&65535u);str(" CX=");num(cpu.r[B86_CX]&65535u);str(" us=");num(freq()?dt*1000000u/freq():0);ch('\n');
 if(ref!=B86_HALT||(cpu.r[B86_AX]&65535u)!=0x6001u||cpu.r[B86_CX]!=0){str("[b86-pi-jit] COMPLETE result=FAIL\n");goto done;}
 static const struct { const char *name; unsigned fast,chain; } modes[]={
  {"baseline",0,0},{"fast-only",1,0},{"chain-only",0,1},{"full",1,1}
 };
 for(unsigned k=0;k<sizeof modes/sizeof modes[0];++k){
  const char *name=modes[k].name;
  reset_cpu();b86_hw_alloc_reset();
  struct B86Jit *j=b86_jit_create(&cpu,codebuf,sizeof codebuf);
  if(!j){str("[b86-pi-jit] ERROR allocation\n[b86-pi-jit] COMPLETE result=FAIL\n");goto done;}
  b86_jit_set_max_block(j,48);
  b86_jit_set_no_fast(j,!modes[k].fast);
  uint64_t calls=0;int rc=B86_BUDGET;t=ticks();
  while(calls<30000u){
   rc=b86_jit_run(&cpu,modes[k].chain?30000u:1u);
   calls++;
   if(rc!=B86_BUDGET)break;
  }
  dt=ticks()-t;
  const B86JitStats *st=b86_jit_stats(j);
  int ok=rc==B86_HALT&&(cpu.r[B86_AX]&65535u)==0x6001u&&(cpu.r[B86_CX]&65535u)==0;
  str("[b86-pi-jit] mode=");str(name);str(" result=");str(ok?"PASS":"FAIL");
  str(" rc=");num((unsigned)rc);str(" AX=");num(cpu.r[B86_AX]&65535u);
  str(" CX=");num(cpu.r[B86_CX]&65535u);str(" calls=");num(calls);
  str(" us=");num(freq()?dt*1000000u/freq():0);str(" blocks=");num(st->blocks);
  str(" chains=");num(st->chains);str(" lookups=");num(st->lookups);
  str(" dispatches=");num(st->dispatches);ch('\n');
  if(!ok){str("[b86-pi-jit] COMPLETE result=FAIL\n");goto done;}
 }
 str("[b86-pi-jit] COMPLETE result=PASS\n");
 done:for(;;)__asm__ volatile("wfe");
}
