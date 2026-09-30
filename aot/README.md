# aot/

Source-assisted metadata for `dosrecomp` builds of third-party binaries.

## msdos2.entries

Entry points for the released MS-DOS 2.0 `MSDOS.SYS` (raw image, base 0000h),
used when `third_party/msdos` is present. Two sources:

- `table 0x06AA 88`: the INT 21h dispatch table. The kernel dispatches with
  `PUSH CS:[BX+DISPATCH]` + `RET` at 0647h (MSCODE.ASM), which static analysis
  cannot follow; the table holds 88 near pointers for functions 00h-57h.
- One offset per line, profile-guided: every kernel offset reached by a
  non-sequential transfer during a DOS2TEST session (IVT handlers, device
  return paths, push/ret targets). Regenerate with:

      microdos_dos2_e2e MSDOS.SYS e2e_msdos2.img --profile-kernel msdos2.entries

The binary stays authoritative: entries only tell `dosrecomp` where to start
decoding; anything not discovered simply runs in the interpreter.
