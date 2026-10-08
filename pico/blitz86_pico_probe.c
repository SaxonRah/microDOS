/*
 * blitz86 B1/B2 Pico probe. Standalone test target in microDOS build.
 * Validates real ARM cross-compile, PSRAM, USB, and interpreter.
 * JIT execution is intentionally disabled until allocation/exec-memory
 * contracts are verified on device. No claim of a speed comparison.
 */
#include "b86.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/clocks.h"
#include "hardware/psram.h"
#include <stdio.h>
#include <string.h>

#ifndef MICRODOS_PICO_SYS_KHZ
#define MICRODOS_PICO_SYS_KHZ 300000
#endif
static uint8_t __uninitialized_psram("blitz86_mem")
    __attribute__((aligned(64))) guest[B86_MEM_BYTES];
static B86Cpu cpu;

/* mov ax, 1; mov cx, 8192; add ax, 3; dec cx; jnz -6; hlt */
static const uint8_t program[] = {
    0xB8,0x01,0x00, 0xB9,0x00,0x20,
    0x83,0xC0,0x03, 0x49,0x75,0xFA,0xF4
};
static int run_case(void)
{
    uint64_t start, us;
    int result;
    if (!psram_is_available() ||
        !psram_check_address(&guest[0]) ||
        !psram_check_address(&guest[B86_MEM_BYTES-1])) {
        printf("[b86] PSRAM FAIL\n");
        return 0;
    }
    memset(guest,0,sizeof guest);
    memcpy(guest+0x10100,program,sizeof program);
    b86_init(&cpu,guest);
    b86_set_seg(&cpu,B86_CS,0x1000);
    b86_set_seg(&cpu,B86_SS,0x1000);
    b86_set_seg(&cpu,B86_DS,0x1000);
    b86_set_seg(&cpu,B86_ES,0x1000);
    cpu.ip=0x100;
    start=time_us_64();
    result=b86_run_interp(&cpu,100000);
    us=time_us_64()-start;
    printf("[b86] interp result=%d AX=%04lX CX=%04lX icount=%llu us=%llu\n",
           result,(unsigned long)(cpu.r[B86_AX]&65535u),
           (unsigned long)(cpu.r[B86_CX]&65535u),
           (unsigned long long)cpu.icount,(unsigned long long)us);
    /* 1 + 8192*3 mod 65536 = 0x6001. 8192 loops, CX=0. */
    return result == B86_HALT &&
        (cpu.r[B86_AX]&65535u)==0x6001u &&
        (cpu.r[B86_CX]&65535u)==0;
}
int main(void)
{
    stdio_init_all();
    if (!set_sys_clock_khz(MICRODOS_PICO_SYS_KHZ,false)) {
        printf("[b86] clock failure\n");
    }
    if (psram_configure_params(PICO_DEFAULT_PSRAM_MAX_FREQ,
                                PICO_DEFAULT_PSRAM_MAX_SELECT,
                                PICO_DEFAULT_PSRAM_MIN_DESELECT)!=0 ||
        psram_reinitialize()!=0) {
        printf("[b86] PSRAM initialization failure\n");
    }
    while(!stdio_usb_connected()) sleep_ms(50);
    sleep_ms(300);
    printf("[b86] BEGIN B1/B2 probe clock=%lu kHz\n",
           (unsigned long)(clock_get_hz(clk_sys)/1000u));
    int ok=run_case();
    printf("[b86] COMPLETE result=%s\n",ok?"PASS":"FAIL");
    stdio_flush();
    while(true) {
        int ch=getchar_timeout_us(0);
        if(ch=='R'||ch=='r') {
            ok=run_case();
            printf("[b86] COMPLETE result=%s\n",ok?"PASS":"FAIL");
            stdio_flush();
        }
        sleep_ms(20);
    }
}
