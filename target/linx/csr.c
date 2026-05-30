/*
 * LINX Control and Status Registers.
 *
 * Copyright (c) 2022 HiSilicon Technologies.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "qemu/main-loop.h"
#include "exec/exec-all.h"
#include "hw/intc/lxintc.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "fpu/softfloat.h"

/* CSR function table public API */

static LINXException any(CPULINXState *env, int ssrno)
{
    return LINX_EXCP_NONE;
}

#if !defined(CONFIG_USER_ONLY)
static LINXException acr1(CPULINXState *env, int ssrno)
{
    /* FIXME: Check whether the acr1 privilege state is supported.
     * Currently, the acr1 privilege state is supported by default.
     */
    return LINX_EXCP_NONE;
}

#endif

#if defined(CONFIG_USER_ONLY)

#else /* CONFIG_USER_ONLY */

/* Machine constants */

#define ACR_MODE_INTERRUPTS  (IPENDING_EI | IPENDING_TI | IPENDING_SI)

static const target_ulong delegable_ints = ACR_MODE_INTERRUPTS;

static LINXException read_ecstate(CPULINXState *env, int ssrno,
                                        target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].ecstate;

    return LINX_EXCP_NONE;
}

static LINXException write_ecstate(CPULINXState *env, int ssrno,
                                    target_ulong val)
{
    uint64_t acr;
    uint64_t *pvalue = NULL;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    pvalue = &env->sysreg[acr].ecstate;

    *pvalue = (*pvalue & ~ECSTATE_MASK) | (val & ECSTATE_MASK);
    return LINX_EXCP_NONE;
}

static LINXException read_lxlcid(CPULINXState *env, int ssrno,
                                   target_ulong *val)
{
    *val = env->lxlcid;
    return LINX_EXCP_NONE;
}

static LINXException read_cstate(CPULINXState *env, int ssrno,
                                        target_ulong *val)
{
    *val = env->cstate;
    return LINX_EXCP_NONE;
}

static LINXException write_cstate(CPULINXState *env, int ssrno,
                                    target_ulong val)
{
    uint64_t mask = 0;
    uint64_t new_cstate;
    target_ulong newpriv;
    mask = CSTATE_IE | CSTATE_PERMIT | CSTATE_ACR;
    new_cstate = (env->cstate & ~mask) | (val & mask);
    newpriv = get_field(new_cstate, CSTATE_ACR);
    if (newpriv != env->priv) {
        linx_cpu_set_mode(env, newpriv);
    }
    env->cstate = (env->cstate & ~mask) | (new_cstate & mask);
    return LINX_EXCP_NONE;
}

static LINXException read_ebpc(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].ebpc;
    return LINX_EXCP_NONE;
}

static LINXException write_ebpc(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    env->sysreg[acr].ebpc = val;

    return LINX_EXCP_NONE;
}

static LINXException read_ebarg(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].ebarg;
    return LINX_EXCP_NONE;
}

static LINXException write_ebarg(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    uint64_t mask = 0;
    uint64_t *pvalue = NULL;
    pvalue = &env->sysreg[acr].ebarg;
    mask = EBARG_GROUPID | EBARG_RL | EBARG_AQ | EBARG_TAKEN |
           EBARG_TYPE | EBARG_BLOCKTYPE | EBARG_REGDST0 | EBARG_REGDST1 |
           EBARG_REGDST2 | EBARG_REGDST3;

    *pvalue = (*pvalue & ~mask) | (val & mask);
    return LINX_EXCP_NONE;
}

static LINXException read_etpc(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].etpc;
    return LINX_EXCP_NONE;
}

static LINXException write_etpc(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    env->sysreg[acr].etpc = val;

    return LINX_EXCP_NONE;
}

static LINXException read_ebpcn(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].ebpcn;
    return LINX_EXCP_NONE;
}

static LINXException write_ebpcn(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    env->sysreg[acr].ebpcn = val;

    return LINX_EXCP_NONE;
}

static LINXException read_ebarg_group(CPULINXState *env, int ssrno,
                                      target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    int slot = ssrno & 0x0fff;
    LinxSYSReg *sysreg = &env->sysreg[acr];

#ifndef CONFIG_USER_ONLY
    if (acr == env->priv && slot >= 0xF40 && slot <= 0xF5F) {
        *val = linx_read_live_ebarg_group(env, acr, slot);
        return LINX_EXCP_NONE;
    }
#endif

    switch (slot) {
    case 0xF40:
        *val = sysreg->ebarg;
        break;
    case 0xF41:
        *val = sysreg->ebpc;
        break;
    case 0xF42:
        *val = sysreg->ebpcn;
        break;
    case 0xF43:
        *val = sysreg->etpc;
        break;
    case 0xF51:
        *val = sysreg->ebarg_tplflags;
        break;
    default:
        *val = sysreg->elpr[slot & 0x1f];
        break;
    }

    return LINX_EXCP_NONE;
}

