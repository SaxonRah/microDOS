.section .text.boot
.align 7
.global _start
.type _start, %function

/*
 * microDOS - Raspberry Pi Zero 2 W AArch64 bare-metal entry
 *
 * The Pi firmware enters this kernel at EL2 with the stage-1 MMU disabled.
 * In AArch64, MMU-off data accesses are Device-nGnRnE.  GCC's normal
 * AArch64 code generation is therefore not safe for ordinary C data that is
 * only naturally aligned to its C type: it may legally combine adjacent
 * halfword stores into an unaligned 32-bit store.
 *
 * microDOS hit exactly that case in MdX86 lazy flags:
 *
 *     lazy_a   @ byte 30
 *     lazy_b   @ byte 32
 *
 * GCC emitted:
 *
 *     stur wzr, [x6, #30]
 *
 * which is a 32-bit access at a 2-byte-aligned address.  It works for Normal
 * memory but faults for Device memory.
 *
 * This startup therefore installs a small EL2 stage-1 identity map:
 *
 *   0x00000000 .. 0x3EFFFFFF  Normal memory, AttrIdx 0
 *   0x3F000000 .. 0x3FFFFFFF  Device-nGnRnE, AttrIdx 1
 *
 * The mapping uses:
 *
 *   - 4 KiB translation granule
 *   - T0SZ = 32 (32-bit VA space; translation begins at level 1)
 *   - one L1 table
 *   - one L2 table containing 512 x 2 MiB block descriptors
 *
 * The MMU mapping is validated. This variant enables only the instruction
 * cache; the data cache remains off so D-cache maintenance can be brought up
 * separately as its own controlled change.
 */

_start:
    /* Firmware starts kernel8.img in AArch64. Park secondary cores. */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xff
    cbz     x0, 1f

0:
    wfe
    b       0b

1:
    /*
     * Keep asynchronous exceptions out until microDOS has real vector/IRQ
     * support. Polling UART is intentional for the first bare-metal port.
     */
    msr     daifset, #0xf

    ldr     x0, =__stack_top
    mov     sp, x0

    /*
     * Zero .bss first.  The EL2 translation tables live in .bss.mmu, so they
     * must be cleared before we populate them.
     */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
2:
    cmp     x0, x1
    b.hs    3f
    str     xzr, [x0], #8
    b       2b

3:
    /*
     * The observed Raspberry Pi Zero 2 W firmware entry state is EL2.
     * Install the stage-1 EL2 identity map before entering C.
     *
     * If a future firmware enters at another EL, leave the MMU untouched.
     * kernel_main's CurrentEL diagnostic will make that immediately visible.
     */
    mrs     x0, currentel
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #2
    b.ne    4f

    bl      pi0_enable_mmu_el2

4:
    bl      kernel_main

5:
    wfe
    b       5b

.size _start, . - _start


/*
 * pi0_enable_mmu_el2
 *
 * Builds a two-level, 4 KiB-granule stage-1 translation regime at EL2.
 *
 * L1[0] points at the L2 table and covers virtual addresses below 1 GiB.
 * The 512 L2 entries are 2 MiB identity-mapped blocks.
 *
 * Entries 0..503:
 *     0x00000000 .. 0x3EFFFFFF
 *     AttrIdx=0, AF=1, Inner Shareable, Normal memory
 *
 * Entries 504..511:
 *     0x3F000000 .. 0x3FFFFFFF
 *     AttrIdx=1, AF=1, Device-nGnRnE
 *
 * Descriptor bits used:
 *
 *     bit  0      Valid
 *     bit  1      0 for L1/L2 block, 1 for table descriptor
 *     bits 4:2    AttrIndx
 *     bits 9:8    Shareability
 *     bit  10     Access Flag
 *
 * Normal L2 block descriptor low bits:
 *     Valid | AF | SH=Inner = 0x701
 *
 * Device L2 block descriptor low bits:
 *     Valid | AF | AttrIdx=1 = 0x405
 */
.type pi0_enable_mmu_el2, %function
pi0_enable_mmu_el2:
    /*
     * Build L2 first.
     *
     * x0 = L2 base
     * x1 = entry index
     * x2 = 2 MiB block physical base
     * x3 = descriptor
     */
    ldr     x0, =pi0_mmu_l2
    mov     x1, #0

6:
    lsl     x2, x1, #21              /* physical base = index * 2 MiB */

    cmp     x1, #504                 /* 0x3F000000 / 2 MiB */
    b.hs    7f

    /* Normal memory: AttrIdx 0, Inner Shareable, AF, valid block. */
    mov     x3, #0x701
    orr     x3, x3, x2
    b       8f

