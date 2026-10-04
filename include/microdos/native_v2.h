#ifndef MICRODOS_NATIVE_V2_H
#define MICRODOS_NATIVE_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/x86.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MD_NATIVE_V2_CODE_BYTES 1024u
#define MD_NATIVE_V2_MAX_OPS 128u
#define MD_NATIVE_V2_EXEC_FALLBACK 0xFFFFFFFFu

typedef enum MdNativeV2Status {
    MD_NATIVE_V2_OK = 0,
    MD_NATIVE_V2_BAD_ARGUMENT,
    MD_NATIVE_V2_DECODE_ERROR,
    MD_NATIVE_V2_UNSUPPORTED,
    MD_NATIVE_V2_TOO_LARGE,
    MD_NATIVE_V2_BRANCH_RANGE
} MdNativeV2Status;

typedef struct MdNativeV2Code {
    uint32_t _align_word;
    uint8_t bytes[MD_NATIVE_V2_CODE_BYTES];
    uint16_t size;
    uint16_t op_count;
    uint16_t start_ip;
    uint16_t end_ip;
    uint8_t has_local_loop;
    uint8_t phase;
    uint8_t needs_memory;
    uint8_t has_store;
    uint8_t requires_safe_ds_word;
    uint8_t exit_flags_reg;
    uint8_t needs_entry_cf;
    uint8_t cf_sites;
    uint8_t z_sites;
} MdNativeV2Code;

MdNativeV2Status md_native_v2_compile_8086(const uint8_t *image,
                                           size_t image_size,
                                           uint16_t image_base,
                                           uint16_t entry_ip,
                                           MdNativeV2Code *out);

bool md_native_v2_available(void);

/*
 * Returns MD_NATIVE_V2_EXEC_FALLBACK when the region must not execute
 * natively under the current runtime state.
 */
uint32_t md_native_v2_execute(MdX86 *cpu, const MdNativeV2Code *code);

const char *md_native_v2_status_name(MdNativeV2Status status);

#ifdef __cplusplus
}
#endif

#endif
