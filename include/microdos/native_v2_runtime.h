#ifndef MICRODOS_NATIVE_V2_RUNTIME_H
#define MICRODOS_NATIVE_V2_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/native_v2.h"
#include "microdos/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MD_NATIVE_V2_RT_SLOTS
#define MD_NATIVE_V2_RT_SLOTS 32u
#endif

#ifndef MD_NATIVE_V2_RT_MAX_GUEST_BYTES
#define MD_NATIVE_V2_RT_MAX_GUEST_BYTES 64u
#endif

#ifndef MD_NATIVE_V2_RT_MIN_RETIRE
#define MD_NATIVE_V2_RT_MIN_RETIRE 64u
#endif

#define MD_NATIVE_V2_REJECT_BYTES 16u

typedef enum MdNativeV2RuntimeSlotState {
    MD_NV2_RT_EMPTY = 0,
    MD_NV2_RT_REJECTED,
    MD_NV2_RT_COMPILED
} MdNativeV2RuntimeSlotState;

typedef enum MdNativeV2RejectReason {
    MD_NV2_REJECT_NONE = 0,
    MD_NV2_REJECT_COMPILE,
    MD_NV2_REJECT_STORE
} MdNativeV2RejectReason;

typedef struct MdNativeV2RuntimeSlot {
    MdNativeV2Code code;
    uint16_t cs;
    uint16_t ip;
    uint16_t guest_size;
    uint8_t counter_reg;
    uint8_t state;
    uint32_t reject_signature;
    uint8_t reject_reason;
    uint8_t reject_status;
    uint8_t reject_bytes_len;
    uint8_t _pad0;
    uint8_t reject_bytes[MD_NATIVE_V2_REJECT_BYTES];
    uint8_t guest_bytes[MD_NATIVE_V2_RT_MAX_GUEST_BYTES];
    uint32_t entries;
    uint64_t retired;
} MdNativeV2RuntimeSlot;

typedef struct MdNativeV2Runtime {
    MdNativeV2RuntimeSlot slot[MD_NATIVE_V2_RT_SLOTS];

    uint64_t lookups;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t rejected_hits;
    uint64_t probes;
    uint64_t compiles;
    uint64_t compile_rejects;
    uint64_t store_rejects;
    uint64_t store_guard_rejects;
    uint64_t stack_guard_rejects;
    uint64_t muldiv_guard_rejects;
    uint64_t stale_code;
    uint64_t budget_rejects;
    uint64_t chunked_entries;
    uint64_t chunked_iterations;
    uint64_t short_rejects;
    uint64_t runtime_fallbacks;
    uint64_t entries;
    uint64_t retired;
} MdNativeV2Runtime;

typedef struct MdNativeV2RunResult {
    uint64_t retired;
    uint32_t iterations;
    uint16_t cs;
    uint16_t ip;
    uint8_t entered;
} MdNativeV2RunResult;

void md_native_v2_runtime_init(MdNativeV2Runtime *runtime);

bool md_native_v2_runtime_try_execute(MdNativeV2Runtime *runtime,
                                      MdRuntime *machine,
                                      uint64_t budget,
                                      MdNativeV2RunResult *result);

const char *md_native_v2_reject_reason_name(unsigned reason);

#ifdef __cplusplus
}
#endif

#endif
