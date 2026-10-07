#ifndef MICRODOS_RUNTIME_H
#define MICRODOS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "microdos/x86.h"

#ifndef MICRODOS_TRANSLATION_SUPPORT
#define MICRODOS_TRANSLATION_SUPPORT 1
#endif

#ifndef MD_INTERP_OPCODE_PROFILE
#define MD_INTERP_OPCODE_PROFILE 0
#endif

#ifndef MICRODOS_INT_PROFILE
#define MICRODOS_INT_PROFILE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdStopReason {
    MD_STOP_NONE = 0,
    MD_STOP_EXIT,
    MD_STOP_HALT,
    MD_STOP_BUDGET,
    MD_STOP_FAULT
} MdStopReason;

struct MdRuntime;
#ifndef MD_AOT_LIVE_PAGES
#define MD_AOT_LIVE_PAGES 8u    /* 4 KiB: DOS kernel (5 pages) + a compiled program; beyond that, pages fall back to the exact guard path */
#endif

typedef struct MdRuntime MdRuntime;
struct MdBlockCache;
typedef struct MdBlockCache MdBlockCache;

/* The threaded interpreter can return at taken JNZ/LOOP back-edges so a
   native tier (Native v2, or the M25 translator's tiering) can take over at
   loop heads. Native v2 builds always enable it. */
#if defined(MICRODOS_ENABLE_NATIVE_V2) || defined(MICRODOS_ENABLE_BACKEDGE_EXIT)
#define MD_INTERP_BACKEDGE_EXIT 1
#else
#define MD_INTERP_BACKEDGE_EXIT 0
#endif
/* M25 tiering also exits at every taken backward short Jcc and JMP (loops
   closed by JB/JL/JMP). Native v2 keeps its original JNZ/LOOP-only set. */
#if defined(MICRODOS_ENABLE_BACKEDGE_EXIT)
#define MD_INTERP_BACKEDGE_ALL 1
#else
#define MD_INTERP_BACKEDGE_ALL 0
#endif

#if defined(MICRODOS_ENABLE_NATIVE_V2) && defined(MICRODOS_NATIVE_V2_BACKEDGE_PROFILE)
#ifndef MD_NATIVE_V2_BACKEDGE_SLOTS
#define MD_NATIVE_V2_BACKEDGE_SLOTS 64u
#endif
#define MD_NATIVE_V2_BACKEDGE_BYTES 32u

typedef struct MdNativeV2BackedgeSite {
    uint16_t cs;
    uint16_t source_ip;
    uint16_t target_ip;
    uint8_t opcode;
    uint8_t bytes_len;
    uint32_t hits;
    uint8_t bytes[MD_NATIVE_V2_BACKEDGE_BYTES];
} MdNativeV2BackedgeSite;
#endif

typedef bool (*MdInterruptHook)(MdRuntime *runtime, uint8_t vector, void *user);
typedef uint8_t (*MdPortIn8Hook)(MdRuntime *runtime, uint16_t port, void *user);
typedef void (*MdPortOut8Hook)(MdRuntime *runtime, uint16_t port, uint8_t value, void *user);

#if MICRODOS_INT_PROFILE
#ifndef MD_INT_PROFILE_SITE_SLOTS
#define MD_INT_PROFILE_SITE_SLOTS 256u
#endif

typedef struct MdIntProfileSite {
    uint16_t cs;
    uint16_t ip;
    uint16_t ax;
    uint8_t vector;
    uint8_t ah;
    uint32_t count;
} MdIntProfileSite;

typedef struct MdIntProfile {
    uint64_t total;
    uint32_t vector[256];
    uint32_t int21_ah[256];
    uint32_t int10_ah[256];
    uint32_t int13_ah[256];
    uint32_t int16_ah[256];
    uint32_t int33_ax_low[256];
    uint32_t site_drops;
    MdIntProfileSite site[MD_INT_PROFILE_SITE_SLOTS];
} MdIntProfile;

const MdIntProfile *md_int_profile_counts(void);
void md_int_profile_reset(void);
#endif

typedef enum MdQmiCategory {
    MD_QMI_INTERP = 0,
    MD_QMI_M25_NATIVE,
    MD_QMI_TRANSLATE,
    MD_QMI_NATIVE_V2,
    MD_QMI_STEP,
    MD_QMI_DOS_SERVICE,
    MD_QMI_CATEGORY_COUNT
} MdQmiCategory;

#define MD_QMI_PROFILE_STACK_DEPTH 8u