static LINXException write_ebarg_group(CPULINXState *env, int ssrno,
                                       target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    int slot = ssrno & 0x0fff;
    LinxSYSReg *sysreg = &env->sysreg[acr];

#ifndef CONFIG_USER_ONLY
    if (acr == env->priv && slot >= 0xF40 && slot <= 0xF5F) {
        linx_write_live_ebarg_group(env, acr, slot, val);
        return LINX_EXCP_NONE;
    }
#endif

    switch (slot) {
    case 0xF40:
        return write_ebarg(env, ssrno, val);
    case 0xF41:
        sysreg->ebpc = val;
        break;
    case 0xF42:
        sysreg->ebpcn = val;
        break;
    case 0xF43:
        sysreg->etpc = val;
        break;
    case 0xF51:
        sysreg->ebarg_tplflags = val;
        break;
    default:
        sysreg->elpr[slot & 0x1f] = val;
        break;
    }

    return LINX_EXCP_NONE;
}

static LINXException read_evbase(CPULINXState *env, int ssrno,
                                        target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    *val = env->sysreg[acr].evbase;
    return LINX_EXCP_NONE;
}

static LINXException write_evbase(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    uint64_t acr = get_field(ssrno, SYSREG_ADDR_ACR);

    if (g_getenv("LINX_LOG_MMU_CSR")) {
        fprintf(stderr,
                "LINX_EVBASE_WRITE pc=%#" PRIx64 " acr=%" PRIu64
                " old=%#" PRIx64 " new=%#" PRIx64 "\n",
                (uint64_t)env->pc, acr, (uint64_t)env->sysreg[acr].evbase,
                (uint64_t)(val & EVBASE_BASE));
    }
    env->sysreg[acr].evbase = val & EVBASE_BASE;

    return LINX_EXCP_NONE;
}

static LINXException read_ecause(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].ecause;
    return LINX_EXCP_NONE;
}

static LINXException write_ecause(CPULINXState *env, int ssrno,
                                       target_ulong val)
{
    uint64_t acr, mask = 0;
    mask = ECAUSE_E | ECAUSE_SYNDROME | ECAUSE_TRAPNUM;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);

    env->sysreg[acr].ecause = val & mask;
    return LINX_EXCP_NONE;
}

static LINXException read_earg0(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].earg0;
    return LINX_EXCP_NONE;
}

static LINXException write_earg0(CPULINXState *env, int ssrno,
                                       target_ulong val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    env->sysreg[acr].earg0 = val;
    return LINX_EXCP_NONE;
}


static LINXException read_etemp(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    *val = env->sysreg[acr].etemp;
    return LINX_EXCP_NONE;

}

static LINXException write_etemp(CPULINXState *env, int ssrno,
                                    target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    env->sysreg[acr].etemp = val;
    return LINX_EXCP_NONE;
}

static LINXException read_futo(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    *val = env->sysreg[acr].futo;
    return LINX_EXCP_NONE;

}

static LINXException write_futo(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    env->sysreg[acr].futo = val;

    return LINX_EXCP_NONE;
}

static LINXException read_econfig(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    *val = env->sysreg[acr].econfig;
    return LINX_EXCP_NONE;

}

static LINXException write_econfig(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    uint64_t *pvalue = NULL, mask;
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    mask = ECONFIG_EXT | ECONFIG_TIMER | ECONFIG_SOFT | ECONFIG_V | ECONFIG_C;
    pvalue = &env->sysreg[acr].econfig;

    *pvalue = (*pvalue & ~mask) | (val & mask);

    return LINX_EXCP_NONE;
}

static LINXException rmw_ipending_acr0(CPULINXState *env, int ssrno,
                                       target_ulong *ret_value,
                                       target_ulong new_value,
                                       target_ulong write_mask)
{
    LINXCPU *cpu = env_archcpu(env);
    /* Allow software control of delegable interrupts not claimed by hardware */
    target_ulong mask = 0;
    uint32_t old_ipending;

    if (mask) {
        old_ipending = linx_cpu_update_ipending(cpu, mask,
                                                 (new_value & mask), ACR0);
    } else {
        old_ipending = env->sysreg[ACR0].ipending;
    }

    if (ret_value) {
        *ret_value = old_ipending;
    }

    return LINX_EXCP_NONE;
}

