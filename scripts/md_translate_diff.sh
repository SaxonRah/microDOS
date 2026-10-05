#!/bin/sh
# M25 translator differential test on a desktop (Linux or WSL).
#
# Builds the runtime + translator for 32-bit ARM Linux in Thumb mode and runs
# tests/test_translate_diff.c under qemu-arm, which executes the generated
# Thumb-2 code for real next to the canonical interpreter.
#
#   sudo apt install gcc-arm-linux-gnueabihf qemu-user
#   sh scripts/md_translate_diff.sh [cases] [seed] [unaligned 0|1]
#
# Bisect one failing case:  sh scripts/md_translate_diff.sh <cases> <seed> <0|1> <case-index>
set -e
cd "$(dirname "$0")/.."
OUT=build-translate-diff
mkdir -p "$OUT"
arm-linux-gnueabihf-gcc -mthumb -O2 -Wall -Wextra -static \
    -Iinclude -Isrc/runtime -DMD_THREADED_DISPATCH=1 \
    src/runtime/runtime.c src/runtime/x86_interp.c src/runtime/x86_block_cache.c \
    src/runtime/region.c src/runtime/translate.c src/decode/x86_decode.c \
    tests/test_translate_diff.c -o "$OUT/test_translate_diff"
exec qemu-arm "$OUT/test_translate_diff" "${1:-2000}" "${2:-0x4D32355B}" "${3:-0}" ${4:+"$4"}
