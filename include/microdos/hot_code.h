#ifndef MICRODOS_HOT_CODE_H
#define MICRODOS_HOT_CODE_H

/*
 * M26/M26b selective code placement.
 *
 * Pico SDK 2.3.0's __time_critical_func() places the named function in a
 * .time_critical.* section, copied to SRAM by the normal flash linker script.
 * pico.h is the supported public include on RP2350.
 *
 * MD_HOT_FUNC is the original small M26 hot set.
 * MD_EXEC_HOT_FUNC is the M26b execution-kernel set.  Keeping them separate
 * preserves m26-hot160 as a control while allowing a wider execution-only
 * SRAM placement without moving the compiler/emitter machinery.
 */
#if defined(PICO_BUILD) && \
    (((defined(MICRODOS_PICO_HOT_CODE) && MICRODOS_PICO_HOT_CODE) || \
      (defined(MICRODOS_PICO_EXEC_HOT_CODE) && MICRODOS_PICO_EXEC_HOT_CODE)))
#include "pico.h"
#endif

#if defined(PICO_BUILD) && defined(MICRODOS_PICO_HOT_CODE) && MICRODOS_PICO_HOT_CODE
#define MD_HOT_FUNC(name) __time_critical_func(name)
#else
#define MD_HOT_FUNC(name) name
#endif

#if defined(PICO_BUILD) && defined(MICRODOS_PICO_EXEC_HOT_CODE) && MICRODOS_PICO_EXEC_HOT_CODE
#define MD_EXEC_HOT_FUNC(name) __time_critical_func(name)
#else
#define MD_EXEC_HOT_FUNC(name) name
#endif

/* M26d category isolation.  These preserve normal optimizer decisions while
   relocating only an emitted out-of-line body into SRAM. */
#if defined(PICO_BUILD) && defined(MICRODOS_PICO_COMPILER_HOT_CODE) && MICRODOS_PICO_COMPILER_HOT_CODE
#define MD_COMPILER_HOT_FUNC(name) __attribute__((section(".time_critical.m26d_compiler"))) name
#else
#define MD_COMPILER_HOT_FUNC(name) name
#endif

#if defined(PICO_BUILD) && defined(MICRODOS_PICO_DOS_HOT_CODE) && MICRODOS_PICO_DOS_HOT_CODE
#define MD_DOS_HOT_FUNC(name) __attribute__((section(".time_critical.m26d_dos"))) name
#else
#define MD_DOS_HOT_FUNC(name) name
#endif

#if defined(PICO_BUILD) && defined(MICRODOS_PICO_INTERP_HELPERS_HOT) && MICRODOS_PICO_INTERP_HELPERS_HOT
#define MD_INTERP_INLINE static inline __attribute__((section(".time_critical.m26d_interp")))
#else
#define MD_INTERP_INLINE static inline
#endif

#endif /* MICRODOS_HOT_CODE_H */