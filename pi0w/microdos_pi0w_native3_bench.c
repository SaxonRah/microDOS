/* Pi Zero 2 W Native-3 matched cold/warm parity benchmark. */
#include "md_n3_compare_shared.h"
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
#define CMP_GUEST_BYTES (64u*1024u)
#define CMP_CODE_BYTES (128u*1024u)
static uint8_t guest[CMP_GUEST_BYTES] __attribute__((aligned(64)));
static uint8_t code[CMP_CODE_BYTES] __attribute__((aligned(64)));
static MdRuntime rt;static MdNative3 n3;static MdHooks hooks;
static inline void wr(uintptr_t a,uint32_t v){*(volatile uint32_t*)a=v;}
static inline uint32_t rd(uintptr_t a){return *(volatile uint32_t*)a;}
static void delay(unsigned n){while(n--)__asm__ volatile("nop");}
static void uart_init(void){uint32_t r;wr(AUX_ENABLES,rd(AUX_ENABLES)|1u);wr(CNTL,0);wr(IER,0);wr(LCR,3);wr(MCR,0);wr(IIR,0xC6);wr(BAUD,270);r=rd(GPFSEL1);r&=~((7u<<12)|(7u<<15));r|=(2u<<12)|(2u<<15);wr(GPFSEL1,r);wr(GPPUD,0);delay(150);wr(GPPUDCLK0,(1u<<14)|(1u<<15));delay(150);wr(GPPUDCLK0,0);wr(CNTL,3);}
static void ch(char c){if(c=='\n')ch('\r');while(!(rd(LSR)&0x20u)){}wr(IO,(uint32_t)c);}
static void str(const char*s){while(*s)ch(*s++);}
static void num(uint64_t x){char b[24];unsigned i=0;if(!x){ch('0');return;}while(x){b[i++]=(char)('0'+x%10u);x/=10u;}while(i)ch(b[--i]);}
static void hexdigit(unsigned n){n&=15u;ch((char)(n<10u?'0'+n:'a'+n-10u));}
static void hex32(uint32_t x){int i;for(i=28;i>=0;i-=4)hexdigit(x>>(unsigned)i);}
static uint64_t tick(void){uint64_t x;__asm__ volatile("isb\n\tmrs %0,CNTPCT_EL0":"=r"(x)::"memory");return x;}
static uint64_t hz(void){uint64_t x;__asm__ volatile("mrs %0,CNTFRQ_EL0":"=r"(x));return x;}
static uint64_t us(uint64_t dt){return hz()?dt*1000000u/hz():0u;}
static void reset(const CmpWork *w){memset(guest,0,sizeof guest);memcpy(guest+0x100,w->program,w->size);memset(rt.cpu.r,0,sizeof rt.cpu.r);rt.cpu.cs=0;rt.cpu.ds=0;rt.cpu.es=0;rt.cpu.ss=0;rt.cpu.ip=0x100;md_x86_set_flags(&rt.cpu, MD_X86_FLAG_ALWAYS1);rt.cpu.r[MD_X86_SP]=0xFFFE;rt.stop_reason=MD_STOP_NONE;}
static int episode(uint64_t *ret){unsigned n;*ret=0;for(n=0;n<32&&rt.stop_reason==MD_STOP_NONE;n++){MdN3RunResult rr;if(!md_native3_run(&n3,&rt,2000000u,&rr))return 0;*ret+=rr.retired;}return rt.stop_reason==MD_STOP_HALT;}
static void prefix(const char *work,const char *phase){str("[compare] arch=pi engine=microdos-native3 workload=");str(work);str(" phase=");str(phase);}
static uint32_t normhash(void) {uint32_t h=2166136261u;for(unsigned i=0;i<65536u;i++){h^=guest[i];h*=16777619u;}return h;}
static uint32_t state32(const CmpSnapshot *s){
 uint32_t h=2166136261u;uint16_t v[14];unsigned i;
 for(i=0;i<8;i++)v[i]=s->r[i];
 v[8]=s->es;v[9]=s->cs;v[10]=s->ss;v[11]=s->ds;
 v[12]=s->ip;v[13]=(uint16_t)(s->flags & 0x08D7u);
 for(i=0;i<14;i++){h^=(uint8_t)v[i];h*=16777619u;h^=(uint8_t)(v[i]>>8);h*=16777619u;}
 return h;
}
static int one(const CmpWork *w){CmpSnapshot ref,s;uint64_t t,cold,warm[7],ret;unsigned i;int ok;
 memset(&hooks,0,sizeof hooks);memset(guest,0,sizeof guest);memcpy(guest+0x100,w->program,w->size);md_runtime_init(&rt,guest,&hooks);md_native3_init(&n3,code,sizeof code);reset(w);
 t=tick();ok=episode(&ret);cold=us(tick()-t);cmp_snapshot(&ref,&rt,guest,sizeof guest);ok=ok&&ret==w->retired;
 prefix(w->name,"cold");str(" result=");str(ok?"PASS":"FAIL");str(" us=");num(cold);str(" retired=");num(ret);str(" native=");num(n3.stats.native_retired);str(" interp=");num(n3.stats.interp_retired);str(" hash=");hex32(ref.memhash);str(" norm64=");hex32(normhash());str(" state32=");num(state32(&ref));ch('\n');
 if(!ok)return 0;
 for(i=0;i<10;i++){uint64_t dt;reset(w);t=tick();ok=episode(&ret);dt=us(tick()-t);cmp_snapshot(&s,&rt,guest,sizeof guest);
  if(!ok||ret!=w->retired||!cmp_equal(&ref,&s)){prefix(w->name,i<3?"warmup":"warm");str(" result=FAIL index=");num(i);str(" retired=");num(ret);str(" expected=");num(w->retired);str(" hash=");hex32(s.memhash);str(" expected_hash=");hex32(ref.memhash);str(" norm64=");hex32(normhash());str(" state32=");num(state32(&ref));ch('\n');return 0;}if(i>=3)warm[i-3]=dt;
 }
 prefix(w->name,"warm-median-7");str(" result=PASS us=");num(cmp_median7(warm));str(" retired=");num(w->retired);str(" native_total=");num(n3.stats.native_retired);str(" interp_total=");num(n3.stats.interp_retired);str(" nv2_total=");num(n3.stats.nv2_retired);str(" lookups=");num(n3.stats.lookups);str(" hits=");num(n3.stats.hits);str(" compiles=");num(n3.stats.compiles);str(" invalidations=");num(n3.stats.invalidations);str(" hash=");hex32(ref.memhash);str(" norm64=");hex32(normhash());str(" state32=");num(state32(&ref));ch('\n');return 1;}

