#ifndef MICRODOS_HOT_CODE_H
#define MICRODOS_HOT_CODE_H

/*
 * M26 selective code placement.
 *
 * Pico SDK 2.3.0's __time_critical_func() places the named function in a
 * .time_critical.* section, which the normal flash linker script copies to
 * SRAM.  In SDK 2.3.0 the macro is already no-inline; 2.3.1 later split out
 * a separate __no_inline_time_critical_func() spelling.  microDOS currently
 * builds against 2.3.0, so use the portable 2.3.0 spelling here.
 *
 * pico.h is the supported public include.  Including pico/platform.h directly
 * is rejected by SDK 2.3.0 on RP2350.
 */
#if defined(PICO_BUILD) && defined(MICRODOS_PICO_HOT_CODE) && MICRODOS_PICO_HOT_CODE
#include "pico.h"
#define MD_HOT_FUNC(name) __time_critical_func(name)
#else
#define MD_HOT_FUNC(name) name
#endif

#endif /* MICRODOS_HOT_CODE_H */
