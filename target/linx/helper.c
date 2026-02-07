/*
 * LinxISA helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "accel/tcg/cpu-ldst.h"
#include "fpu/softfloat-helpers.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "exec/memopidx.h"
#include "accel/tcg/cpu-ldst-common.h"

/* Semihosting operations via EBREAK immediate */
#define LINX_SEMIHOST_EXIT      0  /* Exit program */
#define LINX_SEMIHOST_PUTCHAR   1  /* a0 = character to output */
#define LINX_SEMIHOST_WRITE     2  /* a0 = fd, a1 = buf, a2 = len -> a0 = bytes written */
#define LINX_SEMIHOST_READ      3  /* a0 = fd, a1 = buf, a2 = len -> a0 = bytes read */

/* ------------------------------------------------------------------------- */
/* System Status Register (SSR) helpers                                      */
/* ------------------------------------------------------------------------- */

/* SSR IDs (bring-up subset; see `isa.txt`). */
enum {
    LINX_SSR_TP    = 0x0000,
    LINX_SSR_GP    = 0x0001,
    LINX_SSR_TIME  = 0x0010,
    LINX_SSR_CYCLE = 0x0c00,
    LINX_SSR_CSTATE = 0x0020,
};

/* Managing-ACR SSR indices (low 12 bits). */
enum {
    LINX_SSR_ECSTATE  = 0xF00,
    LINX_SSR_EVBASE   = 0xF01,
    LINX_SSR_TRAPNO   = 0xF02,
    LINX_SSR_TRAPARG0 = 0xF03,
    LINX_SSR_IPENDING = 0xF08,
    LINX_SSR_EOIEI    = 0xF0A,
    LINX_SSR_EBPC     = 0xF0B,
    LINX_SSR_EBARG    = 0xF0C,
    LINX_SSR_ETPC     = 0xF0D,
    LINX_SSR_EBPCN    = 0xF0E,
    LINX_SSR_TIMER_TIME   = 0xF20,
    LINX_SSR_TIMER_TIMECMP = 0xF21,
};

/*
 * CSTATE (bring-up encoding).
 *
 * The privileged architecture describes CSTATE as a packed state register
 * (ACR, interrupt enable, flags, ...). For QEMU bring-up, model only:
 *   - CSTATE.ACR: bits[3:0]  (current Access Control Ring)
 *   - CSTATE.I:   bit[4]     (interrupt enable for same-ring interrupts)
 *
 * All other bits are preserved on writes but are otherwise ignored.
 */
#define LINX_CSTATE_ACR_MASK 0xFULL
#define LINX_CSTATE_I_BIT    (1ULL << 4)

static inline uint64_t linx_cstate_set_acr(uint64_t cstate, uint32_t acr)
{
    return (cstate & ~LINX_CSTATE_ACR_MASK) | ((uint64_t)acr & LINX_CSTATE_ACR_MASK);
}

static inline uint32_t linx_cstate_get_acr(uint64_t cstate)
{
    return (uint32_t)(cstate & LINX_CSTATE_ACR_MASK);
}

static inline bool linx_irq_allowed(const CPULinxState *env, uint32_t dst_acr)
{
    const uint32_t cur_acr = env->acr & 0xF;
    const uint64_t cstate = env->ssr[LINX_SSR_CSTATE];
    const bool ie = (cstate & LINX_CSTATE_I_BIT) != 0;

    /*
     * v0.1 draft: if an interrupt routes to a more privileged ACR, it may
     * preempt regardless of the current ring's I bit. If it routes to the
     * current ACR, it is gated by CSTATE.I.
     */
    if (dst_acr < cur_acr) {
        return true;
    }
    if (dst_acr == cur_acr) {
        return ie;
    }
    /* Less-privileged target interrupts are not modeled (bring-up). */
    return ie;
}

static inline void linx_irq_kick_if_allowed(CPULinxState *env, uint32_t dst_acr)
{
    CPUState *cs = env_cpu(env);

    if (!linx_irq_allowed(env, dst_acr)) {
        return;
    }
    if (env->ssr_acr[dst_acr][LINX_SSR_IPENDING] == 0) {
        return;
    }
    cpu_interrupt(cs, CPU_INTERRUPT_HARD);
}

/* ACRC request_type values (v0.1 draft). */
enum {
    LINX_SCT_MAC = 0,
    LINX_SCT_SYS = 1,
    LINX_SCT_SEC = 2,
};

static inline uint32_t linx_ssr_low12(uint32_t ssrid)
{
    return ssrid & 0xfffu;
}

static inline bool linx_ssr_is_manager_idx(uint32_t idx)
{
    return (idx & 0xf00u) == 0xf00u;
}

