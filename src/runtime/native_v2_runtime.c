#include "microdos/native_v2_runtime.h"

#include <string.h>

_Static_assert(MD_NATIVE_V2_RT_SLOTS > 0u &&
               (MD_NATIVE_V2_RT_SLOTS & (MD_NATIVE_V2_RT_SLOTS - 1u)) == 0u,
               "Native v2 runtime slots must be a power of two");

static unsigned md_nv2_rt_slot_index(uint16_t cs, uint16_t ip)
{
    return (unsigned)(((uint32_t)cs * 33u + ip) &
                      (MD_NATIVE_V2_RT_SLOTS - 1u));
}

static uint32_t md_nv2_rt_signature(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0u; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static size_t md_nv2_rt_window(const MdX86 *cpu, uint16_t ip, uint32_t linear)
{
    size_t n = MD_NATIVE_V2_RT_MAX_GUEST_BYTES;
    const size_t to_ip_wrap = (size_t)(0x10000u - ip);
    const size_t to_phys_wrap = (size_t)(MD_X86_ADDRESS_SPACE - linear);

    if (n > to_ip_wrap) n = to_ip_wrap;
    if (n > to_phys_wrap) n = to_phys_wrap;
    return n;
}


static size_t md_nv2_rt_span_bytes(const MdNativeV2Code *code,
                                   uint16_t fallback_size)
{
    size_t total = 0u;
    unsigned i;

    if (code->guest_span_count == 0u)
        return fallback_size;

    if (code->guest_span_count > 4u)
        return 0u;

    for (i = 0u; i < code->guest_span_count; ++i) {
        if (code->guest_span_len[i] == 0u)
            return 0u;
        total += code->guest_span_len[i];
    }
    return total;
}

static int md_nv2_rt_code_matches(const MdX86 *cpu,
                                  const MdNativeV2RuntimeSlot *slot)
{
    size_t at = 0u;
    unsigned i;

    if (slot->code.guest_span_count == 0u) {
        const uint32_t linear = md_x86_linear(slot->cs, slot->ip);
        if (slot->guest_size > MD_NATIVE_V2_RT_MAX_GUEST_BYTES)
            return 0;
        return memcmp(cpu->memory + linear,
                      slot->guest_bytes,
                      slot->guest_size) == 0;
    }

    if (slot->code.guest_span_count > 4u)
        return 0;

    for (i = 0u; i < slot->code.guest_span_count; ++i) {
        const uint16_t ip = slot->code.guest_span_ip[i];
        const unsigned len = slot->code.guest_span_len[i];
        const uint32_t linear = md_x86_linear(slot->cs, ip);
        unsigned j;

        if (at + len > MD_NATIVE_V2_RT_MAX_GUEST_BYTES)
            return 0;

        for (j = 0u; j < len; ++j) {
            const uint32_t phys =
                (linear + j) & (MD_X86_ADDRESS_SPACE - 1u);
            if (cpu->memory[phys] != slot->guest_bytes[at + j])
                return 0;
        }
        at += len;
    }

    return 1;
}

static int md_nv2_rt_capture_code(const MdX86 *cpu,
                                  MdNativeV2RuntimeSlot *slot)
{
    size_t at = 0u;
    unsigned i;

    if (slot->code.guest_span_count == 0u) {
        const uint32_t linear = md_x86_linear(slot->cs, slot->ip);
        if (slot->guest_size > MD_NATIVE_V2_RT_MAX_GUEST_BYTES)
            return 0;
        memcpy(slot->guest_bytes, cpu->memory + linear, slot->guest_size);
        return 1;
    }

    if (slot->code.guest_span_count > 4u)
        return 0;

    for (i = 0u; i < slot->code.guest_span_count; ++i) {
        const uint16_t ip = slot->code.guest_span_ip[i];
        const unsigned len = slot->code.guest_span_len[i];
        const uint32_t linear = md_x86_linear(slot->cs, ip);
        unsigned j;

        if (at + len > MD_NATIVE_V2_RT_MAX_GUEST_BYTES)
            return 0;

        for (j = 0u; j < len; ++j) {
            const uint32_t phys =
                (linear + j) & (MD_X86_ADDRESS_SPACE - 1u);
            slot->guest_bytes[at + j] = cpu->memory[phys];
        }
        at += len;
    }

    return 1;
}

static int md_nv2_rt_range_overlaps_code(const MdNativeV2RuntimeSlot *slot,
                                          uint32_t data_start,
                                          uint32_t data_end)
{
    unsigned i;

    if (slot->code.guest_span_count == 0u) {
        const uint32_t code_start = md_x86_linear(slot->cs, slot->ip);
        const uint32_t code_end = code_start + slot->guest_size - 1u;
        return !(data_end < code_start || data_start > code_end);
    }

    for (i = 0u; i < slot->code.guest_span_count; ++i) {
        const unsigned len = slot->code.guest_span_len[i];
        const uint32_t code_start =
            md_x86_linear(slot->cs, slot->code.guest_span_ip[i]);
        uint32_t code_end;

        if (len == 0u || code_start + len > MD_X86_ADDRESS_SPACE)
            return 1;

        code_end = code_start + len - 1u;
        if (!(data_end < code_start || data_start > code_end))
            return 1;
    }

    return 0;
}

static void md_nv2_rt_reject(MdNativeV2RuntimeSlot *slot,
                             uint16_t cs,
                             uint16_t ip,
                             uint32_t signature,
                             unsigned reason,
                             MdNativeV2Status status,
                             const uint8_t *guest,
                             size_t window)
{
    size_t n = window;

    if (n > MD_NATIVE_V2_REJECT_BYTES)
        n = MD_NATIVE_V2_REJECT_BYTES;

    memset(slot, 0, sizeof(*slot));
    slot->cs = cs;
    slot->ip = ip;
    slot->state = MD_NV2_RT_REJECTED;
    slot->reject_signature = signature;
    slot->reject_reason = (uint8_t)reason;
    slot->reject_status = (uint8_t)status;
    slot->reject_bytes_len = (uint8_t)n;

    if (n != 0u)
        memcpy(slot->reject_bytes, guest, n);
}

void md_native_v2_runtime_init(MdNativeV2Runtime *runtime)
{
    if (runtime != NULL)
        memset(runtime, 0, sizeof(*runtime));
}

bool md_native_v2_runtime_try_execute(MdNativeV2Runtime *runtime,
                                      MdRuntime *machine,
                                      uint64_t budget,
                                      MdNativeV2RunResult *result)
{
    MdX86 *cpu;
    MdNativeV2RuntimeSlot *slot;
    const uint8_t *guest;
    uint32_t linear;
    uint32_t signature;
    int preserve_compiled = 0;
    uint64_t iterations;
    uint64_t run_iterations;
    uint64_t retired;
    uint64_t max_retired;
    uint64_t prefix_retired = 0u;
    int chunked = 0;
    uint32_t native_rc;
    size_t window;
    unsigned index;
    uint16_t request_ip;
    uint16_t candidate_ip;
    uint16_t saved_counter = 0u;
    uint8_t zero_counter = 0u;
    uint8_t prefix_ops = 0u;

    if (result != NULL)
        memset(result, 0, sizeof(*result));

    if (runtime == NULL || machine == NULL || budget == 0u)
        return false;

    cpu = &machine->cpu;
    if (cpu->memory == NULL)
        return false;

    ++runtime->lookups;

    request_ip = cpu->ip;
    candidate_ip = request_ip;

    /*
     * Phase 3N: event-driven admission reaches the Phase-4 outer header
     * (XOR CX,CX) rather than the inner CALL/LOOP header. Recognize that
     * one-shot prelude without consuming it yet, then key/cache the native
     * graph at the real CALL entry. If compilation or any guard later fails,
     * cpu->ip and CX are still untouched and the interpreter executes the
     * original XOR normally.
     */
    (void)md_native_v2_find_local_call_loop_entry(
        cpu->memory, cpu->cs, request_ip,
        &candidate_ip, &prefix_ops, &zero_counter);
    prefix_retired = prefix_ops;

    linear = md_x86_linear(cpu->cs, candidate_ip);
    window = md_nv2_rt_window(cpu, candidate_ip, linear);
    if (window < 3u)
        return false;

    guest = cpu->memory + linear;
    signature = md_nv2_rt_signature(guest, window);

    index = md_nv2_rt_slot_index(cpu->cs, candidate_ip);
    slot = &runtime->slot[index];

    if (slot->state != MD_NV2_RT_EMPTY &&
        slot->cs == cpu->cs &&
        slot->ip == candidate_ip) {
        if (slot->state == MD_NV2_RT_REJECTED) {
            if (slot->reject_signature == signature) {
                ++runtime->rejected_hits;
                return false;
            }
            memset(slot, 0, sizeof(*slot));
            ++runtime->stale_code;
        } else if (slot->state == MD_NV2_RT_COMPILED) {
            if (md_nv2_rt_code_matches(cpu, slot)) {
                ++runtime->cache_hits;
                goto execute;
            }
            memset(slot, 0, sizeof(*slot));
            ++runtime->stale_code;
        }
    } else {
        ++runtime->cache_misses;

        /*
         * A cold unsupported candidate must not evict a useful compiled
         * region merely because both hash to the same direct slot. Preserve
         * the compiled resident while probing; replace it only when the new
         * candidate itself compiles successfully.
         */
        preserve_compiled = slot->state == MD_NV2_RT_COMPILED;
        if (!preserve_compiled)
            memset(slot, 0, sizeof(*slot));
    }

    ++runtime->probes;

    {
        MdNativeV2Code code;
        MdNativeV2Status st;
        size_t guest_size = 0u;
        uint8_t counter_reg = 0xFFu;

        memset(&code, 0, sizeof(code));

        st = md_native_v2_compile_counted_loop(
            guest, window, candidate_ip,
            &code, &guest_size, &counter_reg);

        if (st != MD_NATIVE_V2_OK) {
            st = md_native_v2_compile_local_call_loop(
                cpu->memory, cpu->cs, candidate_ip,
                &code, &counter_reg);
            if (st == MD_NATIVE_V2_OK) {
                guest_size = code.guest_span_count != 0u
                    ? code.guest_span_len[0]
                    : 0u;
            }
        }

        if (st != MD_NATIVE_V2_OK) {
            st = md_native_v2_compile_rep_string_loop(
                cpu->memory, cpu->cs, candidate_ip,
                &code, &counter_reg);
            if (st == MD_NATIVE_V2_OK)
                guest_size = 46u;
        }

        if (st != MD_NATIVE_V2_OK ||
            guest_size == 0u ||
            guest_size > MD_NATIVE_V2_RT_MAX_GUEST_BYTES ||
            md_nv2_rt_span_bytes(&code, (uint16_t)guest_size) == 0u ||
            md_nv2_rt_span_bytes(&code, (uint16_t)guest_size) >
                MD_NATIVE_V2_RT_MAX_GUEST_BYTES) {
            if (!preserve_compiled) {
                md_nv2_rt_reject(slot,
                                 cpu->cs, candidate_ip, signature,
                                 MD_NV2_REJECT_COMPILE, st,
                                 guest, window);
            }
            ++runtime->compile_rejects;
            return false;
        }

        if (code.has_store &&
            !code.safe_store_bx_si_loop &&
            !code.safe_stack_pushpop &&
            !code.local_call_graph &&
            !code.safe_rep_string_loop) {
            if (!preserve_compiled) {
                md_nv2_rt_reject(slot,
                                 cpu->cs, candidate_ip, signature,
                                 MD_NV2_REJECT_STORE, MD_NATIVE_V2_OK,
                                 guest, guest_size);
            }
            ++runtime->store_rejects;
            return false;
        }

        memset(slot, 0, sizeof(*slot));
        slot->code = code;
        slot->cs = cpu->cs;
        slot->ip = candidate_ip;
        slot->guest_size = (uint16_t)guest_size;
        slot->counter_reg = counter_reg;
        slot->state = MD_NV2_RT_COMPILED;
        if (!md_nv2_rt_capture_code(cpu, slot)) {
            memset(slot, 0, sizeof(*slot));
            ++runtime->compile_rejects;
            return false;
        }
        ++runtime->compiles;
    }

execute:
    /*
     * The redirected Phase-4 entry has architecturally executed XOR CX,CX
     * before reaching the cached CALL graph. Do not mutate the machine yet:
     * reserve that one retirement and use the implied 65,536 LOOP count for
     * scheduling. State is committed only immediately before native entry.
     */
    if (zero_counter) {
        iterations = 65536u;
    } else {
        iterations = cpu->r[slot->counter_reg & 7u];
        if (iterations == 0u)
            iterations = 65536u;
    }

    run_iterations = iterations;
    max_retired = prefix_retired +
        run_iterations * (uint64_t)slot->code.op_count;

    /*
     * Phase 3K: E2 LOOP itself does not modify x86 FLAGS. When the body
     * does not read CX, execute only the number of iterations that fit the
     * current scheduler budget. After native return, restore the real
     * remaining CX and put IP back at the loop header.
     */
    if (max_retired > budget &&
        ((slot->code.chunkable_loop == 1u &&
          slot->code.loop_terminal == 0xE2u) ||
         (slot->code.chunkable_loop == 2u &&
          slot->code.loop_terminal == 0x75u))) {
        if (budget > prefix_retired) {
            run_iterations =
                (budget - prefix_retired) / (uint64_t)slot->code.op_count;
        } else {
            run_iterations = 0u;
        }
        if (run_iterations > iterations)
            run_iterations = iterations;
        if (run_iterations != 0u) {
            max_retired = prefix_retired +
                run_iterations * (uint64_t)slot->code.op_count;
            chunked = run_iterations < iterations;
        }
    }

    if (max_retired < MD_NATIVE_V2_RT_MIN_RETIRE) {
        ++runtime->short_rejects;
        return false;
    }

    if (max_retired > budget) {
        ++runtime->budget_rejects;
        return false;
    }

    if (slot->code.safe_store_bx_si_loop) {
        const uint32_t start_off =
            (uint16_t)(cpu->r[MD_X86_BX] + cpu->r[MD_X86_SI]);
        const uint64_t byte_count = run_iterations * 2u;
        const uint64_t end_off_exclusive =
            (uint64_t)start_off + byte_count;
        const uint32_t code_start = md_x86_linear(cpu->cs, cpu->ip);
        const uint32_t code_end =
            code_start + (uint32_t)slot->guest_size - 1u;
        uint32_t data_start;
        uint32_t data_end;

        if (!slot->code.safe_store_bx_si_loop ||
            cpu->ds > 0xEFFFu ||
            (start_off & 1u) != 0u ||
            end_off_exclusive > 0x10000u) {
            ++runtime->store_guard_rejects;
            return false;
        }

        data_start = md_x86_linear(cpu->ds, (uint16_t)start_off);
        data_end = data_start + (uint32_t)byte_count - 1u;

        if (!(data_end < code_start || data_start > code_end)) {
            ++runtime->store_guard_rejects;
            return false;
        }
    }

    /*
     * Phase 3P REP/string direct stores. The compiler records the two word
     * spans written by STOSW and MOVSW. Runtime proves the exact measured
     * segment relation (DS==ES), forward/non-wrapping ranges, and that neither
     * write span overlaps the guest bytes currently executing natively.
     * Other compiled regions remain SMC-safe because every Native-v2 re-entry
     * validates its complete guest byte image.
     */
    if (slot->code.safe_rep_string_loop) {
        const uint64_t byte_count =
            (uint64_t)slot->code.rep_words * 2u;
        const uint64_t src_end_exclusive =
            (uint64_t)slot->code.rep_src_off + byte_count;
        const uint64_t dst_end_exclusive =
            (uint64_t)slot->code.rep_dst_off + byte_count;
        const uint32_t code_start =
            md_x86_linear(cpu->cs, slot->ip);
        const uint32_t code_end =
            code_start + (uint32_t)slot->guest_size - 1u;
        uint32_t src_start;
        uint32_t src_end;
        uint32_t dst_start;
        uint32_t dst_end;

        if (slot->code.rep_words == 0u ||
            cpu->ds != cpu->es ||
            cpu->ds > 0xEFFFu ||
            src_end_exclusive > 0x10000u ||
            dst_end_exclusive > 0x10000u) {
            ++runtime->store_guard_rejects;
            return false;
        }

        src_start = md_x86_linear(cpu->es, slot->code.rep_src_off);
        src_end = src_start + (uint32_t)byte_count - 1u;
        dst_start = md_x86_linear(cpu->es, slot->code.rep_dst_off);
        dst_end = dst_start + (uint32_t)byte_count - 1u;

        if (!(src_end < code_start || src_start > code_end) ||
            !(dst_end < code_start || dst_start > code_end)) {
            ++runtime->store_guard_rejects;
            return false;
        }
    }

    /*
     * Stack-writing regions have a different proof from streaming DS stores.
     * Do not route PUSHF/POPF through the [BX+SI] store-range guard above.
     */
    if (slot->code.safe_stack_pushpop) {
        const uint16_t push_sp =
            (uint16_t)(cpu->r[MD_X86_SP] - 2u);
        const uint32_t data_start =
            md_x86_linear(cpu->ss, push_sp);
        const uint32_t data_end = data_start + 1u;
        const uint32_t code_start =
            md_x86_linear(cpu->cs, slot->ip);
        const uint32_t code_end =
            code_start + (uint32_t)slot->guest_size - 1u;

        if (cpu->ss > 0xEFFFu ||
            !(data_end < code_start || data_start > code_end)) {
            ++runtime->stack_guard_rejects;
            return false;
        }
    }


    /*
     * Phase 3M CALL/RET graphs use a bounded SS:SP window whose guest writes
     * are distinct from both the streaming DS-store proof and PUSHF/POPF.
     * The first graph has a maximum depth of 16 bytes. Reject wrap and any
     * overlap with any caller/callee byte span before entering native code.
     */
    if (slot->code.local_call_graph) {
        const unsigned depth = slot->code.call_stack_bytes;
        uint16_t low_sp;
        uint32_t data_start;
        uint32_t data_end;

        if (depth == 0u || cpu->ss > 0xEFFFu ||
            cpu->r[MD_X86_SP] < depth) {
            ++runtime->stack_guard_rejects;
            return false;
        }

        low_sp = (uint16_t)(cpu->r[MD_X86_SP] - depth);
        data_start = md_x86_linear(cpu->ss, low_sp);
        data_end = data_start + depth - 1u;

        if (data_end >= MD_X86_ADDRESS_SPACE ||
            md_nv2_rt_range_overlaps_code(slot, data_start, data_end)) {
            ++runtime->stack_guard_rejects;
            return false;
        }
    }

    if (slot->code.requires_safe_muldiv) {
        const uint16_t multiplier =
            cpu->r[slot->code.muldiv_mul_reg & 7u];
        const uint16_t divisor =
            cpu->r[slot->code.muldiv_div_reg & 7u];

        if (divisor == 0u || multiplier >= divisor) {
            ++runtime->muldiv_guard_rejects;
            return false;
        }
    }

    saved_counter = cpu->r[slot->counter_reg & 7u];

    if (zero_counter) {
        /* Commit the skipped XOR CX,CX only after every admission guard. */
        cpu->r[slot->counter_reg & 7u] = 0u;
    }

    if (chunked) {
        cpu->r[slot->counter_reg & 7u] =
            run_iterations == 65536u ? 0u : (uint16_t)run_iterations;
    }

    native_rc = md_native_v2_execute(cpu, &slot->code);
    if (native_rc == MD_NATIVE_V2_EXEC_FALLBACK) {
        if (chunked || zero_counter)
            cpu->r[slot->counter_reg & 7u] = saved_counter;
        cpu->ip = request_ip;
        ++runtime->runtime_fallbacks;
        return false;
    }

    if (slot->code.dynamic_retire) {
        const uint64_t optional_max =
            run_iterations *
            (uint64_t)(slot->code.op_count - slot->code.retire_base_ops);

        if ((uint64_t)native_rc > optional_max) {
            ++runtime->runtime_fallbacks;
            return false;
        }

        retired = prefix_retired +
            run_iterations * (uint64_t)slot->code.retire_base_ops +
            (uint64_t)native_rc;
    } else {
        retired = max_retired;
    }

    if (chunked) {
        const uint64_t remaining = iterations - run_iterations;
        const uint16_t remaining16 =
            remaining == 65536u ? 0u : (uint16_t)remaining;

        cpu->r[slot->counter_reg & 7u] = remaining16;
        cpu->ip = slot->ip;

        if (slot->code.chunkable_loop == 2u) {
            /*
             * The native chunk used a temporary counter equal to the number
             * of iterations in this scheduler slice, so its terminal DEC
             * naturally ended at zero. Architecturally, however, stopping
             * after the same number of real iterations leaves COUNTER at
             * `remaining`, and FLAGS are those from:
             *
             *     DEC (remaining + 1) -> remaining
             *
             * DEC preserves CF; lazy_carry emitted by the native region is
             * already the correct CF from the final body iteration.
             */
            cpu->lazy_op = MD_LAZY_DEC16;
            cpu->lazy_a = (uint16_t)(remaining16 + 1u);
            cpu->lazy_b = 1u;
            cpu->lazy_res = remaining16;
        }

        ++runtime->chunked_entries;
        runtime->chunked_iterations += run_iterations;
    }

    machine->instructions += retired;
    ++runtime->entries;
    runtime->retired += retired;
    ++slot->entries;
    slot->retired += retired;

    if (result != NULL) {
        result->retired = retired;
        result->iterations = (uint32_t)run_iterations;
        result->cs = slot->cs;
        result->ip = slot->ip;
        result->entered = 1u;
    }

    return true;
}

const char *md_native_v2_reject_reason_name(unsigned reason)
{
    switch (reason) {
        case MD_NV2_REJECT_COMPILE: return "compile";
        case MD_NV2_REJECT_STORE: return "store";
        default: return "none";
    }
}
