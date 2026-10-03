/*
 * Compatibility translation unit.
 *
 * The implementation moved to jit_core.c + jit_thumb2_backend.inc.
 * Existing out-of-tree commands that still compile jit_thumb2.c keep working
 * through this wrapper.
 */
#ifndef MD_JIT_BACKEND_THUMB2
#define MD_JIT_BACKEND_THUMB2 1
#endif
#include "jit_core.c"