uint64_t HELPER(linx_ssr_read)(CPULinxState *env, uint32_t ssrid)
{
    uint32_t idx = linx_ssr_low12(ssrid);
    const bool is_manager = linx_ssr_is_manager_idx(idx);
    const uint32_t bank = is_manager ? ((ssrid >> 12) & 0xFu) : 0u;

    switch (idx) {
    case LINX_SSR_CYCLE:
        /* Bring-up: model CYCLE as the dynamic instruction counter. */
        return env->insn_count;
    case LINX_SSR_TIME:
        /* Virtual time in nanoseconds. */
        return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    default:
        if (is_manager) {
            if (idx == LINX_SSR_TIMER_TIME) {
                return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
            if (bank < LINX_ACR_COUNT) {
                return env->ssr_acr[bank][idx];
            }
            return 0;
        }
        return env->ssr[idx];
    }
}

void HELPER(linx_ssr_write)(CPULinxState *env, uint32_t ssrid, uint64_t value)
{
    uint32_t idx = linx_ssr_low12(ssrid);
    const bool is_manager = linx_ssr_is_manager_idx(idx);
    const uint32_t bank = is_manager ? ((ssrid >> 12) & 0xFu) : 0u;

    switch (idx) {
    case LINX_SSR_CYCLE:
    case LINX_SSR_TIME:
        /* Read-only for now. Ignore writes. */
        return;
    case LINX_SSR_CSTATE:
        /*
         * Track ACR in both env->acr and CSTATE.ACR. If software enables
         * interrupts and there is a pending interrupt for the current ring,
         * kick the CPU so it can be taken.
         */
        env->ssr[idx] = value;
        env->acr = linx_cstate_get_acr(value);
        linx_irq_kick_if_allowed(env, env->acr & 0xF);
        return;
    default:
        if (is_manager) {
            if (bank >= LINX_ACR_COUNT) {
                return;
            }

            if (idx == LINX_SSR_EOIEI) {
                /*
                 * End of interrupt (v0.1 draft): clear the pending bit for the
                 * given interrupt ID.
                 *
                 * Bring-up: model a simple bit-per-interrupt IPENDING bitmap,
                 * and treat EOIEI's write value as the interrupt ID.
                 */
                const uint32_t irq_id = (uint32_t)value & 63u;
                env->ssr_acr[bank][LINX_SSR_IPENDING] &= ~(1ull << irq_id);
                return;
            }

            if (idx == LINX_SSR_TIMER_TIMECMP) {
                /*
                 * Virtual timer compare (bring-up).
                 *
                 * If TIMECMP is non-zero, schedule a virtual timer interrupt at
                 * that absolute virtual time (ns). If TIMECMP is zero, cancel.
                 */
                env->ssr_acr[bank][idx] = value;

                if (bank == 0 && env->timer) {
                    CPUState *cs = env_cpu(env);
                    if (value == 0) {
                        timer_del(env->timer);
                        env->ssr_acr[0][LINX_SSR_IPENDING] &= ~(1ull << 0);
                        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
                        return;
                    }

                    const uint64_t now = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                    if (value <= now) {
                        env->ssr_acr[0][LINX_SSR_IPENDING] |= (1ull << 0);
                        linx_irq_kick_if_allowed(env, 0);
                        return;
                    }
                    timer_mod_ns(env->timer, (int64_t)value);
                }
                return;
            }

            env->ssr_acr[bank][idx] = value;
            return;
        }
        env->ssr[idx] = value;
        return;
    }
}

uint64_t HELPER(linx_ssr_swap)(CPULinxState *env, uint32_t ssrid, uint64_t value)
{
    uint64_t old = HELPER(linx_ssr_read)(env, ssrid);
    HELPER(linx_ssr_write)(env, ssrid, value);
    return old;
}

/* ------------------------------------------------------------------------- */
/* Privilege transitions (bring-up)                                          */
/* ------------------------------------------------------------------------- */

void HELPER(linx_service_request)(CPULinxState *env, uint32_t request_type,
                                  uint64_t bpc, uint64_t tpc, uint64_t pc_next)
{
    CPUState *cs = env_cpu(env);
    const uint32_t src_acr = env->acr & 0xFu;
    uint32_t dst_acr = 0;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: SERVICE_REQUEST src_acr=%u req=%u bpc=0x%" PRIx64 " tpc=0x%" PRIx64
                  " pc_next=0x%" PRIx64 "\n",
                  src_acr, request_type, bpc, tpc, pc_next);

    /* ACRC request_type validity + routing (v0.1 draft). */
    if (src_acr == 1) {
        if (request_type != LINX_SCT_MAC && request_type != LINX_SCT_SEC) {
            cs->exception_index = LINX_EXCP_ILLEGAL_INST;
            cpu_loop_exit(cs);
        }
        dst_acr = 0;
    } else if (src_acr == 2) {
        if (request_type != LINX_SCT_MAC &&
            request_type != LINX_SCT_SYS &&
            request_type != LINX_SCT_SEC) {
            cs->exception_index = LINX_EXCP_ILLEGAL_INST;
            cpu_loop_exit(cs);
        }
        /*
         * Linux bring-up model: route all user SERVICE_REQUEST traps to ACR0.
         *
         * The kernel currently installs EVBASE only for ACR0 and reads trap
         * metadata from the ACR0 banked SSRs (see arch/linx/kernel/traps.c).
         */
        dst_acr = 0;
    } else {
        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
        cpu_loop_exit(cs);
    }

    /* Save trap state into the managing ACR bank. */
    env->ssr_acr[dst_acr][LINX_SSR_ECSTATE] = env->ssr[LINX_SSR_CSTATE];
    env->ssr_acr[dst_acr][LINX_SSR_EBPC] = bpc;
    env->ssr_acr[dst_acr][LINX_SSR_ETPC] = tpc;
    env->ssr_acr[dst_acr][LINX_SSR_EBPCN] = pc_next;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG] = 0;

    /* Trap reporting (minimal bring-up encoding). */
    env->ssr_acr[dst_acr][LINX_SSR_TRAPNO] = 16; /* E_SCALL */
    env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = request_type;

    /* Disable interrupts and switch to managing ring, then vector to EVBASE. */
    env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
    env->acr = dst_acr;
    env->ssr[LINX_SSR_CSTATE] = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], dst_acr);
    const uint64_t evbase = env->ssr_acr[dst_acr][LINX_SSR_EVBASE];
    env->pc = evbase ? evbase : tpc;

    cs->exception_index = -1;
    cpu_loop_exit(cs);
}

