#!/bin/sh
# NV2-G G-1 qemu-arm differential test.
#
# Runs generated Thumb-2 from src/runtime/native_v2g.c against the canonical
# interpreter. Linux or WSL:
#
#   sudo apt install gcc-arm-linux-gnueabihf qemu-user
#   sh scripts/md_nativev2g_diff.sh [random-cases] [seed] [directed-states]
#
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/build-nativev2g-diff}"
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

# Match microdos_pico_nativev2g exactly for runtime/layout-affecting defines.
DEFS="\
-DMD_THREADED_DISPATCH=1 \
-DMICRODOS_ENABLE_NATIVE_V2=1 \
-DMICRODOS_ENABLE_NATIVE_V2G=1 \
-DMD_X86_TRACK_WRITES=0 \
-DMICRODOS_TRANSLATION_SUPPORT=0"

# Important: x86_block_cache.c is intentionally NOT linked here.
# The Pico NV2-G target also runs with MICRODOS_TRANSLATION_SUPPORT=0 and
# does not include the decoded block-cache implementation. Pulling that file
# into this harness would require MdRuntime translation-only fields that are
# correctly compiled out in the NV2-G configuration.
SRCS="\
$ROOT/src/runtime/runtime.c \
$ROOT/src/runtime/x86_interp.c \
$ROOT/src/runtime/region.c \
$ROOT/src/decode/x86_decode.c \
$ROOT/src/runtime/native_v2.c \
$ROOT/src/runtime/native_v2g.c \
$ROOT/tests/test_native_v2g_diff.c"

echo "== build NV2-G qemu differential =="
$CC $FLAGS $INCS $DEFS $SRCS -o "$OUT/test_native_v2g_diff"

echo "== execute generated Thumb-2 under qemu-arm =="
exec qemu-arm "$OUT/test_native_v2g_diff" \
    "${1:-4000}" \
    "${2:-0x4E563247}" \
    "${3:-64}"