static LINXException rmw_ipending_acr1(CPULINXState *env, int ssrno,
                                       target_ulong *ret_value,
                                       target_ulong new_value,
                                       target_ulong write_mask)
{
    LINXCPU *cpu = env_archcpu(env);
    /* fix: need to do add topie */
    target_ulong mask = write_mask & delegable_ints;
    uint32_t old_ipending;

    if (mask) {
        old_ipending = linx_cpu_update_ipending(cpu, mask,
                                                 (new_value & mask), ACR1);
    } else {
        old_ipending = env->sysreg[ACR1].ipending;
    }

    if (ret_value) {
        *ret_value = old_ipending;
    }

    return LINX_EXCP_NONE;
}

static LINXException read_mmtbase(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    *val = env->sysreg[acr].mmtbase;

    return LINX_EXCP_NONE;
}

static const char valid_vm_1_10_64[16] = {
    [VM_0_VA36_VA39] = 1,
    [VM_1_VA44_VA48] = 1,
    [VM_2_VA52_VA57] = 1,
};

static int validate_vm(target_ulong vm, target_ulong mode)
{
    if (mode == 0) return 1;
    return valid_vm_1_10_64[vm & 0x3];
}

static LINXException write_mmtbase(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    target_ulong vm, mask, mode;
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    target_ulong old = env->sysreg[acr].mmtbase;
    const unsigned int mmtbase_field_shift = 2;
    target_ulong raw_replay_encoded = val >> (PGSHIFT - mmtbase_field_shift);

    if (g_getenv("LINX_LOG_MMU_CSR")) {
        fprintf(stderr,
                "LINX_MMTBASE_WRITE pc=%#" PRIx64 " acr=%d old=%#" PRIx64
                " new=%#" PRIx64 "\n",
                (uint64_t)env->pc, acr, (uint64_t)old,
                (uint64_t)val);
    }

    /*
     * Low-head stale re-entry can replay the same ACR1 page-table root using
     * the raw physical pointer form instead of the architectural encoded
     * MMTBASE form. If MMU is already enabled and that raw replay normalizes
     * back to the current active root, ignore it rather than blowing away the
     * live translation base.
     */
    if ((env->sysreg[acr].mmconfig & MMCONFIG_EN) &&
        env->pc == 0x2e8 &&
        old != 0 &&
        val == (old << (PGSHIFT - mmtbase_field_shift)) &&
        raw_replay_encoded == old) {
        if (g_getenv("LINX_LOG_MMU_CSR")) {
            fprintf(stderr,
                    "LINX_MMTBASE_WRITE_IGNORED pc=%#" PRIx64
                    " acr=%d raw=%#" PRIx64 " encoded=%#" PRIx64 "\n",
                    (uint64_t)env->pc, acr, (uint64_t)val,
                    (uint64_t)raw_replay_encoded);
        }
        return LINX_EXCP_NONE;
    }

    mode = get_field(old, MMTBASE_CNT);
    vm = validate_vm(get_field(val, MMTBASE_CNT), mode);
    mask = (val ^ old) &
           (MMTBASE_ASID | MMTBASE_TN0PB | MMTBASE_CNT);

    if (vm && mask) {
        /*
         * Any change to the active translation base or mode can invalidate
         * cached low-address fetches, not just ASID changes.
         */
        tlb_flush(env_cpu(env));
        env->sysreg[acr].mmtbase = val;
    }

    return LINX_EXCP_NONE;
}

static LINXException read_mmconfig(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    *val = env->sysreg[acr].mmconfig;

    return LINX_EXCP_NONE;
}

static LINXException write_mmconfig(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    if (g_getenv("LINX_LOG_MMU_CSR")) {
        fprintf(stderr,
                "LINX_MMCONFIG_WRITE pc=%#" PRIx64 " acr=%d old=%#" PRIx64
                " new=%#" PRIx64 "\n",
                (uint64_t)env->pc, acr, (uint64_t)env->sysreg[acr].mmconfig,
                (uint64_t)val);
    }

    if (env->sysreg[acr].mmconfig != val) {
        tlb_flush(env_cpu(env));
    }
    env->sysreg[acr].mmconfig = val;
    return LINX_EXCP_NONE;
}

static uint64_t cpu_linx_read_rtc(uint32_t timebase_freq)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    timebase_freq, NANOSECONDS_PER_SECOND);
}

/*
 * Called when timecmp is written to update the QEMU timer or immediat
 * trigger timer interrupt if mtimecmp <= current timer value.
 */