void HELPER(linx_acr_enter)(CPULinxState *env, uint32_t rra_type)
{
    (void)rra_type;
    CPUState *cs = env_cpu(env);
    const uint32_t mgr = env->acr & 0xFu;
    const uint64_t ecstate = env->ssr_acr[mgr][LINX_SSR_ECSTATE];
    const uint32_t target = linx_cstate_get_acr(ecstate);
    const uint64_t resume_pc = env->ssr_acr[mgr][LINX_SSR_EBPC];

    env->acr = target;
    env->ssr[LINX_SSR_CSTATE] = ecstate;
    env->pc = resume_pc;
    linx_irq_kick_if_allowed(env, env->acr & 0xF);

    cs->exception_index = -1;
    cpu_loop_exit(cs);
}

/* ------------------------------------------------------------------------- */
/* Atomics (LR/SC + fetch-RMW)                                               */
/* ------------------------------------------------------------------------- */

static inline MemOpIdx linx_oi_le(MemOp mop)
{
    /* Linx uses a single MMU index (0) and little-endian. */
    return make_memop_idx(mop | MO_LE, 0);
}

static inline void linx_lr_set(CPULinxState *env, uint64_t addr, uint32_t size)
{
    env->lr_addr = addr;
    env->lr_size = size;
    env->lr_valid = 1;
}

static inline void linx_lr_clear(CPULinxState *env)
{
    env->lr_valid = 0;
}

uint64_t HELPER(linx_lr_w)(CPULinxState *env, uint64_t addr)
{
    uint32_t v = cpu_ldl_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UL), GETPC());
    linx_lr_set(env, addr, 4);
    return (uint64_t)v;
}

uint64_t HELPER(linx_lr_d)(CPULinxState *env, uint64_t addr)
{
    uint64_t v = cpu_ldq_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UQ), GETPC());
    linx_lr_set(env, addr, 8);
    return v;
}

uint64_t HELPER(linx_sc_w)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    /*
     * SC.W returns 0 on success, non-zero on failure (bring-up convention).
     * This is a simplified reservation model: any intervening store clears the
     * reservation (via the translator calling linx_lr_clear on stores/atomics).
     */
    uint64_t ok = (env->lr_valid && env->lr_addr == addr && env->lr_size == 4) ? 0 : 1;
    if (ok == 0) {
        cpu_stl_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UL), GETPC());
    }
    linx_lr_clear(env);
    return ok;
}

uint64_t HELPER(linx_sc_d)(CPULinxState *env, uint64_t addr, uint64_t value)
{
    uint64_t ok = (env->lr_valid && env->lr_addr == addr && env->lr_size == 8) ? 0 : 1;
    if (ok == 0) {
        cpu_stq_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UQ), GETPC());
    }
    linx_lr_clear(env);
    return ok;
}

uint64_t HELPER(linx_swapw)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    linx_lr_clear(env);
    return (uint64_t)cpu_atomic_xchgl_le_mmu((CPUArchState *)env, addr, value,
                                            linx_oi_le(MO_UL), GETPC());
}

uint64_t HELPER(linx_swapd)(CPULinxState *env, uint64_t addr, uint64_t value)
{
    linx_lr_clear(env);
    return cpu_atomic_xchgq_le_mmu((CPUArchState *)env, addr, value,
                                   linx_oi_le(MO_UQ), GETPC());
}

uint64_t HELPER(linx_lw_add)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    linx_lr_clear(env);
    return (uint64_t)cpu_atomic_fetch_addl_le_mmu((CPUArchState *)env, addr, value,
                                                  linx_oi_le(MO_UL), GETPC());
}

uint64_t HELPER(linx_ld_add)(CPULinxState *env, uint64_t addr, uint64_t value)
{
    linx_lr_clear(env);
    return cpu_atomic_fetch_addq_le_mmu((CPUArchState *)env, addr, value,
                                        linx_oi_le(MO_UQ), GETPC());
}

/* ------------------------------------------------------------------------- */
/* Floating-point helpers (hard-float bring-up)                              */
/* ------------------------------------------------------------------------- */

/* FCSR bits (as documented in docs/isa-manual): */
#define LINX_FCSR_FFLAGS_MASK 0x1fu
#define LINX_FCSR_FRM_SHIFT   8u
#define LINX_FCSR_FRM_MASK    (0x7u << LINX_FCSR_FRM_SHIFT)

