# Native v2 Phase 3J — exact FLAGS/stack counted loop

Phase 3I checkpoint:

- Host regression suite PASS
- DOS2TEST 25/25
- MDSTRESS checksum 0xA298
- Native coverage 12.6%
- Native retirement 2,418,133 guest instructions
- Active 3.300 MIPS
- Runtime fallback 0
- Store guard rejection 0
- MUL/DIV guard rejection 0
- Phase-5 MUL/DIV loop 14CB:0395:
  - phase=10
  - entries=8
  - retired=727,904

Phase 3J targets MDSTRESS Phase 7:

```asm
.inner:
    xchg ax,bx
    pushf
    lahf
    xor ah,5Ah
    sahf
    popf
    not ax
    neg bx
    cbw
    cwd
    xchg dx,si
    inc ax
    dec bx
    loop .inner
```

The Phase-3C profile measured 262,136 taken LOOP backedges at this site.

## Exact FLAGS model

This loop consumes the previous iteration's arithmetic flags through LAHF,
so a simple exit-only lazy-flags handoff is insufficient.

Phase 3J keeps a complete materialized x86 FLAGS word in host r11 for the
entire native region.

- entry: materialize lazy x86 FLAGS once
- PUSHF: write the exact r11 word to guest SS:SP
- LAHF: construct AH from SF/ZF/AF/PF/CF
- XOR AH,5Ah: execute the real register change
- SAHF: update the status bits in r11
- POPF: reload the exact pushed word
- NEG BX: retain its CF
- DEC BX: synthesize exact OF/SF/ZF/AF/PF while preserving NEG's CF
- LOOP: does not alter the x86 FLAGS word
- exit: write r11 directly to flags_raw and clear lazy_op

The DEC status synthesis is helper-free and branchless.

## Real guest stack semantics

PUSHF/POPF are not optimized away. The native region performs the actual
guest halfword store and load through SS:SP, and SP is decremented/incremented
exactly as on 8086.

Runtime admits the native region only when:

- SS <= EFFFh, so a direct halfword cannot wrap physical 1 MiB memory
- the PUSHF stack word does not overlap the currently executing guest code

Writes to some other compiled guest region remain coherent because each
Native-v2 cache entry byte-compares its full guest bytes before re-entry.

## Native register use

```text
r0..r7  AX,CX,DX,BX,SP,BP,SI,DI
r8      MdX86*
r9      guest RAM host pointer
r10     cached SS << 4
r11     exact x86 FLAGS word
r12     scratch
```

Guest DI is unused by this measured loop. Its value is saved once on the
host stack so r7 can serve as a second scratch register for parity/auxiliary
flag synthesis, then restored before native exit.

Expected metadata:

```text
bytes=20
ops=14
phase=11
terminal=E2
needs_memory=1
has_store=1
needs_entry_flags=1
safe_stack_pushpop=1
```