static void linx_gt_ptimer_write_timecmp(LINXCPU *cpu, int acr,
                                         uint32_t timebase_freq)
{
    uint64_t next;
    uint64_t diff;

    uint64_t rtc_r = cpu_linx_read_rtc(timebase_freq);

    if (cpu->env.sysreg[acr].timecmp <= rtc_r) {
        /*
         * If we're setting an TIMECMP value in the "past",
         * immediately raise the timer interrupt
         */
        if(acr == 1)
            qemu_set_irq(cpu->gt_timer_output[GTIMER_ACR1], 1);
        else if(acr == 0)
            qemu_set_irq(cpu->gt_timer_output[GTIMER_ACR0], 1);
        return;
    }

    /* otherwise, set up the future timer interrupt */
    if(acr == 1)
        qemu_set_irq(cpu->gt_timer_output[GTIMER_ACR1], 0);
    else if(acr == 0)
        qemu_set_irq(cpu->gt_timer_output[GTIMER_ACR0], 0);
    diff = cpu->env.sysreg[acr].timecmp - rtc_r;
    /* back to ns (note args switched in muldiv64) */
    uint64_t ns_diff = muldiv64(diff, NANOSECONDS_PER_SECOND, timebase_freq);

    /*
     * check if ns_diff overflowed and check if the addition would pot
     * overflow
     */
    if ((NANOSECONDS_PER_SECOND > timebase_freq && ns_diff < diff) ||
        ns_diff > INT64_MAX) {
        next = INT64_MAX;
    } else {
        /*
         * as it is very unlikely qemu_clock_get_ns will return a valu
         * greater than INT64_MAX, no additional check is needed for a
         * unsigned integer overflow.
         */
        next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns_diff;
        /*
         * if ns_diff is INT64_MAX next may still be outside the range
         * of a signed integer.
         */
        next = MIN(next, INT64_MAX);
    }

    if(acr == 1)
        timer_mod(cpu->env.gt_timer[GTIMER_ACR1], next);
    else if(acr == 0)
        timer_mod(cpu->env.gt_timer[GTIMER_ACR0], next);
}

static LINXException read_time(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    //offset for virtual counter, todo
    uint64_t delta = 0;

    *val = cpu_linx_read_rtc(LINX_DEFAULT_TIMEBASE_FREQ) + delta;
    return LINX_EXCP_NONE;
}

static LINXException read_timecmp(CPULINXState *env, int ssrno,
                                       target_ulong *val)
{
    int acr;
    acr = get_field(ssrno, SYSREG_ADDR_ACR);
    *val = env->sysreg[acr].timecmp;
    return LINX_EXCP_NONE;
}

static LINXException write_timecmp(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    env->sysreg[acr].timecmp = val;
    linx_gt_ptimer_write_timecmp(env_archcpu(env), acr, LINX_DEFAULT_TIMEBASE_FREQ);
    return LINX_EXCP_NONE;
}
#endif

#define READ_ACR_SSR(name, ssr)                                        \
    static LINXException read_##name(CPULINXState *env, int ssrno,     \
                                      target_ulong *val)               \
    {                                                                  \
        int acr = get_field(ssrno, SYSREG_ADDR_ACR);                   \
        *val = env->sysreg[acr].ssr;                                   \
        return LINX_EXCP_NONE;                                         \
    }

#define READ_SSR(name, ssr)                                            \
    static LINXException read_##name(CPULINXState *env, int ssrno,     \
                                      target_ulong *val)               \
    {                                                                  \
        *val = env->ssr;                                               \
        return LINX_EXCP_NONE;                                         \
    }

#define WRITE_SSR(name, ssr)                                           \
    static LINXException write_##name(CPULINXState *env, int ssrno,    \
                                       target_ulong val)               \
    {                                                                  \
        env->ssr = val;                                                \
        return LINX_EXCP_NONE;                                         \
    }

static LINXException read_scratch(CPULINXState *env, int ssrno,
                                  target_ulong *val)
{
    int idx = ssrno - CSR_SCRATCH0;

    *val = env->csr_scratch[idx];
    return LINX_EXCP_NONE;
}

static LINXException write_scratch(CPULINXState *env, int ssrno,
                                   target_ulong val)
{
    int idx = ssrno - CSR_SCRATCH0;

    env->csr_scratch[idx] = val;
    return LINX_EXCP_NONE;
}

static LINXException write_fssr(CPULINXState *env, int csrno,
                                target_ulong val)
{
    int softrm;
    uint32_t rm = extract32(val, 8, 2);

    switch (rm) {
    case 0:
        softrm = float_round_nearest_even;
        break;
    case 1:
        softrm = float_round_to_zero;
        break;
    case 2:
        softrm = float_round_down;
        break;
    case 3:
        softrm = float_round_up;
        break;
    default:
        return LINX_EXCP_INSN_ILLEGAL;
    }

    set_float_rounding_mode(softrm, &env->fp_status);
    env->csr_fssr = val;
    return LINX_EXCP_NONE;
}

