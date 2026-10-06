#ifndef MICRODOS_QMI_PROFILE_H
#define MICRODOS_QMI_PROFILE_H

#include "microdos/runtime.h"

/*
 * M26c RP2350 QMI/XIP attribution.
 *
 * Scopes are exclusive. Entering a nested scope first charges the current
 * category up to the transition point, then switches category. Leaving the
 * nested scope charges it and resumes the parent category. This lets a DOS
 * interrupt be separated from the interpreter that invoked it, and a K_STEP
 * helper be separated from the generated M25 episode that called it.
 */
#if defined(PICO_BUILD) && defined(MICRODOS_PICO_QMI_PROFILE) && MICRODOS_PICO_QMI_PROFILE
#include "hardware/structs/xip_ctrl.h"

static inline void md_qmi_profile_charge(MdRuntime *rt, uint32_t acc, uint32_t hit)
{
    uint32_t da, dh, dm;
    unsigned cat;
    if (rt == NULL || rt->qmi_current == 0u) return;
    da = (uint32_t)(acc - rt->qmi_last_access);
    dh = (uint32_t)(hit - rt->qmi_last_hit);
    dm = da >= dh ? da - dh : 0u;
    cat = (unsigned)rt->qmi_current - 1u;
    if (cat < MD_QMI_CATEGORY_COUNT) {
        rt->qmi_accesses[cat] += da;
        rt->qmi_misses[cat] += dm;
    }
}

static inline void md_qmi_profile_enter(MdRuntime *rt, MdQmiCategory category)
{
    const uint32_t acc = xip_ctrl_hw->ctr_acc;
    const uint32_t hit = xip_ctrl_hw->ctr_hit;
    if (rt == NULL || (unsigned)category >= MD_QMI_CATEGORY_COUNT) return;
    if (rt->qmi_depth >= MD_QMI_PROFILE_STACK_DEPTH) {
        ++rt->qmi_stack_overflows;
        return;
    }
    md_qmi_profile_charge(rt, acc, hit);
    rt->qmi_stack[rt->qmi_depth++] = rt->qmi_current;
    rt->qmi_current = (uint8_t)((unsigned)category + 1u);
    rt->qmi_last_access = acc;
    rt->qmi_last_hit = hit;
}

static inline void md_qmi_profile_leave(MdRuntime *rt)
{
    const uint32_t acc = xip_ctrl_hw->ctr_acc;
    const uint32_t hit = xip_ctrl_hw->ctr_hit;
    if (rt == NULL || rt->qmi_depth == 0u) return;
    md_qmi_profile_charge(rt, acc, hit);
    rt->qmi_current = rt->qmi_stack[--rt->qmi_depth];
    rt->qmi_last_access = acc;
    rt->qmi_last_hit = hit;
}
#else
static inline void md_qmi_profile_enter(MdRuntime *rt, MdQmiCategory category)
{
    (void)rt; (void)category;
}
static inline void md_qmi_profile_leave(MdRuntime *rt)
{
    (void)rt;
}
#endif

#endif /* MICRODOS_QMI_PROFILE_H */
