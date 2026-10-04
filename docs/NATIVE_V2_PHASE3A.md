# Native v2 Phase 3A — First live MS-DOS admission

Phase 2D proved that Pico Plus 2 PSRAM is effectively SRAM-speed through a
16 KiB hot working set, so Phase 3A does **not** add a software memory cache.

The new `microdos_pico_nativev2` target contains only:

```text
canonical threaded interpreter
    +
Native v2 exact counted-loop tier
```

No static AOT. No old JIT. No decoded block cache.

Phase 3A admits only exact read-only loops of the form:

```asm
entry:
    ; Native-v2-supported straight-line body
    ...
    dec counter
    jnz entry
```

The counter may not be written elsewhere. This makes retirement exact:

```text
iterations = counter_at_entry ? counter_at_entry : 65536
retired = iterations * operations_per_iteration
```

Every native entry validates the exact guest bytes used to compile the slot.
If interpreted code changed them, the slot is invalidated and reprobed.
Native loops containing stores are rejected in Phase 3A so a loop cannot
self-modify after the pre-entry byte check.

Unsupported code remains on the current direct-threaded interpreter. The live
DOS loop returns to the Native-v2 admission point every 127 interpreted guest
instructions; 127 avoids power-of-two phase locking in short guest loops.

Ctrl+] reports real Native-v2 coverage, compiles, entries, retired guest
instructions, invalidations and rejection reasons.