static FloatRoundMode linx_fcsr_rounding_mode(uint32_t fcsr)
{
    switch ((fcsr & LINX_FCSR_FRM_MASK) >> LINX_FCSR_FRM_SHIFT) {
    case 0: /* RNE */
        return float_round_nearest_even;
    case 1: /* RDN */
        return float_round_down;
    case 2: /* RUP */
        return float_round_up;
    case 3: /* RTZ */
        return float_round_to_zero;
    case 4: /* RMM */
        return float_round_ties_away;
    default:
        return float_round_nearest_even;
    }
}

static int linx_fcsr_to_softfloat_flags(uint32_t fcsr)
{
    int flags = 0;
    if (fcsr & (1u << 0)) {
        flags |= float_flag_invalid;
    }
    if (fcsr & (1u << 1)) {
        flags |= float_flag_divbyzero;
    }
    if (fcsr & (1u << 2)) {
        flags |= float_flag_overflow;
    }
    if (fcsr & (1u << 3)) {
        flags |= float_flag_underflow;
    }
    if (fcsr & (1u << 4)) {
        flags |= float_flag_inexact;
    }
    return flags;
}

static uint32_t linx_softfloat_flags_to_fcsr(int flags)
{
    uint32_t fcsr = 0;
    if (flags & float_flag_invalid) {
        fcsr |= (1u << 0);
    }
    if (flags & float_flag_divbyzero) {
        fcsr |= (1u << 1);
    }
    if (flags & float_flag_overflow) {
        fcsr |= (1u << 2);
    }
    if (flags & float_flag_underflow) {
        fcsr |= (1u << 3);
    }
    if (flags & float_flag_inexact) {
        fcsr |= (1u << 4);
    }
    return fcsr;
}

static void linx_fp_sync_from_fcsr(CPULinxState *env)
{
    set_float_rounding_mode(linx_fcsr_rounding_mode(env->fcsr), &env->fp_status);
    set_float_exception_flags(linx_fcsr_to_softfloat_flags(env->fcsr), &env->fp_status);
}

static void linx_fp_sync_to_fcsr(CPULinxState *env)
{
    uint32_t fcsr = env->fcsr & ~LINX_FCSR_FFLAGS_MASK;
    fcsr |= linx_softfloat_flags_to_fcsr(get_float_exception_flags(&env->fp_status));
    env->fcsr = fcsr;
}