typedef struct MdHooks {
    MdInterruptHook interrupt;
    MdPortIn8Hook in8;
    MdPortOut8Hook out8;
    void *user;
} MdHooks;

struct MdRuntime {
    MdX86 cpu;
    MdHooks hooks;
    uint64_t instructions;
    /* REP telemetry counts architectural REP instructions separately from
       their internal string-element work. payload_bytes counts one logical
       string element width; memory_bytes estimates guest-memory traffic
       (MOVS/CMPS touch two streams, the other string forms one). */
    uint64_t rep_instructions;
    uint64_t rep_elements;
    uint64_t rep_payload_bytes;
    uint64_t rep_memory_bytes;
    uint64_t rep_op_instructions[10];   /* A4-A7, AA-AF */
    uint64_t rep_op_elements[10];

    /* M26c exclusive QMI/XIP attribution. qmi_current is category+1; zero
       means no active scope. Nested scopes charge the parent up to the
       transition, then resume it on leave. */
    uint64_t qmi_accesses[MD_QMI_CATEGORY_COUNT];
    uint64_t qmi_misses[MD_QMI_CATEGORY_COUNT];
    uint32_t qmi_last_access;
    uint32_t qmi_last_hit;
    uint32_t qmi_stack_overflows;
    uint8_t qmi_current;
    uint8_t qmi_depth;
    uint8_t qmi_stack[MD_QMI_PROFILE_STACK_DEPTH];

    /* Subset of `instructions` retired by generated AOT code. */
    uint64_t aot_instructions;
    MdStopReason stop_reason;
    uint32_t fault_linear;
    uint8_t fault_opcode;
    uint8_t exit_code;

#if MD_INTERP_BACKEDGE_EXIT
    uint16_t native_v2_backedge_cs;
    uint16_t native_v2_backedge_ip;
    uint8_t native_v2_backedge_hit;
    uint8_t _native_v2_pad[3];
    uint32_t native_v2_suppress_bloom[2];
#if defined(MICRODOS_NATIVE_V2_BACKEDGE_PROFILE)
    uint64_t native_v2_backedge_hits;
    MdNativeV2BackedgeSite native_v2_backedge[MD_NATIVE_V2_BACKEDGE_SLOTS];
#endif
#endif

#if MICRODOS_TRANSLATION_SUPPORT
    /* code_epoch invalidates an entire external cache after image/reset events.
       Page generations handle normal self-modifying-code invalidation. */
    uint32_t code_epoch;
    uint32_t code_write_epoch;
    uint32_t code_page_generation[MD_X86_CODE_PAGE_COUNT];
    uint8_t code_page_executable[MD_X86_CODE_PAGE_COUNT];

    /* Optional caller-owned decoded cache. Generated AOT uses this when it must
       leave compiled code and execute an unknown/modified region. */
    MdBlockCache *block_cache;

    /* Attachments of generated images (M15). Cleared by init/reset, so an
       attachment can never outlive the runtime it was made for. */
    MdAotGuard aot_slots[MD_AOT_ATTACH_SLOTS];
    uint8_t aot_page_owner[MD_X86_CODE_PAGE_COUNT];   /* M21.1b, see MdX86 */
    /* M21.1b live-code bitmaps: per page a pointer into a small pool (one
       bit per byte of a 4 KiB page); pages beyond the pool use an all-ones
       bitmap, which simply means "always take the guard path". */
    uint8_t *aot_live_bits[MD_X86_CODE_PAGE_COUNT];
    uint8_t aot_live_pool[MD_AOT_LIVE_PAGES][MD_X86_CODE_PAGE_SIZE / 8u];
    uint8_t aot_live_pool_used;
    uint32_t aot_use_clock;
    uint32_t aot_evictions;

#endif
};

void md_runtime_init(MdRuntime *runtime, uint8_t *memory, const MdHooks *hooks);
void md_runtime_reset(MdRuntime *runtime);
void md_runtime_load_com(MdRuntime *runtime, const uint8_t *data, size_t size, uint16_t segment);
void md_runtime_load_raw(MdRuntime *runtime, const uint8_t *data, size_t size,
                         uint16_t segment, uint16_t offset);
void md_runtime_request_exit(MdRuntime *runtime, uint8_t exit_code);
void md_runtime_set_block_cache(MdRuntime *runtime, MdBlockCache *cache);

/* Coarse invalidation remains available for image replacement and platform code
   that cannot identify the touched page. Normal guest writes invalidate only
   executable pages through the MdX86 write helpers. */