READ_SSR(tp, csr_tp)
WRITE_SSR(tp, csr_tp)
READ_SSR(gp, csr_gp)
WRITE_SSR(gp, csr_gp)
READ_SSR(cw, csr_cw)
WRITE_SSR(cw, csr_cw)
READ_SSR(tr1, csr_tr1)
WRITE_SSR(tr1, csr_tr1)
READ_SSR(tr2, csr_tr2)
WRITE_SSR(tr2, csr_tr2)
READ_SSR(fssr, csr_fssr)

READ_SSR(lc0, csr_lc[0])
WRITE_SSR(lc0, csr_lc[0])
READ_SSR(lb0, csr_lb[0])
WRITE_SSR(lb0, csr_lb[0])
READ_SSR(lpcb0, csr_lpcb[0])
WRITE_SSR(lpcb0, csr_lpcb[0])
READ_SSR(lpce0, csr_lpce[0])
WRITE_SSR(lpce0, csr_lpce[0])

READ_SSR(lc1, csr_lc[1])
WRITE_SSR(lc1, csr_lc[1])
READ_SSR(lb1, csr_lb[1])
WRITE_SSR(lb1, csr_lb[1])
READ_SSR(lpcb1, csr_lpcb[1])
WRITE_SSR(lpcb1, csr_lpcb[1])
READ_SSR(lpce1, csr_lpce[1])
WRITE_SSR(lpce1, csr_lpce[1])

READ_SSR(lc2, csr_lc[2])
WRITE_SSR(lc2, csr_lc[2])
READ_SSR(lb2, csr_lb[2])
WRITE_SSR(lb2, csr_lb[2])
READ_SSR(lpcb2, csr_lpcb[2])
WRITE_SSR(lpcb2, csr_lpcb[2])
READ_SSR(lpce2, csr_lpce[2])
WRITE_SSR(lpce2, csr_lpce[2])

READ_SSR(lanenum, csr_lanenum)
WRITE_SSR(lanenum, csr_lanenum)

#ifndef CONFIG_USER_ONLY
static LINXException write_xbinfo(CPULINXState *env, int ssrno,
                                  target_ulong val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);
    env->sysreg[acr].xbinfo = val & XBINFO_BASE;
    return LINX_EXCP_NONE;
}

READ_ACR_SSR(xbinfo, xbinfo)
READ_ACR_SSR(acr_param, acr_param)
#endif

static void log_crs_update(const char *name, target_ulong old_value,
                           target_ulong new_value, LINXException ret)
{
    if (likely(!qemu_loglevel_mask(CPU_LOG_SSR))) {
        return;
    }

    if (old_value == new_value)
        qemu_log("SSR %s write(no update): " TARGET_FMT_lx, name, old_value);
    else
        qemu_log("SSR %s update: " TARGET_FMT_lx " => " TARGET_FMT_lx,
                 name, old_value, new_value);

    if (ret == LINX_EXCP_NONE)
        qemu_log("\n");
    else
        qemu_log(" fail: %d\n", ret);
}

/*
 * linx_csrrw - read and/or update control and status register
 *
 * csrr   <->  linx_csrrw(env, ssrno, ret_value, 0, 0);
 * csrrw  <->  linx_csrrw(env, ssrno, ret_value, value, -1);
 * csrrs  <->  linx_csrrw(env, ssrno, ret_value, -1, value);
 * csrrc  <->  linx_csrrw(env, ssrno, ret_value, 0, value);
 */

