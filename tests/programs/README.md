# Validation `.COM` images

These are tiny hand-authored machine-code fixtures, so the build does not require an assembler.

- `hello.com` — immediate MOV + `INT 21h`; code ends at `010Ch`, followed by string data.
- `loop.com` — backward `JNZ` loop; verifies CFG splitting, cached loop execution, and instruction-count equivalence.
- `hybrid.com` — AOT -> cached interpreter -> AOT. `89 C3` (`mov bx,ax`) is supported by the canonical interpreter but deliberately not by the first AOT/cache decoder.
- `selfmod.com` — generated AOT stores `F4h` over the very next compiled `NOP`. Correct execution must invalidate AOT immediately and execute the newly written `HLT` through the cached path in three guest instructions.

CMake runs `dosrecomp` on all four and compiles the generated C into the host tests.