7:
    /* Device-nGnRnE: AttrIdx 1, AF, valid block. */
    mov     x3, #0x405
    orr     x3, x3, x2

8:
    str     x3, [x0, x1, lsl #3]
    add     x1, x1, #1
    cmp     x1, #512
    b.lo    6b

    /*
     * L1[0] = table descriptor pointing to pi0_mmu_l2.
     * Remaining L1 entries stay zero/invalid from the .bss clear.
     */
    ldr     x0, =pi0_mmu_l1
    ldr     x1, =pi0_mmu_l2
    mov     x2, #3                   /* valid table descriptor */
    orr     x1, x1, x2
    str     x1, [x0]

    /*
     * MAIR_EL2
     *
     * AttrIdx 0 = 0xFF: Normal, Inner/Outer Write-Back,
     *                   Read/Write Allocate.
     * AttrIdx 1 = 0x00: Device-nGnRnE.
     *
     * SCTLR_EL2.C remains 0 in this step; RAM is Normal memory but the
     * data/unified cache remains disabled.
     */
    mov     x0, #0xff
    msr     mair_el2, x0

    /*
     * TCR_EL2 = 0x3520
     *
     * T0SZ  = 32  -> 32-bit VA space
     * IRGN0 = 01  -> WB RA/WA table walks
     * ORGN0 = 01  -> WB RA/WA table walks
     * SH0   = 11  -> Inner Shareable
     * TG0   = 00  -> 4 KiB granule
     * PS    = 000 -> 32-bit physical address size
     */
    mov     x0, #0x3520
    msr     tcr_el2, x0

    /* TTBR0_EL2 points at the level-1 table. */
    ldr     x0, =pi0_mmu_l1
    msr     ttbr0_el2, x0

    /*
     * Make table writes visible and invalidate any stale EL2 stage-1 TLB
     * state before enabling translation.
     */
    dsb     sy
    tlbi    alle2
    dsb     sy
    isb

    /*
     * Bring up the instruction cache as a separate controlled step.
     *
     * Invalidate the local instruction cache first, then preserve all
     * firmware-provided SCTLR_EL2 bits and set:
     *
     *     M (bit 0)   stage-1 MMU
     *     I (bit 12)  instruction cache
     *
     * C (bit 2) remains clear until the microDOS-owned RAM range has been
     * invalidated by VA below.
     */
    ic      iallu
    dsb     sy
    isb

    mrs     x0, sctlr_el2
    orr     x0, x0, #1
    orr     x0, x0, #0x1000
    msr     sctlr_el2, x0
    isb

    /*
     * Controlled D-cache bring-up.
     *
     * The failed earlier experiment attempted a global set/way invalidate.
     * For this test, invalidate only microDOS-owned Normal RAM by VA:
     *
     *     _start .. __image_end
     *
     * That range contains the kernel image, BSS, guest RAM, disk RAM,
     * MMU tables, and stack.  MMU translation is already active here, but
     * SCTLR_EL2.C is still clear.
     *
     * CTR_EL0.DminLine (bits 19:16) is log2(words per D-cache line), so
     * line_shift = DminLine + 2 gives log2(bytes per line).
     */
    mrs     x3, ctr_el0
    ubfx    x3, x3, #16, #4
    add     x3, x3, #2

    mov     x4, #1
    lsl     x4, x4, x3              /* x4 = cache line bytes */
    sub     x5, x4, #1              /* x5 = line mask */

    ldr     x1, =_start
    bic     x1, x1, x5              /* align start downward */
    ldr     x2, =__image_end

9:
    cmp     x1, x2
    b.hs    10f
    dc      ivac, x1
    add     x1, x1, x4
    b       9b

10:
    dsb     sy
    isb

    /*
     * Enable the data/unified cache only after the microDOS-owned range has
     * been invalidated.  Preserve the already-enabled MMU and I-cache bits.
     */
    mrs     x0, sctlr_el2
    orr     x0, x0, #4
    msr     sctlr_el2, x0
    isb

    ret

.size pi0_enable_mmu_el2, . - pi0_enable_mmu_el2


/*
 * Translation tables.
 *
 * Both must be 4 KiB aligned.  They are NOLOAD BSS storage and are cleared by
 * _start before pi0_enable_mmu_el2 populates them.
 */
.section .bss.mmu, "aw", %nobits
.balign 4096
pi0_mmu_l1:
    .skip 4096

.balign 4096
pi0_mmu_l2:
    .skip 4096
