#include "microdos/native3.h"
#include <stddef.h>
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
#define GUEST_BYTES (64u*1024u)
#define CODE_BYTES (128u*1024u)
static uint8_t guest[GUEST_BYTES] __attribute__((aligned(64)));static uint8_t code[CODE_BYTES] __attribute__((aligned(64)));static MdRuntime rt;static MdNative3 n3;
static inline void wr(uintptr_t a,uint32_t v){*(volatile uint32_t*)a=v;}static inline uint32_t rd(uintptr_t a){return *(volatile uint32_t*)a;}static void d(unsigned n){while(n--)__asm__ volatile("nop");}static void ui(void){uint32_t r;wr(AUX_ENABLES,rd(AUX_ENABLES)|1u);wr(CNTL,0);wr(IER,0);wr(LCR,3);wr(MCR,0);wr(IIR,0xC6);wr(BAUD,270);r=rd(GPFSEL1);r&=~((7u<<12)|(7u<<15));r|=(2u<<12)|(2u<<15);wr(GPFSEL1,r);wr(GPPUD,0);d(150);wr(GPPUDCLK0,(1u<<14)|(1u<<15));d(150);wr(GPPUDCLK0,0);wr(CNTL,3);}static void pc(char c){if(c=='\n')pc('\r');while(!(rd(LSR)&0x20)){}wr(IO,(uint32_t)c);}static void ps(const char*s){while(*s)pc(*s++);}static void pu(uint64_t v){char b[24];unsigned n=0;if(!v){pc('0');return;}while(v){b[n++]=(char)('0'+v%10);v/=10;}while(n)pc(b[--n]);}static void f3(uint64_t v){pu(v/1000);pc('.');pc('0'+(v/100)%10);pc('0'+(v/10)%10);pc('0'+v%10);}static uint64_t tk(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0,CNTPCT_EL0":"=r"(v)::"memory");return v;}static uint64_t hz(void){uint64_t v;__asm__ volatile("mrs %0,CNTFRQ_EL0":"=r"(v));return v;}
static const uint8_t lp[]={0xB9,0,0x80,0x49,0x75,0xFD,0xF4};static const uint8_t rm[]={0xB8,1,0,0xBB,3,0,0xB9,0,0x80,0x31,0xD2,0x03,0xC3,0x33,0xD8,0x03,0xD0,0x49,0x75,0xF7,0xF4};static const uint8_t cm[]={0xB9,0,0x80,0xBB,0,0,0xE8,4,0,0x49,0x75,0xFA,0xF4,0x83,0xC3,3,0xC3};
static void one(const char*n,const uint8_t*p,size_t z,unsigned runs){MdHooks h;uint64_t a,b,ret=0;unsigned i;memset(&h,0,sizeof(h));memset(guest,0,sizeof(guest));memcpy(guest+0x100,p,z);md_runtime_init(&rt,guest,&h);md_native3_init(&n3,code,sizeof(code));for(i=0;i<4;i++){MdN3RunResult rr;rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[4]=0xFFFE;rt.stop_reason=MD_STOP_NONE;(void)md_native3_run(&n3,&rt,2000000,&rr);}a=tk();for(i=0;i<runs;i++){MdN3RunResult rr;memset(rt.cpu.r,0,sizeof(rt.cpu.r));rt.cpu.cs=0;rt.cpu.ip=0x100;rt.cpu.ss=0;rt.cpu.r[4]=0xFFFE;rt.stop_reason=MD_STOP_NONE;while(rt.stop_reason==MD_STOP_NONE){if(!md_native3_run(&n3,&rt,2000000,&rr))break;ret+=rr.retired;}}b=tk();{uint64_t us=(b-a)*1000000u/hz(),m=us?ret*1000u/us:0;ps("[n3] ");ps(n);ps(" ");f3(m);ps(" MIPS GATE40=");ps(m>=40000u?"PASS\n":"FAIL\n");}}
void kernel_main(void){ui();ps("=== Native-3 Pi Zero 2 W 40-MIPS gate ===\n");one("loop",lp,sizeof(lp),128);one("regmix",rm,sizeof(rm),64);one("callmix",cm,sizeof(cm),32);for(;;)__asm__ volatile("wfe");}
