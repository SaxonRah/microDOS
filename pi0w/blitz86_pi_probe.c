/* blitz86 Pi Zero 2 W bare-metal integration probe.
   Existing microDOS start.S, linker.ld, mini_libc.c, and v29 HID UART transport.
   This is a reference CPU + AArch64 backend LINK test. Native JIT execution
   is deliberately NOT enabled until a managed executable arena/allocator and
   on-device watchdog have been validated. */
#include "b86.h"
#include <stdint.h>
#include <stddef.h>
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
static B86Cpu cpu;
static inline void wr(uintptr_t a,uint32_t v){*(volatile uint32_t*)a=v;}
static inline uint32_t rd(uintptr_t a){return *(volatile uint32_t*)a;}
static void delay(unsigned n){while(n--)__asm__ volatile("nop");}
static void uart_init(void){
 uint32_t r;
 wr(AUX_ENABLES,rd(AUX_ENABLES)|1u);wr(CNTL,0);wr(IER,0);
 wr(LCR,3);wr(MCR,0);wr(IIR,0xC6);wr(BAUD,270);
 r=rd(GPFSEL1);r&=~((7u<<12)|(7u<<15));r|=(2u<<12)|(2u<<15);
 wr(GPFSEL1,r);wr(GPPUD,0);delay(150);
 wr(GPPUDCLK0,(1u<<14)|(1u<<15));delay(150);
 wr(GPPUDCLK0,0);wr(CNTL,3);
}
static void ch(char c){if(c=='\n')ch('\r');while(!(rd(LSR)&0x20)){}wr(IO,(uint32_t)c);}
static void str(const char*s){while(*s)ch(*s++);}
static void num(uint64_t v){char b[32];unsigned n=0;if(!v){ch('0');return;}while(v){b[n++]=(char)('0'+v%10);v/=10;}while(n)ch(b[--n]);}
static uint64_t ticks(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0,CNTPCT_EL0":"=r"(v)::"memory");return v;}
static uint64_t freq(void){uint64_t v;__asm__ volatile("mrs %0,CNTFRQ_EL0":"=r"(v));return v;}
static const uint8_t program[]={0xB8,1,0,0xB9,0,0x20,0x83,0xC0,3,0x49,0x75,0xFA,0xF4};
void kernel_main(void){
 uint64_t t0,t1,us;int r;uint32_t ax,cx;
 uart_init();str("[b86-pi] BEGIN B1/B2 probe (AArch64)\n");
 memset(guest,0,sizeof(guest));memcpy(guest+0x10100,program,sizeof(program));
 b86_init(&cpu,guest);b86_set_seg(&cpu,B86_CS,0x1000);
 b86_set_seg(&cpu,B86_DS,0x1000);b86_set_seg(&cpu,B86_ES,0x1000);
 b86_set_seg(&cpu,B86_SS,0x1000);cpu.ip=0x100;
 t0=ticks();r=b86_run_interp(&cpu,100000);t1=ticks();
 us=freq()?(t1-t0)*1000000u/freq():0;
 ax=cpu.r[B86_AX]&65535u;cx=cpu.r[B86_CX]&65535u;
 str("[b86-pi] interp result=");num((unsigned)r);
 str(" AX=");num(ax);str(" CX=");num(cx);
 str(" icount=");num(cpu.icount);str(" us=");num(us);ch('\n');
 if(r==B86_HALT&&ax==0x6001u&&cx==0){str("[b86-pi] COMPLETE result=PASS\n");}
 else{str("[b86-pi] COMPLETE result=FAIL\n");}
 for(;;)__asm__ volatile("wfe");
}