LINXException linx_csrrw(CPULINXState *env, int ssrno,
                           target_ulong *ret_value,
                           target_ulong new_value, target_ulong write_mask)
{
    LINXException ret;
    target_ulong old_value;

    /* check privileges and return LINX_EXCP_INSN_ILLEGAL if check fails */
#if !defined(CONFIG_USER_ONLY)
    int effective_priv = env->priv;

    /* FIXME: The mode of checking the privilege status needs to be modified */
    if (!env->debugger &&
        (ssrno >= A0_ECSTATE &&
         effective_priv > get_field(ssrno, SYSREG_ADDR_ACR))) {
        return LINX_EXCP_INSN_ILLEGAL;
    }
#endif

    /* check predicate */
    if (!csr_ops[ssrno].predicate) {
        env->badssr = ssrno;
        return LINX_EXCP_ILLSSR;
    }
    ret = csr_ops[ssrno].predicate(env, ssrno);
    if (ret != LINX_EXCP_NONE) {
        return ret;
    }

    /* execute combined read/write operation if it exists */
    if (csr_ops[ssrno].op) {
        return csr_ops[ssrno].op(env, ssrno, ret_value, new_value, write_mask);
    }

    /* if no accessor exists then return failure */
    if (!csr_ops[ssrno].read) {
        return LINX_EXCP_INSN_ILLEGAL;
    }
    /* read old value */
    ret = csr_ops[ssrno].read(env, ssrno, &old_value);
    if (ret != LINX_EXCP_NONE) {
        return ret;
    }

    /* write value if writable and write mask set, otherwise drop writes */
    if (write_mask) {
        new_value = (old_value & ~write_mask) | (new_value & write_mask);
        if (csr_ops[ssrno].write) {
            ret = csr_ops[ssrno].write(env, ssrno, new_value);

            log_crs_update(csr_ops[ssrno].name, old_value, new_value, ret);

            if (ret != LINX_EXCP_NONE)
                return ret;
        }
    }

    /* return old value */
    if (ret_value) {
        *ret_value = old_value;
    }

    return LINX_EXCP_NONE;
}

/*
 * Debugger support.  If not in user mode, set env->debugger before the
 * linx_csrrw call and clear it after the call.
 */
LINXException linx_csrrw_debug(CPULINXState *env, int ssrno,
                                 target_ulong *ret_value,
                                 target_ulong new_value,
                                 target_ulong write_mask)
{
    LINXException ret;
#if !defined(CONFIG_USER_ONLY)
    env->debugger = true;
#endif
    ret = linx_csrrw(env, ssrno, ret_value, new_value, write_mask);
#if !defined(CONFIG_USER_ONLY)
    env->debugger = false;
#endif
    return ret;
}