static uint64_t linx_fp_unop_fabs(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_abs((float64)a);
        break;
    case 1: { /* fs */
        float32 ra = float32_abs((float32)(uint32_t)a);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_binop_add(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_add((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_add((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_binop_sub(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_sub((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_sub((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_binop_mul(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_mul((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_mul((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_binop_div(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_div((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_div((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_cmp_eq(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    bool ok = false;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        ok = float64_eq((float64)a, (float64)b, &env->fp_status);
        break;
    case 1:
        ok = float32_eq((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return ok ? 1 : 0;
}

static uint64_t linx_fp_cmp_lt(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    bool ok = false;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        ok = float64_lt((float64)a, (float64)b, &env->fp_status);
        break;
    case 1:
        ok = float32_lt((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return ok ? 1 : 0;
}

static uint64_t linx_fp_cmp_ge(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    bool ok = false;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        ok = float64_le((float64)b, (float64)a, &env->fp_status);
        break;
    case 1:
        ok = float32_le((float32)(uint32_t)b, (float32)(uint32_t)a, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return ok ? 1 : 0;
}

static uint64_t linx_fp_fcvt(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: { /* src fd */
        if ((dsttype & 0x1fu) == 0) {
            res = a;
        } else if ((dsttype & 0x1fu) == 1) {
            float32 v = float64_to_float32((float64)a, &env->fp_status);
            res = (uint64_t)(uint32_t)v;
        } else {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    case 1: { /* src fs */
        uint32_t a32 = (uint32_t)a;
        if ((dsttype & 0x1fu) == 1) {
            res = a32;
        } else if ((dsttype & 0x1fu) == 0) {
            float64 v = float32_to_float64((float32)a32, &env->fp_status);
            res = (uint64_t)v;
        } else {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_fcvtz(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    const unsigned dt = dsttype & 0x1fu;

    switch (srctype & 0x3u) {
    case 0: { /* src fd */
        float64 v = (float64)a;
        switch (dt) {
        case 8: /* s64 */
            res = (uint64_t)float64_to_int64_round_to_zero(v, &env->fp_status);
            break;
        case 9: /* s32 */
            res = (uint64_t)(int64_t)float64_to_int32_round_to_zero(v, &env->fp_status);
            break;
        case 0: /* u64 */
            res = float64_to_uint64_round_to_zero(v, &env->fp_status);
            break;
        case 1: /* u32 */
            res = (uint64_t)float64_to_uint32_round_to_zero(v, &env->fp_status);
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    case 1: { /* src fs */
        float32 v = (float32)(uint32_t)a;
        switch (dt) {
        case 8: /* s64 */
            res = (uint64_t)float32_to_int64_round_to_zero(v, &env->fp_status);
            break;
        case 9: /* s32 */
            res = (uint64_t)(int64_t)float32_to_int32_round_to_zero(v, &env->fp_status);
            break;
        case 0: /* u64 */
            res = float32_to_uint64_round_to_zero(v, &env->fp_status);
            break;
        case 1: /* u32 */
            res = (uint64_t)float32_to_uint32_round_to_zero(v, &env->fp_status);
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_scvtf(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    const unsigned dt = dsttype & 0x1fu;

    if (dt != 0 && dt != 1) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    switch (srctype & 0x3u) {
    case 0: { /* sd */
        int64_t v = (int64_t)a;
        if (dt == 0) {
            res = (uint64_t)int64_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)int64_to_float32(v, &env->fp_status);
        }
        break;
    }
    case 1: { /* sw */
        int32_t v = (int32_t)a;
        if (dt == 0) {
            res = (uint64_t)int32_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)int32_to_float32(v, &env->fp_status);
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_ucvtf(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    const unsigned dt = dsttype & 0x1fu;

    if (dt != 0 && dt != 1) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    switch (srctype & 0x3u) {
    case 0: { /* ud */
        uint64_t v = a;
        if (dt == 0) {
            res = (uint64_t)uint64_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)uint64_to_float32(v, &env->fp_status);
        }
        break;
    }
    case 1: { /* uw */
        uint32_t v = (uint32_t)a;
        if (dt == 0) {
            res = (uint64_t)uint32_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)uint32_to_float32(v, &env->fp_status);
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

uint64_t HELPER(linx_fadd)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_add(env, a, b, srctype);
}

uint64_t HELPER(linx_fsub)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_sub(env, a, b, srctype);
}

uint64_t HELPER(linx_fmul)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_mul(env, a, b, srctype);
}

uint64_t HELPER(linx_fdiv)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_div(env, a, b, srctype);
}

uint64_t HELPER(linx_fabs)(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    return linx_fp_unop_fabs(env, a, srctype);
}

uint64_t HELPER(linx_feq)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_eq(env, a, b, srctype);
}

uint64_t HELPER(linx_flt)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_lt(env, a, b, srctype);
}

uint64_t HELPER(linx_fge)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_ge(env, a, b, srctype);
}

uint64_t HELPER(linx_fcvt)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvt(env, a, dsttype, srctype);
}

uint64_t HELPER(linx_fcvtz)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvtz(env, a, dsttype, srctype);
}

uint64_t HELPER(linx_scvtf)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_scvtf(env, a, dsttype, srctype);
}

uint64_t HELPER(linx_ucvtf)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_ucvtf(env, a, dsttype, srctype);
}

void HELPER(linx_ebreak)(CPULinxState *env, uint32_t imm)
{
    CPUState *cs = env_cpu(env);
    
    qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK imm=%d, a0=0x%lx, a1=0x%lx, a2=0x%lx\n",
                  imm, (unsigned long)env->gpr[LINX_REG_A0],
                  (unsigned long)env->gpr[LINX_REG_A1],
                  (unsigned long)env->gpr[LINX_REG_A2]);
    
    switch (imm) {
    case LINX_SEMIHOST_EXIT:
        /* Exit program - graceful shutdown */
        qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK EXIT at PC=0x%lx\n",
                      (unsigned long)env->pc);
        cs->exception_index = LINX_EXCP_BREAKPOINT;
        cpu_loop_exit_restore(cs, GETPC());
        break;
        
    case LINX_SEMIHOST_PUTCHAR: {
        /* Output single character from a0 */
        int ch = env->gpr[LINX_REG_A0] & 0xff;
        qemu_log_mask(CPU_LOG_INT, "Linx: PUTCHAR '%c' (0x%02x)\n", 
                      (ch >= 32 && ch < 127) ? ch : '.', ch);
        /* Write to stderr for immediate visibility */
        fputc(ch, stderr);
        fflush(stderr);
        env->gpr[LINX_REG_A0] = ch;  /* Return the character */
        return;  /* Continue execution */
    }
        
    case LINX_SEMIHOST_WRITE: {
        /* Write buffer: a0=fd (ignored, always stderr), a1=buf, a2=len */
        uint64_t buf_addr = env->gpr[LINX_REG_A1];
        uint64_t len = env->gpr[LINX_REG_A2];
        uint64_t i;
        
        qemu_log_mask(CPU_LOG_INT, "Linx: WRITE buf=0x%lx len=%lu\n",
                      (unsigned long)buf_addr, (unsigned long)len);
        
        /* Read and output each byte from guest memory */
        for (i = 0; i < len; i++) {
            uint8_t ch = cpu_ldub_data(env, buf_addr + i);
            fputc(ch, stderr);
        }
        fflush(stderr);
        env->gpr[LINX_REG_A0] = len;  /* Return bytes written */
        return;  /* Continue execution */
    }
        
    case LINX_SEMIHOST_READ: {
        /* Read not implemented for now - return 0 */
        env->gpr[LINX_REG_A0] = 0;
        return;
    }
        
    default:
        /* Unhandled semihosting operation - treat as breakpoint */
        qemu_log_mask(LOG_GUEST_ERROR, 
                      "Linx: Unhandled EBREAK imm=%d at PC=0x%lx\n",
                      imm, (unsigned long)env->pc);
        cs->exception_index = LINX_EXCP_BREAKPOINT;
        cpu_loop_exit_restore(cs, GETPC());
        break;
    }
}

void HELPER(raise_exception)(CPULinxState *env, uint32_t exception)
{
    CPUState *cs = env_cpu(env);
    cs->exception_index = exception;
    cpu_loop_exit_restore(cs, GETPC());
}

/* ------------------------------------------------------------------------- */
/* Tile block helpers (TAU bring-up)                                         */
/* ------------------------------------------------------------------------- */

enum {
    LINX_BLOCK_STD  = 0,
    LINX_BLOCK_TMA  = 2,
    LINX_BLOCK_CUBE = 6,
};

enum {
    LINX_TMA_TLOAD  = 0,
    LINX_TMA_TSTORE = 1,
};

enum {
    LINX_CUBE_MAMULB = 0,
    LINX_CUBE_ACCCVT = 8,
};

enum {
    LINX_IOT_S0V = 1u << 0,
    LINX_IOT_S1V = 1u << 1,
    LINX_IOT_S0R = 1u << 2,
    LINX_IOT_S1R = 1u << 3,
};

static inline uint32_t linx_tile_read32(CPULinxState *env, uint64_t addr)
{
    return cpu_ldl_le_data(env, (abi_ptr)addr);
}

static inline void linx_tile_write32(CPULinxState *env, uint64_t addr, uint32_t v)
{
    cpu_stl_le_data(env, (abi_ptr)addr, v);
}

static void linx_tile_load(CPULinxState *env, unsigned dst_tile, unsigned addr_reg)
{
    if (dst_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    size_t bytes;
    const unsigned size_code = env->tile_iot_size & 0x1f;
    if (size_code == 0) {
        bytes = 4096;
    } else {
        bytes = (size_t)1ull << size_code;
        if (bytes > 4096) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }
    if ((bytes & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const size_t words = bytes / 4u;

    const uint64_t base = env->gpr[addr_reg];
    for (size_t i = 0; i < words; i++) {
        env->tile_reg[dst_tile][i] = linx_tile_read32(env, base + (uint64_t)i * 4u);
    }
    for (size_t i = words; i < 1024; i++) {
        env->tile_reg[dst_tile][i] = 0;
    }
}

static void linx_tile_store(CPULinxState *env, unsigned src_tile, unsigned addr_reg)
{
    if (src_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    size_t bytes;
    const unsigned size_code = env->tile_iot_size & 0x1f;
    if (size_code == 0) {
        bytes = 4096;
    } else {
        bytes = (size_t)1ull << size_code;
        if (bytes > 4096) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }
    if ((bytes & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const size_t words = bytes / 4u;

    const uint64_t base = env->gpr[addr_reg];
    for (size_t i = 0; i < words; i++) {
        linx_tile_write32(env, base + (uint64_t)i * 4u, env->tile_reg[src_tile][i]);
    }
}

static void linx_tile_mamulb(CPULinxState *env, unsigned src_a, unsigned src_b)
{
    if (src_a >= 32 || src_b >= 32) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned m = env->lb[0] ? MIN((unsigned)env->lb[0], 8u) : 8u;
    const unsigned n = env->lb[1] ? MIN((unsigned)env->lb[1], 8u) : 8u;
    const unsigned kdim = env->lb[2] ? MIN((unsigned)env->lb[2], 8u) : 8u;

    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            int64_t acc = 0;
            for (unsigned k = 0; k < kdim; k++) {
                const int32_t a = (int32_t)env->tile_reg[src_a][i * 8u + k];
                const int32_t b = (int32_t)env->tile_reg[src_b][k * 8u + j];
                acc += (int64_t)a * (int64_t)b;
            }
            env->tile_acc[i * 8u + j] = (uint32_t)(int32_t)acc;
        }
    }

    /* Zero the rest of the accumulator for determinism. */
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 8; j++) {
            if (i < m && j < n) {
                continue;
            }
            env->tile_acc[i * 8u + j] = 0;
        }
    }
    for (unsigned i = 64; i < 1024; i++) {
        env->tile_acc[i] = 0;
    }
}

static void linx_tile_acccvt(CPULinxState *env, unsigned dst_tile)
{
    if (dst_tile >= 32) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    for (unsigned i = 0; i < 1024; i++) {
        env->tile_reg[dst_tile][i] = env->tile_acc[i];
    }
}

void HELPER(linx_tile_commit)(CPULinxState *env)
{
    if (env->tile_iot_valid == 0) {
        return;
    }

    switch (env->blocktype) {
    case LINX_BLOCK_TMA:
        switch (env->tile_func & 0x1f) {
        case LINX_TMA_TLOAD:
            linx_tile_load(env,
                           ((env->tile_iot_grp & 0x1u) << 3) | (env->tile_iot_dst & 0x7u),
                           env->tile_iot_reg & 0x1f);
            break;
        case LINX_TMA_TSTORE:
            if (env->tile_iot_flags & LINX_IOT_S0V) {
                linx_tile_store(env, env->tile_iot_src0 & 0x1f, env->tile_iot_reg & 0x1f);
            } else {
                /* Backward compatibility: treat dst+grp as a TU tile index. */
                linx_tile_store(env,
                                ((env->tile_iot_grp & 0x1u) << 3) | (env->tile_iot_dst & 0x7u),
                                env->tile_iot_reg & 0x1f);
            }
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    case LINX_BLOCK_CUBE:
        switch (env->tile_func & 0x1f) {
        case LINX_CUBE_MAMULB:
            if ((env->tile_iot_flags & (LINX_IOT_S0V | LINX_IOT_S1V)) !=
                (LINX_IOT_S0V | LINX_IOT_S1V)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            linx_tile_mamulb(env, env->tile_iot_src0 & 0x1f, env->tile_iot_src1 & 0x1f);
            break;
        case LINX_CUBE_ACCCVT:
            linx_tile_acccvt(env,
                             16u | ((env->tile_iot_grp & 0x1u) << 3) | (env->tile_iot_dst & 0x7u));
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    default:
        /* Non-tile blocks: nothing to do. */
        break;
    }

    /* Consume the per-block descriptor. */
    env->tile_iot_valid = 0;
    env->tile_iot_size = 0;
    env->tile_iot_grp = 0;
}


static unsigned linx_insn_len(uint16_t hw)
{
    if ((hw & 0x1) == 0) {
        return ((hw & 0xf) == 0xe) ? 6 : 2;
    }
    return ((hw & 0xf) == 0xf) ? 8 : 4;
}

static bool linx_is_bstart_at_addr(CPULinxState *env, uint64_t pc)
{
    CPUState *cs = env_cpu(env);
    uint8_t buf[8];

    if (cpu_memory_rw_debug(cs, pc, buf, 2, 0) != 0) {
        return false;
    }

    const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    const unsigned len = linx_insn_len(hw);

    if (len == 2) {
        /* C.BSTART.STD / C.BSTART.FP: mask=0xc7ff, BrType in bits [13:11] */
        if ((hw & 0xc7ff) == 0x0000 || (hw & 0xc7ff) == 0x0080) {
            const uint8_t brtype = (hw >> 11) & 0x7;
            if (brtype != 0) {
                return true;
            }
        }

        /* C.BSTART DIRECT/COND: distinguish by low nibble */
        if ((hw & 0x000f) == 0x0002 || (hw & 0x000f) == 0x0004) {
            return true;
        }

        /* Common fixed fall-through markers for non-STD block types. */
        switch (hw) {
        case 0x0840: /* C.BSTART.SYS FALL */
        case 0x08c0: /* C.BSTART.MPAR FALL */
        case 0x48c0: /* C.BSTART.MSEQ FALL */
        case 0x88c0: /* C.BSTART.VPAR FALL */
        case 0xc8c0: /* C.BSTART.VSEQ FALL */
            return true;
        default:
            return false;
        }
    }

    if (len == 4) {
        if (cpu_memory_rw_debug(cs, pc, buf, 4, 0) != 0) {
            return false;
        }
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

        /* BSTART.*: bits[6:0]=0x01, branch kind in bits [14:12] is non-zero. */
        if ((insn & 0x7f) == 0x01 && ((insn >> 12) & 0x7) != 0) {
            return true;
        }

        /* Template blocks: FENTRY/FEXIT/FRET.* share opcode bits[6:0]=0x41. */
        if ((insn & 0x7f) == 0x41 && ((insn >> 12) & 0x7) <= 3) {
            return true;
        }

        return false;
    }

    if (len == 6) {
        if (cpu_memory_rw_debug(cs, pc, buf, 6, 0) != 0) {
            return false;
        }

        const uint16_t prefix = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        const uint32_t main32 = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8) |
                                ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24);
        if ((prefix & 0xf) != 0xe) {
            return false;
        }

        /* HL.BSTART.*: encoded as a 16-bit prefix + 32-bit BSTART main part. */
        if ((main32 & 0xff) == 0x01 && ((main32 >> 12) & 0x7) != 0) {
            return true;
        }
        return false;
    }

    return false;
}

void HELPER(linx_check_bstart_target)(CPULinxState *env, uint64_t target)
{
    /*
     * This helper is on the hot path for indirect control flow (RET/IND/ICALL
     * and template returns). Cache the most recently-validated targets to avoid
     * re-reading guest memory for tight call/return loops.
     *
     * Note: This cache is conservative for typical bare-metal workloads (code
     * is not self-modifying). If guest code changes, TB invalidation will
     * naturally trigger re-translation, but this cache may still accept a
     * previously-validated address until reset.
     */
    for (size_t i = 0; i < ARRAY_SIZE(env->bstart_cache); i++) {
        if (env->bstart_cache[i] == target) {
            return;
        }
    }

    if (linx_is_bstart_at_addr(env, target)) {
        env->bstart_cache[env->bstart_cache_next & (ARRAY_SIZE(env->bstart_cache) - 1)] = target;
        env->bstart_cache_next++;
        return;
    }

    CPUState *cs = env_cpu(env);

    {
        uint8_t buf[8] = { 0 };
        int rc = cpu_memory_rw_debug(cs, target, buf, sizeof(buf), 0);
        if (rc == 0) {
            const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            const unsigned len = linx_insn_len(hw);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: target bytes @0x%" PRIx64 ": %02x %02x %02x %02x %02x %02x %02x %02x (hw=0x%04x len=%u)\n",
                          target, buf[0], buf[1], buf[2], buf[3],
                          buf[4], buf[5], buf[6], buf[7], hw, len);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: target bytes @0x%" PRIx64 ": <unreadable>\n",
                          target);
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: invalid branch target 0x%" PRIx64 " (not a block start marker)\n",
                  target);
    cs->exception_index = LINX_EXCP_BAD_BRANCH_TARGET;
    cpu_loop_exit_restore(cs, GETPC());
}

void HELPER(linx_watch_store)(CPULinxState *env, uint64_t pc, uint64_t addr,
                              uint64_t val, uint32_t size)
{
    CPUState *cs = env_cpu(env);
    static int do_abort = -1;

    if (do_abort < 0) {
        const char *mode = getenv("LINX_WATCH_STORE_MODE");
        do_abort = 1;
        if (mode && mode[0] && strcmp(mode, "0") != 0 &&
            (strcmp(mode, "log") == 0 || strcmp(mode, "LOG") == 0)) {
            do_abort = 0;
        }
    }

    fprintf(stderr,
            "Linx: WATCH store pc=0x%016" PRIx64 " addr=0x%016" PRIx64
            " size=%u val=0x%016" PRIx64 "\n",
            pc, addr, size, val);
    fflush(stderr);

    if (do_abort) {
        cpu_abort(cs, "Linx: watched store");
    }
}

void HELPER(linx_watch_load)(CPULinxState *env, uint64_t pc, uint64_t addr,
                             uint64_t val, uint32_t size)
{
    CPUState *cs = env_cpu(env);
    static int do_abort = -1;

    if (do_abort < 0) {
        const char *mode = getenv("LINX_WATCH_LOAD_MODE");
        do_abort = 1;
        if (mode && mode[0] && strcmp(mode, "0") != 0 &&
            (strcmp(mode, "log") == 0 || strcmp(mode, "LOG") == 0)) {
            do_abort = 0;
        }
    }

    fprintf(stderr,
            "Linx: WATCH load pc=0x%016" PRIx64 " addr=0x%016" PRIx64
            " size=%u val=0x%016" PRIx64 "\n",
            pc, addr, size, val);
    fflush(stderr);

    if (do_abort) {
        cpu_abort(cs, "Linx: watched load");
    }
}

void HELPER(linx_trace_ra)(CPULinxState *env, uint64_t pc, uint32_t what,
                           uint64_t v0, uint64_t v1)
{
    const char *tag = "ra";
    switch (what) {
    case 1:
        tag = "setret";
        break;
    case 2:
        tag = "fentry-save";
        break;
    case 3:
        tag = "fret-restore";
        break;
    case 4:
        tag = "call-commit";
        break;
    default:
        break;
    }

    fprintf(stderr,
            "Linx: TRACE %s pc=0x%016" PRIx64 " sp=0x%016" PRIx64
            " ra=0x%016" PRIx64 " brtype=%u cond=%u carg=0x%08x tgt=0x%016" PRIx64
            " v0=0x%016" PRIx64 " v1=0x%016" PRIx64 "\n",
            tag, pc, env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->brtype & 0x7u, env->cond, env->carg, env->tgt, v0, v1);
    fflush(stderr);
}

void HELPER(linx_trace_reg)(CPULinxState *env, uint64_t pc, uint32_t what,
                            uint32_t reg, uint64_t v0, uint64_t v1)
{
    const char *tag = "reg";
    switch (what) {
    case 1:
        tag = "set";
        break;
    case 2:
        tag = "fentry-save";
        break;
    case 3:
        tag = "fret-restore";
        break;
    default:
        break;
    }

    fprintf(stderr,
            "Linx: TRACE %s r%u pc=0x%016" PRIx64 " sp=0x%016" PRIx64
            " ra=0x%016" PRIx64 " brtype=%u cond=%u carg=0x%08x tgt=0x%016" PRIx64
            " v0=0x%016" PRIx64 " v1=0x%016" PRIx64 "\n",
            tag, reg, pc, env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->brtype & 0x7u, env->cond, env->carg, env->tgt, v0, v1);
    fflush(stderr);
}

/*
 * Immediate exit helper - called when guest requests exit via EBREAK imm=0.
 * This function ensures QEMU terminates immediately by:
 * 1. Requesting a graceful shutdown
 * 2. Calling cpu_loop_exit to break out of the execution loop
 */
void HELPER(linx_exit)(CPULinxState *env)
{
    CPUState *cs = env_cpu(env);
    
    qemu_log_mask(CPU_LOG_INT, "Linx: EXIT request at PC=0x%lx\n",
                  (unsigned long)env->pc);

    /* Always print dynamic instruction count for benchmark runs. */
    fprintf(stderr, "LINX_INSN_COUNT=%" PRIu64 "\n", env->insn_count);
    fflush(stderr);
    
    /* Request graceful shutdown of the VM */
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    
    /* Exit via the normal exception path. */
    cs->exception_index = LINX_EXCP_BREAKPOINT;
    cpu_loop_exit_restore(cs, GETPC());
}
