#include "microdos/runtime.h"
#include "loop_recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static const uint8_t kLoopCom[]={0xB9,0xFF,0xFF,0x49,0x75,0xFD,0xF4};
static double secs(clock_t a,clock_t b){return(double)(b-a)/(double)CLOCKS_PER_SEC;}
static int bench_i(unsigned long rounds,uint8_t*m){unsigned long i;uint64_t total=0;MdRuntime r;MdHooks h={0};clock_t a,b;double s;memset(m,0,MD_X86_ADDRESS_SPACE);md_runtime_init(&r,m,&h);a=clock();for(i=0;i<rounds;++i){md_runtime_load_com(&r,kLoopCom,sizeof(kLoopCom),0x1000u);if(md_interp_run(&r,200000u)!=MD_STOP_HALT)return 1;total+=r.instructions;}b=clock();s=secs(a,b);printf("interp rounds=%lu guest_instructions=%llu seconds=%.6f MIPS=%.2f\n",rounds,(unsigned long long)total,s,s>0?((double)total/s)/1e6:0);return 0;}
static int bench_a(unsigned long rounds,uint8_t*m){unsigned long i;uint64_t total=0;MdRuntime r;MdHooks h={0};clock_t a,b;double s;memset(m,0,MD_X86_ADDRESS_SPACE);md_runtime_init(&r,m,&h);a=clock();for(i=0;i<rounds;++i){if(md_recomp_loop(&r,0x1000u,200000u)!=MD_STOP_HALT)return 1;total+=r.instructions;}b=clock();s=secs(a,b);printf("aot    rounds=%lu guest_instructions=%llu seconds=%.6f MIPS=%.2f\n",rounds,(unsigned long long)total,s,s>0?((double)total/s)/1e6:0);return 0;}
int main(int argc,char**argv){unsigned long rounds=1000ul;uint8_t*m;int f=0;if(argc>1){rounds=strtoul(argv[1],NULL,10);if(!rounds)rounds=1;}m=(uint8_t*)malloc(MD_X86_ADDRESS_SPACE);if(!m)return 2;f|=bench_i(rounds,m);f|=bench_a(rounds,m);free(m);return f;}
