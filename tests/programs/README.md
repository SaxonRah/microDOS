# Validation `.COM` images

These are tiny hand-authored machine-code fixtures, so the build does not require an assembler.

- `hello.com` — immediate MOV + `INT 21h`; code ends at `010Ch`, followed by string data.
- `loop.com` — backward `JNZ` loop; verifies CFG splitting and instruction-count equivalence.
- `hybrid.com` — AOT -> interpreter -> AOT. `89 C3` (`mov bx,ax`) is supported by the interpreter but deliberately not by the first AOT decoder.

CMake runs `dosrecomp` on all three and compiles the generated C into the host tests.
