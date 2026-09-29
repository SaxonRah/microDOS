#include "microdos/runtime.h"
#include "hello_recomp.h"
#include "host_dos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t kHelloCom[] = {
    0xB4,0x09, 0xBA,0x0C,0x01, 0xCD,0x21, 0xB8,0x00,0x4C, 0xCD,0x21,
    'H','e','l','l','o',' ','f','r','o','m',' ','m','i','c','r','o','D','O','S','!',13,10,'$'
};

static int run_interp(uint8_t *memory,char *output,size_t cap)
{
    MdRuntime r; MdHostDos host; MdHooks h; MdStopReason stop;
    memset(memory,0,MD_X86_ADDRESS_SPACE); md_host_dos_init(&host,output,cap);
    h.interrupt=md_host_dos_interrupt; h.in8=NULL; h.out8=NULL; h.user=&host;
    md_runtime_init(&r,memory,&h); md_runtime_load_com(&r,kHelloCom,sizeof(kHelloCom),0x1000u); stop=md_interp_run(&r,1000u);
    printf("interp: stop=%s instructions=%llu exit=%u\n",md_stop_reason_name(stop),(unsigned long long)r.instructions,(unsigned)r.exit_code);
    return stop==MD_STOP_EXIT&&r.exit_code==0u?0:1;
}
static int run_recomp(uint8_t *memory,char *output,size_t cap)
{
    MdRuntime r; MdHostDos host; MdHooks h; MdStopReason stop;
    memset(memory,0,MD_X86_ADDRESS_SPACE); md_host_dos_init(&host,output,cap);
    h.interrupt=md_host_dos_interrupt; h.in8=NULL; h.out8=NULL; h.user=&host;
    md_runtime_init(&r,memory,&h); stop=md_recomp_hello(&r,0x1000u,1000u);
    printf("recomp: stop=%s instructions=%llu exit=%u\n",md_stop_reason_name(stop),(unsigned long long)r.instructions,(unsigned)r.exit_code);
    return stop==MD_STOP_EXIT&&r.exit_code==0u?0:1;
}
int main(void)
{
    uint8_t *memory=(uint8_t*)malloc(MD_X86_ADDRESS_SPACE); char a[256],b[256]; int failed=0;
    if(!memory){fprintf(stderr,"could not allocate 1 MiB guest address space\n");return 1;}
    failed|=run_interp(memory,a,sizeof(a)); failed|=run_recomp(memory,b,sizeof(b));
    printf("output: %s",a); if(strcmp(a,b)!=0){fprintf(stderr,"interpreter/AOT output mismatch\n");failed=1;} free(memory); return failed;
}