void md_runtime_invalidate_code(MdRuntime *runtime);

/* Mark bytes as executable so later guest writes to their 4 KiB pages invalidate
   decoded blocks and, conservatively, any generated AOT using the image. */
void md_runtime_mark_code_range(MdRuntime *runtime, uint16_t segment,
                                uint16_t offset, size_t size);

/* Returns true when a host/native hook handled the interrupt. If it did not,
   the function performs real-mode 8086 interrupt-vector dispatch through the IVT. */
bool md_runtime_interrupt(MdRuntime *runtime, uint8_t vector);

/* Generated-image attachments (see MdAotGuard). An attachment is keyed by
   (program identity, segment). attach() reuses that key's slot, else a free
   slot, else evicts the least recently used one (the evicted copy simply
   runs interpreted). It arms the guard valid. find() returns the slot index
   or -1; the slot may exist but be invalid after a code write. */
int md_runtime_aot_find(const MdRuntime *runtime, const void *program, uint16_t segment);
MdAotGuard *md_runtime_aot_attach(MdRuntime *runtime, const void *program, uint16_t segment,
                                  uint32_t base, uint32_t size, const uint8_t *code_bits);
/* Marks the slot most recently used (call when entering compiled code). */
void md_runtime_aot_touch(MdRuntime *runtime, int slot);

/* M17 out-of-line helpers for compact generated code (MD_AOT_COMPACT):
   one shared copy instead of the store/guard/flag logic inlined at every
   compiled instruction. Semantics are exactly the inline versions'. */
void md_aot_store8(MdX86 *cpu, uint16_t segment, uint16_t offset, uint8_t value);
void md_aot_store16(MdX86 *cpu, uint16_t segment, uint16_t offset, uint16_t value);
void md_aot_push(MdX86 *cpu, uint16_t value);
void md_aot_push_reg(MdX86 *cpu, unsigned reg);
uint8_t md_aot_alu8(MdX86 *cpu, unsigned operation, uint8_t lhs, uint8_t rhs);
uint16_t md_aot_alu16(MdX86 *cpu, unsigned operation, uint16_t lhs, uint16_t rhs);
uint8_t md_aot_shift8(MdX86 *cpu, unsigned operation, uint8_t value, unsigned count);
uint16_t md_aot_shift16(MdX86 *cpu, unsigned operation, uint16_t value, unsigned count);
uint8_t md_aot_incdec8(MdX86 *cpu, uint8_t value, int dec);     /* CF preserved */
uint16_t md_aot_incdec16(MdX86 *cpu, uint16_t value, int dec);  /* CF preserved */
int md_aot_condition(const MdX86 *cpu, unsigned cc);
int md_aot_chunks_ok_ol(const MdAotGuard *guard, uint32_t first, uint32_t last);

/* M18: interpreter semantics exported for compiled code, so the generator
   never re-implements them. `ext` is ModR/M.reg (4 MUL .. 7 IDIV); a divide
   fault stops the runtime exactly as the interpreter does. The string op
   takes the raw prefix bytes (0 = none; 26/2E/36/3E; F2/F3). */
void md_interp_muldiv(MdRuntime *runtime, uint8_t opcode, unsigned ext, uint16_t operand,
                      uint16_t ip_before);
void md_interp_string_op(MdRuntime *runtime, uint8_t opcode, uint8_t segment_prefix,
                         uint8_t repeat_prefix);

#if MD_INTERP_OPCODE_PROFILE
const uint32_t *md_interp_opcode_profile_counts(void);
const uint32_t *md_interp_unpref_modrm_profile_counts(void);
const uint32_t *md_interp_prefix_profile_counts(void);
const uint32_t *md_interp_hot_modrm_profile_counts(void);
void md_interp_opcode_profile_reset(void);
#endif
MdStopReason md_interp_step(MdRuntime *runtime);
MdStopReason md_interp_run(MdRuntime *runtime, uint64_t instruction_budget);
/* Like md_interp_run(), but also returns (with MD_STOP_NONE) as soon as CS
   changes: far jumps/calls/returns, INT and IRET. The DOS system loop uses it
   to run the kernel and non-compiled programs at threaded speed while still
   stopping wherever compiled code could take over (M16). */
MdStopReason md_interp_run_until_cs_change(MdRuntime *runtime, uint64_t instruction_budget);
const char *md_stop_reason_name(MdStopReason reason);

#ifdef __cplusplus
}
#endif

#endif