/* Independent scaling probe: a batched count with raw hardware counter ticks.
   Does NOT equate equivalent guest work with literal ARM instruction count. */
static int audit_loop_scaling(void){
 static const unsigned counts[]={32u,512u,8192u,32768u};
 unsigned k,rep;
 str("[audit] arch=pi engine=microdos-native3 test=loop-scale timer_hz=");num(hz());ch('\n');
 for(k=0;k<sizeof counts/sizeof counts[0];++k){
  const unsigned n=counts[k];
  const uint64_t per=(uint64_t)n*2u+2u;
  const unsigned repetitions=128u;
  uint64_t total=0,ret=0,begin,end;
  CmpWork w={"scale",cmp_loop,sizeof cmp_loop,(uint32_t)per};
  memset(guest,0,sizeof guest);
  memcpy(guest+0x100,w.program,w.size);
  guest[0x101]=(uint8_t)n;
  guest[0x102]=(uint8_t)(n>>8);
  memset(&hooks,0,sizeof hooks);
  md_runtime_init(&rt,guest,&hooks);
  md_native3_init(&n3,code,sizeof code);
  rt.cpu.cs=0;rt.cpu.ds=0;rt.cpu.es=0;rt.cpu.ss=0;
  rt.cpu.ip=0x100;rt.cpu.r[MD_X86_SP]=0xFFFE;
  md_x86_set_flags(&rt.cpu,MD_X86_FLAG_ALWAYS1);
  rt.stop_reason=MD_STOP_NONE;
  if(!episode(&ret)||ret!=per||rt.cpu.r[MD_X86_CX]!=0){str("[audit] loop-scale FAIL warmup\n");return 0;}
  begin=tick();
  for(rep=0;rep<repetitions;++rep){
   rt.cpu.ip=0x100;rt.cpu.r[MD_X86_CX]=0;
   rt.cpu.r[MD_X86_SP]=0xFFFE;
   md_x86_set_flags(&rt.cpu,MD_X86_FLAG_ALWAYS1);
   rt.stop_reason=MD_STOP_NONE;
   if(!episode(&ret)||ret!=per||rt.cpu.r[MD_X86_CX]!=0||rt.cpu.ip!=0x107){
    str("[audit] loop-scale FAIL run=");num(rep);ch('\n');return 0;
   }
   total+=ret;
  }
  end=tick();
  str("[audit] arch=pi engine=microdos-native3 test=loop-scale count=");num(n);
  str(" reps=");num(repetitions);
  str(" retired_expected=");num(per*repetitions);
  str(" retired_observed=");num(total);
  str(" ticks=");num(end-begin);
  str(" timer_hz=");num(hz());
  str(" us=");num(us(end-begin));
  str(" result=");str(total==per*repetitions?"PASS":"FAIL");ch('\n');
  if(total!=per*repetitions)return 0;
 }
 return 1;
}
void kernel_main(void){unsigned i;int ok=1;uart_init();str("[n3-compare] BEGIN Pi Native-3 matched parity\n");for(i=0;i<sizeof cmp_work/sizeof cmp_work[0];i++)if(!one(&cmp_work[i])){ok=0;break;}if(ok&&!audit_loop_scaling())ok=0;str("[n3-compare] COMPLETE result=");str(ok?"PASS\n":"FAIL\n");for(;;)__asm__ volatile("wfe");}
