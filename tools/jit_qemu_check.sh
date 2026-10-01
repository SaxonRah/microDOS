#!/bin/sh
# Run the JIT tests as native Thumb-2 code under qemu-arm (Linux or WSL).
# Needs: gcc-arm-linux-gnueabihf qemu-user   (apt install ...)
# This executes the code the JIT emits, so code-generation bugs show up
# before a UF2 reaches the RP2350. ARMv7-A Thumb-2 is used as the closest
# user-mode stand-in for the Cortex-M33 (the JIT emits no M-profile-only
# instructions in the paths these tests cover).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-arm-linux-gnueabihf-gcc}"
OUT="${OUT:-$ROOT/build-qemu}"
mkdir -p "$OUT"
SRCS="$ROOT/src/runtime/jit_thumb2.c $ROOT/src/runtime/runtime.c $ROOT/src/runtime/x86_interp.c $ROOT/src/runtime/x86_block_cache.c $ROOT/src/runtime/region.c $ROOT/src/decode/x86_decode.c"
FLAGS="-std=c11 -O2 -Wall -Wextra -march=armv7-a+fp -mthumb -static -I$ROOT/include -I$ROOT/src/runtime"
$CC $FLAGS "$ROOT/tests/test_jit_diff.c" $SRCS -o "$OUT/jit_diff_arm"
# test_jit.c keeps its code buffer on the stack (executable SRAM on the
# RP2350); Linux needs an executable stack for that.
$CC $FLAGS -z execstack "$ROOT/tests/test_jit.c" $SRCS -o "$OUT/jit_m203_arm"
echo "== JIT differential (native Thumb-2 under qemu-arm)"
qemu-arm "$OUT/jit_diff_arm"
echo "== M20.3 spec (LODS/LOOP resident regions)"
qemu-arm "$OUT/jit_m203_arm"

# Full DOS session with the runtime JIT (M20.4): DOS2TEST under the Pico's
# exact JIT configuration, generated code executed as Thumb-2. Needs a host
# build (generated recompiled sources + the e2e disk image) and the pinned
# MS-DOS 2.0 binaries. HOST_BUILD defaults to build-host (md.bat) or build.
HOST_BUILD="${HOST_BUILD:-}"
if [ -z "$HOST_BUILD" ]; then
    for d in "$ROOT/build-host" "$ROOT/build"; do
        if [ -f "$d/generated/msdos2_recomp.c" ]; then HOST_BUILD="$d"; break; fi
    done
fi
KERNEL="$ROOT/third_party/msdos/v2.0/bin/MSDOS.SYS"
if [ -n "$HOST_BUILD" ] && [ -f "$KERNEL" ] && [ -f "$HOST_BUILD/e2e_msdos2.img" ]; then
    G="$HOST_BUILD/generated"
    JITDEF="-DMICRODOS_ENABLE_JIT=1 -DMD_JIT_BLOCK_SLOTS=64 -DMD_JIT_HOT_THRESHOLD=2 -DMD_JIT_HOTNESS_SLOTS=64 -DMICRODOS_SYSTEM_JIT_CODE_BYTES=24576"
    $CC $FLAGS $JITDEF -DMD_THREADED_DISPATCH=1 -I$ROOT/src/system -I$ROOT/src/host -I$G \
        "$ROOT/tests/test_dos2_e2e.c" "$ROOT/src/system/md_dos2_system.c" "$ROOT/src/host/msdos2_boot.c" \
        "$G/dos2test_recomp.c" "$G/msdos2_recomp.c" $SRCS -o "$OUT/dos2_e2e_jit_arm"
    echo "== Full DOS2TEST session with the runtime JIT (native Thumb-2, Pico JIT config)"
    qemu-arm "$OUT/dos2_e2e_jit_arm" "$KERNEL" "$HOST_BUILD/e2e_msdos2.img" --no-cache > "$OUT/dos2_e2e_jit.log" 2>&1
    grep "passed:\|\[jit\]" "$OUT/dos2_e2e_jit.log"
    grep -q "passed: 25   failed: 0" "$OUT/dos2_e2e_jit.log"
    echo "full-DOS JIT session: ok"
else
    echo "== Full-DOS JIT session skipped (needs a host build with generated sources, the e2e image and third_party/msdos)"
fi
