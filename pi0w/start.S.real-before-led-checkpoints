.section .text.boot
.align 7
.global _start
.type _start, %function

_start:
    /* Firmware starts kernel8.img in AArch64. Park secondary cores. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbz     x0, 1f

0:
    wfe
    b       0b

1:
    /* Keep asynchronous exceptions out until microDOS has real vector/IRQ
       support. Polling UART is intentional for the first bare-metal port. */
    msr     daifset, #0xf

    ldr     x0, =__stack_top
    mov     sp, x0

    /* Zero .bss, including guest RAM/disk working buffers. */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:
    cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b

3:
    bl      kernel_main

4:
    wfe
    b       4b

.size _start, . - _start
