#ifndef MICRODOS_RECOMP_MD_RECOMP_HYBRID_H
#define MICRODOS_RECOMP_MD_RECOMP_HYBRID_H

#include <stdint.h>
#include "microdos/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

MdStopReason md_recomp_hybrid(MdRuntime *runtime, uint16_t segment, uint64_t instruction_budget);

#ifdef __cplusplus
}
#endif
#endif
