#ifndef MICRODOS_NATIVE3_H
#define MICRODOS_NATIVE3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/runtime.h"
#include "microdos/jit.h"
#include "microdos/native_v2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MD_N3_PROFILE
#define MD_N3_PROFILE 1
#endif
#ifndef MD_N3_SITE_SLOTS
#define MD_N3_SITE_SLOTS 256u
#endif
#ifndef MD_N3_SHADOW_SLOTS
#define MD_N3_SHADOW_SLOTS 32u
#endif
#ifndef MD_N3_CFG_PREFETCH
#define MD_N3_CFG_PREFETCH 4u
#endif
#ifndef MD_N3_INTERP_ESCAPE
#define MD_N3_INTERP_ESCAPE 32u
#endif
#ifndef MD_N3_PREWARM_MAX
#define MD_N3_PREWARM_MAX 1024u
#endif

#define MD_N3_CACHE_MAGIC 0x334E444Du
#define MD_N3_CACHE_VERSION 1u

typedef enum MdN3Backend {
    MD_N3_BACKEND_NONE = 0,
    MD_N3_BACKEND_THUMB_NV2,
    MD_N3_BACKEND_THUMB_JIT,
    MD_N3_BACKEND_A64_JIT,
    MD_N3_BACKEND_PORTABLE
} MdN3Backend;

typedef enum MdN3SiteState {
    MD_N3_SITE_EMPTY = 0,
    MD_N3_SITE_SEEN,
    MD_N3_SITE_NATIVE,
    MD_N3_SITE_FALLBACK,
    MD_N3_SITE_UNSTABLE
} MdN3SiteState;

typedef struct MdN3Site {
    uint16_t cs, ip;
    uint32_t signature;
    uint32_t gen0, gen1;
    uint8_t page0, page1, page_count;
    uint8_t state, backend, heat, penalty, flags;
} MdN3Site;

typedef struct MdN3Shadow {
    uint16_t return_cs, return_ip;
    uint16_t target_cs, target_ip;
    uint8_t valid;
    uint8_t _pad[3];
} MdN3Shadow;

typedef struct MdN3Stats {
    uint64_t entries, retired, native_retired, interp_retired;
    uint64_t lookups, hits, misses, probes, compiles, rejects, invalidations;
    uint64_t nv2_entries, nv2_retired, jit_entries, jit_retired;
    uint64_t interp_escapes;
    uint64_t shadow_pushes, shadow_hits, shadow_misses;
    uint64_t prewarm_requests, prewarm_success;
    uint64_t cache_imported, cache_exported;
    uint64_t smc_rejects, budget_rejects;
} MdN3Stats;

typedef struct MdN3CacheHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t record_count;
    uint32_t guest_address_bits;
    uint32_t feature_bits;
    uint32_t checksum;
} MdN3CacheHeader;

typedef struct MdN3CacheRecord {
    uint16_t cs, ip;
    uint32_t signature;
    uint32_t gen0, gen1;
    uint8_t page0, page1, page_count, backend_hint;
    uint8_t heat, flags;
    uint16_t _pad;
} MdN3CacheRecord;

typedef struct MdN3Prewarm { uint16_t cs, ip; } MdN3Prewarm;

typedef struct MdN3RunResult {
    uint64_t retired, native, interpreted;
    uint8_t entered, backend, invalidated, stop;
} MdN3RunResult;

typedef struct MdNative3 {
    MdJit jit;
    MdNativeV2Runtime nv2;
    uint8_t *jit_code;
    size_t jit_code_bytes;
    MdN3Site site[MD_N3_SITE_SLOTS];
    MdN3Shadow shadow[MD_N3_SHADOW_SLOTS];
    uint8_t shadow_top, shadow_count, initialized, profile;
    uint32_t seen_code_epoch;
    MdN3Stats stats;
} MdNative3;

void md_native3_init(MdNative3 *n3, void *jit_code, size_t jit_code_bytes);
void md_native3_reset(MdNative3 *n3);
bool md_native3_run(MdNative3 *n3, MdRuntime *rt, uint64_t budget, MdN3RunResult *result);
bool md_native3_prepare(MdNative3 *n3, MdRuntime *rt, uint16_t cs, uint16_t ip);
unsigned md_native3_prewarm(MdNative3 *n3, MdRuntime *rt, const MdN3Prewarm *entries, unsigned count);
size_t md_native3_cache_export(const MdNative3 *n3, void *dst, size_t capacity);
bool md_native3_cache_import(MdNative3 *n3, MdRuntime *rt, const void *src, size_t bytes);
const MdN3Stats *md_native3_stats(const MdNative3 *n3);
const char *md_native3_backend_name(unsigned backend);

#ifdef __cplusplus
}
#endif
#endif
