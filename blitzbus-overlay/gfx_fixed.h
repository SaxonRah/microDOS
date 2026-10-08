#ifndef GFX_FIXED_H
#define GFX_FIXED_H

#include <stdint.h>

/* 16.16 fixed point by default.
   For very old 16-bit DOS compilers where int64_t code generation is painful,
   define GFX_FIXED_NO_INT64 to use 8.8 fixed point with 32-bit intermediate
   math.
*/
#ifdef GFX_FIXED_NO_INT64

typedef int32_t gfx_fixed_t;
#define GFX_FIXED_SHIFT 8
#define GFX_FIXED_ONE ((gfx_fixed_t)(1L << GFX_FIXED_SHIFT))
#define GFX_TO_FIXED(x) ((gfx_fixed_t)((int32_t)(x) << GFX_FIXED_SHIFT))
#define GFX_FROM_FIXED(x) ((int)((x) / GFX_FIXED_ONE))
#define GFX_FIXED_MUL(a, b)                                                    \
  ((gfx_fixed_t)(((int32_t)(a) * (int32_t)(b)) >> GFX_FIXED_SHIFT))
#define GFX_FIXED_DIV(a, b)                                                    \
  ((gfx_fixed_t)(((int32_t)(a) << GFX_FIXED_SHIFT) / (int32_t)(b)))

#else

typedef int32_t gfx_fixed_t;
#define GFX_FIXED_SHIFT 16
#define GFX_FIXED_ONE ((gfx_fixed_t)(1L << GFX_FIXED_SHIFT))
#define GFX_TO_FIXED(x) ((gfx_fixed_t)((int32_t)(x) << GFX_FIXED_SHIFT))
#define GFX_FROM_FIXED(x) ((int)((x) / GFX_FIXED_ONE))
#define GFX_FIXED_MUL(a, b)                                                    \
  ((gfx_fixed_t)(((int64_t)(a) * (int64_t)(b)) >> GFX_FIXED_SHIFT))
#define GFX_FIXED_DIV(a, b)                                                    \
  ((gfx_fixed_t)(((int64_t)(a) << GFX_FIXED_SHIFT) / (int64_t)(b)))

#endif

#endif /* GFX_FIXED_H */