/* Control and Status Register function table */
linx_csr_operations csr_ops[SSR_TABLE_SIZE] = {

#if !defined(CONFIG_USER_ONLY)
    /* Machine Information Registers */
    [CSTATE]        = { "cstate",   any,    read_cstate,    write_cstate },
    [LXLCID]        = { "lxlcid",   any,    read_lxlcid, },
    [TIME]          = { "time",   any,    read_time, },

    [A0_ECSTATE]        = {"a0_ecstate",    any, read_ecstate, write_ecstate },
    [A0_EVBASE]         = {"a0_evbase",    any, read_evbase, write_evbase },
    [A0_ECAUSE]         = {"a0_ecause", any, read_ecause, write_ecause },
    [A0_EARG0]          = {"a0_earg0", any, read_earg0, write_earg0 },
    [A0_ETEMP]          = {"a0_etemp",  any,    read_etemp, write_etemp},
    [A0_FUTO]           = {"a0_futo",  any,    read_futo, write_futo},
    [A0_IENABLE]        = {"a0_econfig",  any,  read_econfig, write_econfig},
    [A0_IPENDING]       = {"a0_ipending", any, NULL, NULL, rmw_ipending_acr0},
    [A0_TOPEI]          = {"a0_topei", any, read_topei, NULL},
    [A0_EOIEI]          = {"a0_eoiei", any, read_eoiei, write_eoiei, NULL},
    [A0_EBPC]           = {"a0_ebpc",  any, read_ebpc, write_ebpc},
    [A0_EBARG]          = {"a0_ebarg",  any, read_ebarg, write_ebarg},
    [A0_ETPC]           = {"a0_etpc",  any, read_etpc, write_etpc},
    [A0_EBPCN]          = {"a0_ebpcn",  any, read_ebpcn, write_ebpcn},
    [A0_MMTBASE]        = {"a0_mmtbase",  any,    read_mmtbase, write_mmtbase},
    [A0_MMCONFIG]       = {"a0_mmconfig",  any,    read_mmconfig, write_mmconfig},
    [A0_TIME]           = {"a0_time", any, read_time },
    [A0_TIMECMP]        = {"a0_timecmp", any, read_timecmp, write_timecmp },
    [A0_XBINFO]         = {"a0_xbinfo", any, read_xbinfo, write_xbinfo },
    [A0_ACR_PARAM]      = {"a0_acr_param", any, read_acr_param },
    [A0_EBARG0]         = {"a0_ebarg0", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_BPC_CUR]  = {"a0_ebarg_bpc_cur", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_BPC_TGT]  = {"a0_ebarg_bpc_tgt", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_TPC]      = {"a0_ebarg_tpc", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_LRA]      = {"a0_ebarg_lra", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_TQ0]      = {"a0_ebarg_tq0", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_TQ1]      = {"a0_ebarg_tq1", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_TQ2]      = {"a0_ebarg_tq2", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_TQ3]      = {"a0_ebarg_tq3", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_UQ0]      = {"a0_ebarg_uq0", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_UQ1]      = {"a0_ebarg_uq1", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_UQ2]      = {"a0_ebarg_uq2", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_UQ3]      = {"a0_ebarg_uq3", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_LB]       = {"a0_ebarg_lb", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_LC]       = {"a0_ebarg_lc", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_EXTCTX_PTR] = {"a0_ebarg_extctx_ptr", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_EXTCTX_META] = {"a0_ebarg_extctx_meta", any, read_ebarg_group, write_ebarg_group },
    [A0_EBARG_TPLFLAGS] = {"a0_ebarg_tplflags", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT0]   = {"a0_ebstate_ext0", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT1]   = {"a0_ebstate_ext1", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT2]   = {"a0_ebstate_ext2", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT3]   = {"a0_ebstate_ext3", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT4]   = {"a0_ebstate_ext4", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT5]   = {"a0_ebstate_ext5", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT6]   = {"a0_ebstate_ext6", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT7]   = {"a0_ebstate_ext7", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT8]   = {"a0_ebstate_ext8", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT9]   = {"a0_ebstate_ext9", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT10]  = {"a0_ebstate_ext10", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT11]  = {"a0_ebstate_ext11", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT12]  = {"a0_ebstate_ext12", any, read_ebarg_group, write_ebarg_group },
    [A0_EBSTATE_EXT13]  = {"a0_ebstate_ext13", any, read_ebarg_group, write_ebarg_group },

    [A1_ECSTATE]        = {"a1_ecstate", acr1, read_ecstate, write_ecstate},
    [A1_EVBASE]         = {"a1_evbase", acr1, read_evbase, write_evbase},
    [A1_ECAUSE]         = {"a1_ecause", acr1, read_ecause},
    [A1_EARG0]          = {"a1_earg0", acr1, read_earg0, write_earg0 },
    [A1_ETEMP]          = {"a1_etemp", acr1, read_etemp, write_etemp},
    [A1_FUTO]           = {"a1_futo", acr1, read_futo, write_futo},
    [A1_IENABLE]        = {"a1_econfig", acr1, read_econfig, write_econfig},
    [A1_IPENDING]       = {"a1_ipending", acr1, NULL, NULL, rmw_ipending_acr1},
    [A1_TOPEI]          = {"a1_topei", acr1, read_topei, NULL},
    [A1_EOIEI]          = {"a1_eoiei", acr1, read_eoiei, write_eoiei, NULL},
    [A1_EBPC]           = {"a1_ebpc", acr1, read_ebpc, write_ebpc},
    [A1_EBARG]          = {"a1_ebarg", acr1, read_ebarg, write_ebarg},
    [A1_ETPC]           = {"a1_etpc", acr1, read_etpc, write_etpc},
    [A1_EBPCN]          = {"a1_ebpcn", acr1, read_ebpcn, write_ebpcn},
    [A1_MMTBASE]        = {"a1_mmtbase", any, read_mmtbase, write_mmtbase},
    [A1_MMCONFIG]       = {"a1_mmconfig", any, read_mmconfig, write_mmconfig},
    [A1_TIME]           = {"a1_time", any, read_time },
    [A1_TIMECMP]        = {"a1_timecmp", any, read_timecmp, write_timecmp },
    [A1_XBINFO]         = {"a1_xbinfo", any, read_xbinfo, write_xbinfo },
    [A1_ACR_PARAM]      = {"a1_acr_param", any, read_acr_param },
    [A1_EBARG0]         = {"a1_ebarg0", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_BPC_CUR]  = {"a1_ebarg_bpc_cur", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_BPC_TGT]  = {"a1_ebarg_bpc_tgt", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_TPC]      = {"a1_ebarg_tpc", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_LRA]      = {"a1_ebarg_lra", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_TQ0]      = {"a1_ebarg_tq0", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_TQ1]      = {"a1_ebarg_tq1", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_TQ2]      = {"a1_ebarg_tq2", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_TQ3]      = {"a1_ebarg_tq3", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_UQ0]      = {"a1_ebarg_uq0", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_UQ1]      = {"a1_ebarg_uq1", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_UQ2]      = {"a1_ebarg_uq2", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_UQ3]      = {"a1_ebarg_uq3", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_LB]       = {"a1_ebarg_lb", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_LC]       = {"a1_ebarg_lc", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_EXTCTX_PTR] = {"a1_ebarg_extctx_ptr", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_EXTCTX_META] = {"a1_ebarg_extctx_meta", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBARG_TPLFLAGS] = {"a1_ebarg_tplflags", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT0]   = {"a1_ebstate_ext0", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT1]   = {"a1_ebstate_ext1", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT2]   = {"a1_ebstate_ext2", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT3]   = {"a1_ebstate_ext3", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT4]   = {"a1_ebstate_ext4", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT5]   = {"a1_ebstate_ext5", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT6]   = {"a1_ebstate_ext6", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT7]   = {"a1_ebstate_ext7", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT8]   = {"a1_ebstate_ext8", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT9]   = {"a1_ebstate_ext9", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT10]  = {"a1_ebstate_ext10", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT11]  = {"a1_ebstate_ext11", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT12]  = {"a1_ebstate_ext12", acr1, read_ebarg_group, write_ebarg_group },
    [A1_EBSTATE_EXT13]  = {"a1_ebstate_ext13", acr1, read_ebarg_group, write_ebarg_group },
#endif /* !CONFIG_USER_ONLY */
    [CSR_TP]  = { "tp",  any, read_tp,  write_tp  },
    [CSR_GP]  = { "gp",  any, read_gp,  write_gp  },
    [CSR_SCRATCH0]  = { "scratch0",  any, read_scratch, write_scratch },
    [CSR_SCRATCH1]  = { "scratch1",  any, read_scratch, write_scratch },
    [CSR_SCRATCH2]  = { "scratch2",  any, read_scratch, write_scratch },
    [CSR_SCRATCH3]  = { "scratch3",  any, read_scratch, write_scratch },
    [CSR_SCRATCH4]  = { "scratch4",  any, read_scratch, write_scratch },
    [CSR_SCRATCH5]  = { "scratch5",  any, read_scratch, write_scratch },
    [CSR_SCRATCH6]  = { "scratch6",  any, read_scratch, write_scratch },
    [CSR_SCRATCH7]  = { "scratch7",  any, read_scratch, write_scratch },
    [CSR_SCRATCH8]  = { "scratch8",  any, read_scratch, write_scratch },
    [CSR_SCRATCH9]  = { "scratch9",  any, read_scratch, write_scratch },
    [CSR_SCRATCH10] = { "scratch10", any, read_scratch, write_scratch },
    [CSR_SCRATCH11] = { "scratch11", any, read_scratch, write_scratch },
    [CSR_SCRATCH12] = { "scratch12", any, read_scratch, write_scratch },
    [CSR_SCRATCH13] = { "scratch13", any, read_scratch, write_scratch },
    [CSR_SCRATCH14] = { "scratch14", any, read_scratch, write_scratch },
    [CSR_SCRATCH15] = { "scratch15", any, read_scratch, write_scratch },
    [CSR_SCRATCH16] = { "scratch16", any, read_scratch, write_scratch },
    [CSR_SCRATCH17] = { "scratch17", any, read_scratch, write_scratch },
    [CSR_SCRATCH18] = { "scratch18", any, read_scratch, write_scratch },
    [CSR_CW]  = { "cw",  any, read_cw,  write_cw  },
    [CSR_TR1] = { "tr1", any, read_tr1, write_tr1 },
    [CSR_TR2] = { "tr2", any, read_tr2, write_tr2 },

    [CSR_LC0]    = { "lc0",   any, read_lc0,   write_lc0 },
    [CSR_LB0]    = { "lb0",   any, read_lb0,   write_lb0 },
    [CSR_LPCB0]  = { "lpcb0", any, read_lpcb0, write_lpcb0 },
    [CSR_LPCE0]  = { "lpce0", any, read_lpce0, write_lpce0 },
    [CSR_LC1]    = { "lc1",   any, read_lc1,   write_lc1 },
    [CSR_LB1]    = { "lb1",   any, read_lb1,   write_lb1 },
    [CSR_LPCB1]  = { "lpcb1", any, read_lpcb1, write_lpcb1 },
    [CSR_LPCE1]  = { "lpce1", any, read_lpce1, write_lpce1 },
    [CSR_LC2]    = { "lc2",   any, read_lc2,   write_lc2 },
    [CSR_LB2]    = { "lb2",   any, read_lb2,   write_lb2 },
    [CSR_LPCB2]  = { "lpcb2", any, read_lpcb2, write_lpcb2 },
    [CSR_LPCE2]  = { "lpce2", any, read_lpce2, write_lpce2 },

    [CSR_LANENUM] = { "lanenum", any, read_lanenum, write_lanenum},
};
