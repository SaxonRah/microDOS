#!/bin/sh
# Focused NV2-G G-2B2/G-2C qemu-arm differential.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/build-nativev2g-bc-diff}"
CC="${CC:-arm-linux-gnueabihf-gcc}"

mkdir -p "$OUT"

for tool in "$CC" qemu-arm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing tool: $tool"
        echo "Ubuntu/WSL: sudo apt install gcc-arm-linux-gnueabihf qemu-user"
        exit 2
    fi
done

FLAGS="-std=c11 -O2 -Wall -Wextra -Werror -march=armv7-a+fp -mthumb -static"
INCS="-I$ROOT/include -I$ROOT/src/runtime"
DEFS="\
-DMD_THREADED_DISPATCH=1 \
-DMICRODOS_ENABLE_NATIVE_V2=1 \
-DMICRODOS_ENABLE_NATIVE_V2G=1 \
-DMD_NATIVE_V2_CODE_BYTES=2048 \
-DMD_X86_TRACK_WRITES=0 \
-DMICRODOS_TRANSLATION_SUPPORT=0"

SRCS="\
$ROOT/src/runtime/runtime.c \
$ROOT/src/runtime/x86_interp.c \
$ROOT/src/runtime/region.c \
$ROOT/src/decode/x86_decode.c \
$ROOT/src/runtime/native_v2.c \
$ROOT/src/runtime/native_v2g.c \
$ROOT/tests/test_native_v2g_bc_diff.c"

echo "== build NV2-G G-2B2/G-2C focused differential =="
$CC $FLAGS $INCS $DEFS $SRCS -o "$OUT/test_native_v2g_bc_diff"

echo "== execute G-2B2/G-2C generated Thumb-2 under qemu-arm =="
exec qemu-arm "$OUT/test_native_v2g_bc_diff"
