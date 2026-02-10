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
#include "accel/accel-cpu-ops.h"
#include "fpu/softfloat-helpers.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "exec/memopidx.h"
#include "accel/tcg/cpu-ldst-common.h"
#include "exec/cputlb.h"
#include "exec/target_page.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include <inttypes.h>

/* Configured in target/linx/translate.c from $LINX_CALLFRAME_SIZE. */
extern uint64_t linx_callframe_size;

static bool linx_trace_mmu_inited;
static bool linx_trace_mmu_enabled;

static inline bool linx_trace_mmu(void)
{
    if (!linx_trace_mmu_inited) {
        const char *v = getenv("LINX_TRACE_MMU");
        linx_trace_mmu_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_trace_mmu_inited = true;
    }
    return linx_trace_mmu_enabled;
}

static bool linx_trace_ra_inited;
static bool linx_trace_ra_enabled;
static bool linx_trace_ra_pc_filter_enabled;
static uint64_t linx_trace_ra_pc;
static bool linx_print_insn_count_inited;
static bool linx_print_insn_count_enabled;

static inline bool linx_trace_ra_match(uint64_t pc)
{
    if (!linx_trace_ra_inited) {
        const char *v = getenv("LINX_TRACE_RA");
        linx_trace_ra_enabled = v && v[0] && strcmp(v, "0") != 0;

        const char *pc_s = getenv("LINX_TRACE_RA_PC");
        if (pc_s && pc_s[0] && strcmp(pc_s, "0") != 0) {
            char *endp = NULL;
            errno = 0;
            uint64_t parsed = strtoull(pc_s, &endp, 0);
            if (errno == 0 && endp && endp != pc_s && *endp == '\0') {
                linx_trace_ra_pc = parsed;
                linx_trace_ra_pc_filter_enabled = true;
            }
        }

        linx_trace_ra_inited = true;
    }

    if (!linx_trace_ra_enabled) {
        return false;
    }
    return !linx_trace_ra_pc_filter_enabled || pc == linx_trace_ra_pc;
}

static inline bool linx_print_insn_count(void)
{
    if (!linx_print_insn_count_inited) {
        const char *v = getenv("LINX_PRINT_INSN_COUNT");
        linx_print_insn_count_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_print_insn_count_inited = true;
    }
    return linx_print_insn_count_enabled;
}

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
    LINX_SSR_ETEMP    = 0xF05,
    LINX_SSR_ETEMP0   = 0xF06,
    LINX_SSR_IPENDING = 0xF08,
    LINX_SSR_EOIEI    = 0xF0A,
    LINX_SSR_TTBR0    = 0xF10,
    LINX_SSR_TTBR1    = 0xF11,
    LINX_SSR_TCR      = 0xF12,
    LINX_SSR_MAIR     = 0xF13,
    LINX_SSR_IOTTBR   = 0xF14,
    LINX_SSR_IOTCR    = 0xF15,
    LINX_SSR_IOMAIR   = 0xF16,
    LINX_SSR_TIMER_TIME   = 0xF20,
    LINX_SSR_TIMER_TIMECMP = 0xF21,

    /* EBARG register group (v0.2). */
    LINX_SSR_EBARG0          = 0xF40,
    LINX_SSR_EBARG_BPC_CUR   = 0xF41,
    LINX_SSR_EBARG_BPC_TGT   = 0xF42,
    LINX_SSR_EBARG_TPC       = 0xF43,
    LINX_SSR_EBARG_LRA       = 0xF44,
    LINX_SSR_EBARG_TQ0       = 0xF45,
    LINX_SSR_EBARG_TQ1       = 0xF46,
    LINX_SSR_EBARG_TQ2       = 0xF47,
    LINX_SSR_EBARG_TQ3       = 0xF48,
    LINX_SSR_EBARG_UQ0       = 0xF49,
    LINX_SSR_EBARG_UQ1       = 0xF4A,
    LINX_SSR_EBARG_UQ2       = 0xF4B,
    LINX_SSR_EBARG_UQ3       = 0xF4C,
    LINX_SSR_EBARG_LB        = 0xF4D,
    LINX_SSR_EBARG_LC        = 0xF4E,
    LINX_SSR_EBARG_EXT_PTR   = 0xF4F,
    LINX_SSR_EBARG_EXT_META  = 0xF50,

    /* Debug SSR bank (v0.2). */
    LINX_SSR_DBGID           = 0xF80,
    LINX_SSR_DBCR0           = 0xF90,
    LINX_SSR_DBVR0           = 0xF91,
    LINX_SSR_DCCR0           = 0xFA0,
    LINX_SSR_DCVR0           = 0xFA1,
    LINX_SSR_DWCR0           = 0xFB0,
    LINX_SSR_DWVR0           = 0xFB1,
};

/* ECSTATE bits (v0.2 bring-up profile; mirrors key CSTATE fields). */
#define LINX_ECSTATE_BI_BIT        (1ULL << 62)

/* TRAPNO encoding (v0.2 bring-up profile; keep in sync with target/linx/cpu.c). */
#define LINX_TRAPNO_E_BIT          (1ULL << 63) /* 1=async interrupt */
#define LINX_TRAPNO_ARGV_BIT       (1ULL << 62)
#define LINX_TRAPNO_CAUSE_SHIFT    24u
#define LINX_TRAPNO_CAUSE_MASK     0xFFFFFFu
#define LINX_TRAPNO_TRAPNUM_MASK   0x3Fu

static inline uint64_t linx_trapno_make(bool async, bool argv, uint32_t cause, uint8_t trapnum)
{
    const uint64_t e = async ? LINX_TRAPNO_E_BIT : 0;
    const uint64_t a = argv ? LINX_TRAPNO_ARGV_BIT : 0;
    const uint64_t c = ((uint64_t)(cause & LINX_TRAPNO_CAUSE_MASK)) << LINX_TRAPNO_CAUSE_SHIFT;
    const uint64_t t = (uint64_t)(trapnum & LINX_TRAPNO_TRAPNUM_MASK);
    return e | a | c | t;
}

static void linx_commit_trace_init(CPULinxState *env)
{
    if (env->commit_trace.inited) {
        return;
    }
    env->commit_trace.inited = 1;
    env->commit_trace.stop_after_commit = 0;

    const char *path = getenv("LINX_COMMIT_TRACE");
    if (!path || !path[0] || strcmp(path, "0") == 0) {
        env->commit_trace.enabled = 0;
        return;
    }

    env->commit_trace.fp = fopen(path, "w");
    if (!env->commit_trace.fp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: failed to open LINX_COMMIT_TRACE='%s'\n",
                      path);
        env->commit_trace.enabled = 0;
        return;
    }

    env->commit_trace.enabled = 1;
    env->commit_trace.cycle = 0;

    const char *lo_s = getenv("LINX_COMMIT_TRACE_FILTER_PC_LO");
    if (lo_s && lo_s[0] && strcmp(lo_s, "0") != 0) {
        char *endp = NULL;
        errno = 0;
        uint64_t lo = strtoull(lo_s, &endp, 0);
        if (errno == 0 && endp && endp != lo_s && *endp == '\0') {
            uint64_t hi = lo;
            const char *hi_s = getenv("LINX_COMMIT_TRACE_FILTER_PC_HI");
            if (hi_s && hi_s[0] && strcmp(hi_s, "0") != 0) {
                char *endp2 = NULL;
                errno = 0;
                uint64_t parsed = strtoull(hi_s, &endp2, 0);
                if (errno == 0 && endp2 && endp2 != hi_s && *endp2 == '\0') {
                    hi = parsed;
                }
            }
            env->commit_trace.pc_lo = MIN(lo, hi);
            env->commit_trace.pc_hi = MAX(lo, hi);
            env->commit_trace.pc_filter_enabled = 1;
        }
    }
}

static inline bool linx_commit_trace_active(CPULinxState *env)
{
    linx_commit_trace_init(env);
    return env->commit_trace.enabled && env->commit_trace.fp;
}

static inline void linx_trace_wb(CPULinxState *env, uint32_t rd, uint64_t data)
{
    if (!linx_commit_trace_active(env)) {
        return;
    }
    env->trace_wb_valid = 1;
    env->trace_wb_rd = rd;
    env->trace_wb_data = data;
}

static inline void linx_trace_mem(CPULinxState *env, bool is_store,
                                  uint64_t addr, uint64_t wdata,
                                  uint64_t rdata, uint32_t size)
{
    if (!linx_commit_trace_active(env)) {
        return;
    }
    env->trace_mem_valid = 1;
    env->trace_mem_addr = addr;
    env->trace_mem_size = size;
    env->trace_mem_wdata = is_store ? wdata : 0;
    env->trace_mem_rdata = is_store ? 0 : rdata;
}

static inline void linx_template_commit_and_exit(CPULinxState *env,
                                                 CPUState *cs,
                                                 uint64_t next_pc)
{
    if (linx_commit_trace_active(env)) {
        HELPER(linx_commit_trace)(env, next_pc);
    }
    cpu_loop_exit_noexc(cs);
}

void HELPER(linx_commit_trace)(CPULinxState *env, uint64_t next_pc)
{
    linx_commit_trace_init(env);
    if (!env->commit_trace.enabled || !env->commit_trace.fp) {
        return;
    }

    const uint64_t pc = env->trace_pc;
    if (env->commit_trace.pc_filter_enabled &&
        (pc < env->commit_trace.pc_lo || pc > env->commit_trace.pc_hi)) {
        return;
    }

    const uint64_t cycle = env->commit_trace.cycle++;
    const uint32_t trap_valid = env->trace_trap_valid;
    const uint32_t trap_cause = env->trace_trap_cause;
    const uint8_t trapnum = (uint8_t)(trap_cause & 0xffu);
    const uint32_t cause = (uint32_t)((trap_cause >> 8) & 0xffu);
    const bool argv = trap_valid != 0; /* commit-trace: treat TRAPARG0 as present when trap_valid */
    const uint64_t trapno_full = trap_valid ? linx_trapno_make(false, argv, cause, trapnum) : 0;

    /* Mandatory schema fields (see linxisa/docs/bringup/contracts/trace_schema.md). */
    fprintf(env->commit_trace.fp,
            "{\"cycle\":%" PRIu64
            ",\"pc\":%" PRIu64
            ",\"insn\":%" PRIu64
            ",\"wb_valid\":%u,\"wb_rd\":%u,\"wb_data\":%" PRIu64
            ",\"mem_valid\":%u,\"mem_addr\":%" PRIu64
            ",\"mem_wdata\":%" PRIu64 ",\"mem_rdata\":%" PRIu64 ",\"mem_size\":%u"
            ",\"trap_valid\":%u,\"trap_cause\":%u"
            ",\"trapno_full\":%" PRIu64 ",\"traparg0\":%" PRIu64
            ",\"next_pc\":%" PRIu64 "}\n",
            cycle,
            pc,
            env->trace_insn,
            env->trace_wb_valid, env->trace_wb_rd, env->trace_wb_data,
            env->trace_mem_valid, env->trace_mem_addr,
            env->trace_mem_wdata, env->trace_mem_rdata, env->trace_mem_size,
            trap_valid, trap_cause,
            trapno_full, env->trace_traparg0,
            next_pc);
    fflush(env->commit_trace.fp);

    if (env->commit_trace.stop_after_commit) {
        fclose(env->commit_trace.fp);
        env->commit_trace.fp = NULL;
        env->commit_trace.enabled = 0;
        env->commit_trace.stop_after_commit = 0;
    }
}

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
     * v0.2 bring-up profile: if an interrupt routes to a more privileged ACR, it may
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
    if (env->ssr_acr[dst_acr][LINX_SSR_IPENDING] == 0) {
        return;
    }
    /*
     * Latch CPU_INTERRUPT_HARD whenever a source is pending.
     *
     * Delivery permission (CSTATE.I / ring checks) is enforced later in
     * cpu_exec_interrupt(). Keeping the request latched avoids losing pending
     * IRQs across ACR transitions where permission flips after trap return.
     */
    generic_handle_interrupt(cs, CPU_INTERRUPT_HARD);
}

/* ACRC request_type values (v0.2 bring-up profile). */
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

static inline void linx_raise_illegal_inst(CPULinxState *env)
{
    env->pending_trap_arg0 = 0;
    env->pending_trap_cause = 0;
    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
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
            /* v0.2: legacy trap-save SSRs are illegal. */
            if (idx == 0xF0B || idx == 0xF0C || idx == 0xF0D || idx == 0xF0E) {
                env->pending_trap_arg0 = 0;
                env->pending_trap_cause = 0;
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return 0;
            }
            if (idx == LINX_SSR_TIMER_TIME) {
                return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
            if (idx == LINX_SSR_DBGID) {
                const uint64_t cps_minus1 = 0; /* CPs=1 */
                const uint64_t bps_minus1 = 3; /* BPs=4 */
                const uint64_t wps_minus1 = 3; /* WPs=4 */
                return (cps_minus1 << 0) | (bps_minus1 << 4) | (wps_minus1 << 8);
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
         * interrupts and there is a pending interrupt for the external
         * interrupt routing ring (ACR1),
         * kick the CPU so it can be taken.
         */
        env->ssr[idx] = value;
        env->acr = linx_cstate_get_acr(value);
        linx_irq_kick_if_allowed(env, 1);
        return;
    default:
        if (is_manager) {
            if (bank >= LINX_ACR_COUNT) {
                return;
            }

            /* v0.2: legacy trap-save SSRs are illegal. */
            if (idx == 0xF0B || idx == 0xF0C || idx == 0xF0D || idx == 0xF0E) {
                env->pending_trap_arg0 = 0;
                env->pending_trap_cause = 0;
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (idx == LINX_SSR_DBGID) {
                env->pending_trap_arg0 = 0;
                env->pending_trap_cause = 0;
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }

            if (linx_trace_mmu()) {
                switch (idx) {
                case LINX_SSR_TTBR0:
                case LINX_SSR_TTBR1:
                case LINX_SSR_TCR:
                case LINX_SSR_MAIR:
                case LINX_SSR_IOTTBR:
                case LINX_SSR_IOTCR:
                case LINX_SSR_IOMAIR: {
                    const char *name =
                        (idx == LINX_SSR_TTBR0) ? "TTBR0" :
                        (idx == LINX_SSR_TTBR1) ? "TTBR1" :
                        (idx == LINX_SSR_TCR) ? "TCR" :
                        (idx == LINX_SSR_MAIR) ? "MAIR" :
                        (idx == LINX_SSR_IOTTBR) ? "IOTTBR" :
                        (idx == LINX_SSR_IOTCR) ? "IOTCR" :
                        "IOMAIR";
                    fprintf(stderr,
                            "linx: ssr_write %-6s ssrid=0x%06" PRIx32 " bank=%u idx=0x%03" PRIx32
                            " val=0x%016" PRIx64 "\n",
                            name, ssrid, bank, idx, value);
                    fflush(stderr);
                    break;
                }
                default:
                    break;
                }
            }

            if (bank == 1) {
                /*
                 * ACR1 privileged MMU/IOMMU programming registers: validate the
                 * v0.2 bring-up subset and flush translations on updates.
                 */
                if (idx == LINX_SSR_TCR) {
                    const uint64_t allowed =
                        (1ull << 0) | (0x3full << 1) | (0x3full << 7) |
                        (1ull << 13) | (1ull << 14) | (1ull << 15);
                    if ((value & ~allowed) != 0) {
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    tlb_flush(env_cpu(env));
                    return;
                }
                if (idx == LINX_SSR_IOTCR) {
                    const uint64_t allowed = (1ull << 0) | (0x3full << 1);
                    if ((value & ~allowed) != 0) {
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    return;
                }
                if (idx == LINX_SSR_TTBR0 || idx == LINX_SSR_TTBR1 || idx == LINX_SSR_IOTTBR) {
                    if ((value & 0xfffu) != 0) {
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    tlb_flush(env_cpu(env));
                    return;
                }
            }

            if (idx == LINX_SSR_EOIEI) {
                /*
                 * End of interrupt (v0.2 bring-up profile): clear the pending bit for the
                 * given interrupt ID.
                 *
                 * Keep line level and pending latch separate:
                 * - IPENDING is software-cleared via EOIEI.
                 * - irq_level_acr[] reflects current external line level.
                 *
                 * If a level source is still asserted when EOIEI executes,
                 * immediately re-pend it so completion interrupts cannot be
                 * lost due to short deassert/reassert windows.
                 */
                CPUState *cs = env_cpu(env);
                const uint32_t irq_id = (uint32_t)value & 63u;
                const uint64_t bit = (1ull << irq_id);

                env->ssr_acr[bank][LINX_SSR_IPENDING] &= ~bit;
                if (env->irq_level_acr[bank] & bit) {
                    env->ssr_acr[bank][LINX_SSR_IPENDING] |= bit;
                }

                if (env->ssr_acr[bank][LINX_SSR_IPENDING] == 0) {
                    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
                } else {
                    linx_irq_kick_if_allowed(env, bank);
                }
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

                if (bank == 1 && env->timer) {
                    CPUState *cs = env_cpu(env);
                    if (value == 0) {
                        timer_del(env->timer);
                        env->ssr_acr[1][LINX_SSR_IPENDING] &= ~(1ull << 0);
                        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
                        return;
                    }

                    const uint64_t now = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                    if (value <= now) {
                        env->ssr_acr[1][LINX_SSR_IPENDING] |= (1ull << 0);
                        linx_irq_kick_if_allowed(env, 1);
                        return;
                    }
                    timer_mod_ns(env->timer, (int64_t)value);
                }
                return;
            }

            /* Debug SSR validation (v0.2 bring-up subset). */
            if ((idx >= 0xF90 && idx <= 0xF97) || /* DBCR/DBVR[0..3] */
                (idx >= 0xFA0 && idx <= 0xFA1) || /* DCCR/DCVR[0] */
                (idx >= 0xFB0 && idx <= 0xFB7)    /* DWCR/DWVR[0..3] */
                ) {
                const bool is_ctrl = ((idx & 1u) == 0);
                if (is_ctrl) {
                    const uint64_t E = (value >> 0) & 1u;
                    const uint64_t MT = (value >> 1) & 1u;
                    const uint64_t ML = (value >> 2) & 1u;
                    const uint64_t LE_or_LT = (value >> 3) & 1u;
                    const uint64_t ls = (value >> 4) & 3u;
                    const uint64_t mln = (value >> 51) & 0xFu;
                    const uint64_t mask = (value >> 55) & 0x1Fu;

                    (void)E;
                    (void)mask;

                    /* Only Address Match / Context Match is implemented: MT must be 0. */
                    if (MT != 0) {
                        linx_raise_illegal_inst(env);
                    }

                    if (idx >= 0xF90 && idx <= 0xF97) {
                        /* DBCR<n>: allow only defined bits; ML implies MLN in range (CP0 only). */
                        const uint64_t allowed =
                            (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) |
                            (0xFull << 51) | (0x1Full << 55);
                        if ((value & ~allowed) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                        if (ML && mln != 0) {
                            linx_raise_illegal_inst(env);
                        }
                    } else if (idx >= 0xFA0 && idx <= 0xFA1) {
                        /* DCCR0: only support LC match profile (MC=0, CT=0). */
                        const uint64_t allowed =
                            (0x3ull << 6) | (0x3ull << 4) | (1ull << 3) | (1ull << 1) | (1ull << 0);
                        if ((value & ~allowed) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                        if (((value >> 6) & 0x3u) != 0 || ((value >> 4) & 0x3u) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                    } else {
                        /* DWCR<n>: require context linking only when ML=1; validate reserved bits. */
                        const uint64_t allowed =
                            (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) |
                            (0x3ull << 4) |
                            (0xFull << 51) | (0x1Full << 55);
                        if ((value & ~allowed) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                        if (ML) {
                            const uint64_t LT = LE_or_LT;
                            if (LT != 1 || mln != 0) {
                                linx_raise_illegal_inst(env);
                            }
                        }
                        /* LS is only advisory in bring-up; accept any encoding (including 0). */
                        (void)ls;
                    }
                }
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

void HELPER(linx_tlb_iall)(CPULinxState *env)
{
    tlb_flush(env_cpu(env));
}

/* ------------------------------------------------------------------------- */
/* Debug helpers (v0.2 bring-up subset)                                      */
/* ------------------------------------------------------------------------- */

static inline bool linx_dbg_addr_match(uint64_t a, uint64_t b, uint32_t mask_bits)
{
    if (mask_bits >= 63) {
        return true;
    }
    const uint64_t m = (mask_bits == 0) ? 0 : ((1ull << mask_bits) - 1ull);
    return (a & ~m) == (b & ~m);
}

static inline bool linx_dbg_ctx_match(CPULinxState *env, uint32_t acr, uint32_t cp_idx)
{
    if (cp_idx != 0) {
        return false;
    }
    const uint64_t dccr = env->ssr_acr[acr][LINX_SSR_DCCR0];
    const uint64_t dcvr = env->ssr_acr[acr][LINX_SSR_DCVR0];
    const uint64_t E = (dccr >> 0) & 1u;
    const uint64_t MT = (dccr >> 1) & 1u;
    if (!E || MT != 0) {
        return false;
    }
    const uint64_t lc0 = (dcvr >> 0) & 0xffffu;
    const uint64_t lc1 = (dcvr >> 16) & 0xffffu;
    const uint64_t lc2 = (dcvr >> 32) & 0xffffu;
    return ((env->lc[0] & 0xffffu) == lc0) &&
           ((env->lc[1] & 0xffffu) == lc1) &&
           ((env->lc[2] & 0xffffu) == lc2);
}

void HELPER(linx_dbg_check_pc)(CPULinxState *env, uint64_t pc)
{
    CPUState *cs = env_cpu(env);
    const uint32_t acr = env->acr & 0xFu;

    for (uint32_t n = 0; n < 4; n++) {
        const uint32_t cr_idx = LINX_SSR_DBCR0 + 2u * n;
        const uint32_t vr_idx = LINX_SSR_DBVR0 + 2u * n;
        const uint64_t cr = env->ssr_acr[acr][cr_idx];
        const uint64_t E = (cr >> 0) & 1u;
        if (!E) {
            continue;
        }
        const uint64_t MT = (cr >> 1) & 1u;
        if (MT != 0) {
            continue;
        }
        const uint64_t ML = (cr >> 2) & 1u;
        const uint64_t LE = (cr >> 3) & 1u;
        const uint32_t mln = (uint32_t)((cr >> 51) & 0xFu);
        const uint32_t mask = (uint32_t)((cr >> 55) & 0x1Fu);
        (void)LE;

        const uint64_t vr = env->ssr_acr[acr][vr_idx];
        if (!linx_dbg_addr_match(pc, vr, mask)) {
            continue;
        }

        if (ML) {
            if (!linx_dbg_ctx_match(env, acr, mln)) {
                continue;
            }
        }

        env->pending_trap_arg0 = pc;
        env->pending_trap_cause = n & 0xFu;
        cs->exception_index = LINX_EXCP_HW_BREAKPOINT;
        cpu_loop_exit_restore(cs, GETPC());
    }
}

static inline void linx_dbg_check_mem(CPULinxState *env, uint64_t addr, uint32_t size,
                                      bool is_store)
{
    CPUState *cs = env_cpu(env);
    const uint32_t acr = env->acr & 0xFu;
    (void)size;

    for (uint32_t n = 0; n < 4; n++) {
        const uint32_t cr_idx = LINX_SSR_DWCR0 + 2u * n;
        const uint32_t vr_idx = LINX_SSR_DWVR0 + 2u * n;
        const uint64_t cr = env->ssr_acr[acr][cr_idx];
        const uint64_t E = (cr >> 0) & 1u;
        if (!E) {
            continue;
        }
        const uint64_t MT = (cr >> 1) & 1u;
        if (MT != 0) {
            continue;
        }
        const uint64_t ML = (cr >> 2) & 1u;
        const uint64_t LT = (cr >> 3) & 1u;
        const uint32_t ls = (uint32_t)((cr >> 4) & 0x3u);
        const uint32_t mln = (uint32_t)((cr >> 51) & 0xFu);
        const uint32_t mask = (uint32_t)((cr >> 55) & 0x1Fu);

        const bool allow = (ls == 0) ? true :
                           (ls == 1) ? !is_store :
                           (ls == 2) ? is_store :
                           true;
        if (!allow) {
            continue;
        }

        const uint64_t vr = env->ssr_acr[acr][vr_idx];
        if (!linx_dbg_addr_match(addr, vr, mask)) {
            continue;
        }

        if (ML) {
            if (LT != 1 || !linx_dbg_ctx_match(env, acr, mln)) {
                continue;
            }
        }

        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = n & 0xFu;
        cs->exception_index = LINX_EXCP_HW_WATCHPOINT;
        cpu_loop_exit_restore(cs, GETPC());
    }
}

void HELPER(linx_dbg_check_load)(CPULinxState *env, uint64_t pc, uint64_t addr, uint32_t size)
{
    (void)pc;
    linx_dbg_check_mem(env, addr, size, false);
}

void HELPER(linx_dbg_check_store)(CPULinxState *env, uint64_t pc, uint64_t addr, uint32_t size)
{
    (void)pc;
    linx_dbg_check_mem(env, addr, size, true);
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
    uint64_t src_cstate = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], src_acr);
    /* v0.2: ACRC traps are always reported as block-body traps. */
    src_cstate |= LINX_ECSTATE_BI_BIT;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: SERVICE_REQUEST src_acr=%u req=%u bpc=0x%" PRIx64 " tpc=0x%" PRIx64
                  " pc_next=0x%" PRIx64 "\n",
                  src_acr, request_type, bpc, tpc, pc_next);

    /* ACRC request_type validity + routing (bring-up profile; see linxisa manual). */
    if (src_acr == 1) {
        if (request_type != LINX_SCT_MAC && request_type != LINX_SCT_SEC) {
            cs->exception_index = LINX_EXCP_ILLEGAL_INST;
            cpu_loop_exit(cs);
        }
        dst_acr = 0;
    } else if (src_acr == 2) {
        if (request_type != LINX_SCT_MAC && request_type != LINX_SCT_SYS && request_type != LINX_SCT_SEC) {
            cs->exception_index = LINX_EXCP_ILLEGAL_INST;
            cpu_loop_exit(cs);
        }
        /* v0.2 bring-up: ACR2 + SCT_SYS routes to ACR1; others route to ACR0. */
        dst_acr = (request_type == LINX_SCT_SYS) ? 1 : 0;
    } else {
        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
        cpu_loop_exit(cs);
    }

    /*
     * Preserve block/queue state for the trapped ACR so we can resume the
     * interrupted block after returning via ACRE. Without this, the kernel's
     * own block headers clobber the user's commit metadata (brtype/tgt/cond)
     * and hand queues, breaking post-syscall control flow and any mid-block
     * trap return.
     */
    linx_acr_save_block_state(env, src_acr);
    linx_acr_restore_block_state(env, dst_acr);

    /* Save trap state into the managing ACR bank (v0.2: EBARG + TRAPNO). */
    env->ssr_acr[dst_acr][LINX_SSR_ECSTATE] = src_cstate;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG0] = (uint64_t)(env->blocktype & 0x1fu);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_CUR] = bpc;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_TGT] = pc_next;
    /* v0.2: ACRC resume PC is the following instruction (bring-up: explicit BSTOP). */
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TPC] = pc_next;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LRA] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ0] = env->tq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ1] = env->tq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ2] = env->tq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ3] = env->tq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ0] = env->uq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ1] = env->uq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ2] = env->uq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ3] = env->uq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LB] =
        ((env->lb[0] & 0xffffu) << 0) | ((env->lb[1] & 0xffffu) << 16) | ((env->lb[2] & 0xffffu) << 32);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LC] =
        ((env->lc[0] & 0xffffu) << 0) | ((env->lc[1] & 0xffffu) << 16) | ((env->lc[2] & 0xffffu) << 32);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_PTR] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_META] = 0;

    /* Trap reporting (v0.2 bring-up encoding). */
    env->ssr_acr[dst_acr][LINX_SSR_TRAPNO] =
        linx_trapno_make(false, true, (uint32_t)request_type, 6 /* SYSCALL */);
    env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = (uint64_t)request_type;

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
    CPUState *cs = env_cpu(env);
    const uint32_t mgr = env->acr & 0xFu;
    const uint64_t ecstate = env->ssr_acr[mgr][LINX_SSR_ECSTATE];
    const uint32_t target = linx_cstate_get_acr(ecstate);
    const bool bi = (ecstate & LINX_ECSTATE_BI_BIT) != 0;
    const uint64_t resume_bpc = env->ssr_acr[mgr][LINX_SSR_EBARG_BPC_CUR];
    const uint64_t resume_tpc = env->ssr_acr[mgr][LINX_SSR_EBARG_TPC];
    const uint64_t resume_pc = bi ? resume_tpc : resume_bpc;

    if (getenv("LINX_TRACE_ACR_ENTER")) {
        if (mgr != target || getenv("LINX_TRACE_ACR_ENTER_VERBOSE")) {
            fprintf(stderr,
                    "Linx: ACR_ENTER mgr=%u -> target=%u rra=%u bi=%u"
                    " resume_pc=0x%016" PRIx64
                    " resume_bpc=0x%016" PRIx64
                    " resume_tpc=0x%016" PRIx64
                    " a0=0x%016" PRIx64
                    " ecstate=0x%016" PRIx64 "\n",
                    mgr, target, rra_type, bi ? 1u : 0u,
                    resume_pc, resume_bpc, resume_tpc,
                    env->gpr[LINX_REG_A0], ecstate);
            fflush(stderr);
        }
    }

    /*
     * v0.2 bring-up: ACR_ENTER may keep privilege or drop privilege.
     * Entering a more-privileged ring directly from software is invalid.
     */
    if (target >= LINX_ACR_COUNT || target < mgr) {
        env->pending_trap_arg0 = (uint64_t)target;
        env->pending_trap_cause = 0;
        helper_raise_exception(env, LINX_EXCP_EXEC_STATE_CHECK);
        return;
    }

    /*
     * Trap return / ACR handoff.
     *
     * For transitions across ACRs (mgr != target), save the current block state
     * in the manager bank and restore the target ACR's saved state.
     *
     * For same-ACR returns (mgr == target), do *not* overwrite the interrupted
     * context's saved state. The interrupt/trap entry path already saved the
     * pre-trap block/template state into acr_block_state[mgr]; restoring that
     * state is required to resume an interrupted restartable template without
     * clobbering progress when the handler itself executes template blocks.
     */
    if (target != mgr) {
        linx_acr_save_block_state(env, mgr);
    }
    linx_acr_restore_block_state(env, target);

    /* v0.2 ACRE(RRA) behavior: DEFAULT resets BSTATE; RESTORE uses EBARG snapshot. */
    if (rra_type == 0) {
        int i;
        for (i = 0; i < 4; i++) {
            env->tq[i] = 0;
            env->uq[i] = 0;
        }
        env->tgt = 0;
        env->cond = 0;
        env->carg = 0;
        env->brtype = 0;
        env->blocktype = 0;
        env->body_tpc = 0;
        env->return_pc = 0;
        env->in_body = 0;
        env->tmpl_pc = 0;
        env->tmpl_kind = 0;
        env->tmpl_step = 0;
        env->tmpl_reg_cur = 0;
        env->tmpl_reg_begin = 0;
        env->tmpl_reg_end = 0;
        env->tmpl_stacksize = 0;
        env->tmpl_mem_dst = 0;
        env->tmpl_mem_src = 0;
        env->tmpl_mem_remaining = 0;
        env->tmpl_mem_value = 0;
        for (i = 0; i < 3; i++) {
            env->lb[i] = 0;
            env->lc[i] = 0;
        }
    } else if (rra_type == 1) {
        const uint64_t lb = env->ssr_acr[mgr][LINX_SSR_EBARG_LB];
        const uint64_t lc = env->ssr_acr[mgr][LINX_SSR_EBARG_LC];
        env->tq[0] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ0];
        env->tq[1] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ1];
        env->tq[2] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ2];
        env->tq[3] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ3];
        env->uq[0] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ0];
        env->uq[1] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ1];
        env->uq[2] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ2];
        env->uq[3] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ3];
        env->lb[0] = (lb >> 0) & 0xffffu;
        env->lb[1] = (lb >> 16) & 0xffffu;
        env->lb[2] = (lb >> 32) & 0xffffu;
        env->lc[0] = (lc >> 0) & 0xffffu;
        env->lc[1] = (lc >> 16) & 0xffffu;
        env->lc[2] = (lc >> 32) & 0xffffu;
    } else {
        env->pending_trap_arg0 = (uint64_t)rra_type;
        env->pending_trap_cause = 0;
        helper_raise_exception(env, LINX_EXCP_EXEC_STATE_CHECK);
        return;
    }

    /* v0.2: always restore BPC from EBARG. */
    env->bpc = resume_bpc;

    env->acr = target;
    env->ssr[LINX_SSR_CSTATE] = ecstate & ~LINX_ECSTATE_BI_BIT;
    env->pc = resume_pc;
    /*
     * External IRQs route to ACR1 in the bring-up profile.
     * Re-latch a pending request after privilege/state restore.
     */
    linx_irq_kick_if_allowed(env, 1);

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
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        cpu_loop_exit_noexc(cs);
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
        /* Unhandled semihosting operation - treat as a software breakpoint trap. */
        qemu_log_mask(LOG_GUEST_ERROR, 
                      "Linx: Unhandled EBREAK imm=%d at PC=0x%lx\n",
                      imm, (unsigned long)env->pc);
        env->pending_trap_cause = imm & 0xffu;
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

/* ------------------------------------------------------------------------- */
/* Restartable template blocks                                               */
/* ------------------------------------------------------------------------- */

static inline int linx_next_fentry_reg(int current)
{
    current++;
    if (current > 23) {
        current = 2;
    }
    return current;
}

static inline int linx_fentry_reg_count(int begin, int end)
{
    if (begin <= end) {
        return end - begin + 1;
    }
    return (23 - begin + 1) + (end - 2 + 1);
}

static inline void linx_template_clear(CPULinxState *env)
{
    env->tmpl_pc = 0;
    env->tmpl_kind = 0;
    env->tmpl_step = 0;
    env->tmpl_reg_cur = 0;
    env->tmpl_reg_begin = 0;
    env->tmpl_reg_end = 0;
    env->tmpl_stacksize = 0;
    env->tmpl_mem_dst = 0;
    env->tmpl_mem_src = 0;
    env->tmpl_mem_remaining = 0;
    env->tmpl_mem_value = 0;
}

static inline uint8_t linx_extctx_byte(const CPULinxState *env, uint64_t ext_kind, uint64_t off)
{
    static const uint8_t magic[8] = { 'L', 'I', 'N', 'X', '_', 'E', 'X', 'T' };

    if (off < 8) {
        return magic[off];
    }
    if (off < 16) {
        const unsigned sh = (unsigned)((off - 8) * 8u);
        return (uint8_t)((ext_kind >> sh) & 0xffu);
    }
    if (off < 40) {
        const unsigned idx = (unsigned)((off - 16) / 8u);
        const unsigned sh = (unsigned)(((off - 16) % 8u) * 8u);
        return (uint8_t)((env->lb[idx] >> sh) & 0xffu);
    }
    if (off < 64) {
        const unsigned idx = (unsigned)((off - 40) / 8u);
        const unsigned sh = (unsigned)(((off - 40) % 8u) * 8u);
        return (uint8_t)((env->lc[idx] >> sh) & 0xffu);
    }
    return 0;
}

static inline void linx_extctx_write_byte(CPULinxState *env, uint64_t off, uint8_t v)
{
    if (off >= 16 && off < 40) {
        const unsigned idx = (unsigned)((off - 16) / 8u);
        const unsigned sh = (unsigned)(((off - 16) % 8u) * 8u);
        env->lb[idx] = (env->lb[idx] & ~(0xffull << sh)) | ((uint64_t)v << sh);
        return;
    }
    if (off >= 40 && off < 64) {
        const unsigned idx = (unsigned)((off - 40) / 8u);
        const unsigned sh = (unsigned)(((off - 40) % 8u) * 8u);
        env->lc[idx] = (env->lc[idx] & ~(0xffull << sh)) | ((uint64_t)v << sh);
        return;
    }
}

void HELPER(linx_template_step)(CPULinxState *env, uint32_t kind,
                                uint64_t cur_pc, uint64_t next_pc,
                                uint32_t op0, uint32_t op1, uint64_t op2)
{
    CPUState *cs = env_cpu(env);

    if (env->tmpl_pc != cur_pc || env->tmpl_kind != kind) {
        env->tmpl_pc = cur_pc;
        env->tmpl_kind = kind;
        env->tmpl_step = 0;
        env->tmpl_reg_cur = 0;
        env->tmpl_reg_begin = 0;
        env->tmpl_reg_end = 0;
        env->tmpl_stacksize = 0;
        env->tmpl_mem_dst = 0;
        env->tmpl_mem_src = 0;
        env->tmpl_mem_remaining = 0;
        env->tmpl_mem_value = 0;

        switch (kind) {
        case LINX_TEMPLATE_FENTRY:
        case LINX_TEMPLATE_FEXIT:
        case LINX_TEMPLATE_FRET_RA:
        case LINX_TEMPLATE_FRET_STK:
            env->tmpl_reg_begin = op0;
            env->tmpl_reg_end = op1;
            env->tmpl_reg_cur = op0;
            env->tmpl_stacksize = op2;
            break;

        case LINX_TEMPLATE_MCOPY: {
            const uint32_t dst_reg = op0;
            const uint32_t src_reg = op1;
            const uint32_t size_reg = (uint32_t)op2;
            if (dst_reg >= LINX_GPR_COUNT || src_reg >= LINX_GPR_COUNT ||
                size_reg >= LINX_GPR_COUNT) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            env->tmpl_mem_dst = env->gpr[dst_reg];
            env->tmpl_mem_src = env->gpr[src_reg];
            env->tmpl_mem_remaining = env->gpr[size_reg];
            break;
        }

        case LINX_TEMPLATE_MSET: {
            const uint32_t dst_reg = op0;
            const uint32_t val_reg = op1;
            const uint32_t size_reg = (uint32_t)op2;
            if (dst_reg >= LINX_GPR_COUNT || val_reg >= LINX_GPR_COUNT ||
                size_reg >= LINX_GPR_COUNT) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            env->tmpl_mem_dst = env->gpr[dst_reg];
            env->tmpl_mem_value = env->gpr[val_reg] & 0xffu;
            env->tmpl_mem_remaining = env->gpr[size_reg];
            break;
        }

        case LINX_TEMPLATE_ESAVE:
        case LINX_TEMPLATE_ERCOV: {
            const uint32_t base_reg = op0;
            const uint32_t len_reg = op1;
            const uint32_t kind_reg = (uint32_t)op2;
            if (base_reg >= LINX_GPR_COUNT || len_reg >= LINX_GPR_COUNT ||
                kind_reg >= LINX_GPR_COUNT) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            env->tmpl_mem_dst = env->gpr[base_reg];
            env->tmpl_mem_remaining = env->gpr[len_reg];
            env->tmpl_mem_value = env->gpr[kind_reg];
            break;
        }

        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
    }

    switch (kind) {
    case LINX_TEMPLATE_FENTRY: {
        const uint64_t stacksize = env->tmpl_stacksize;
        const uint64_t adj = stacksize + linx_callframe_size;
        const int begin = (int)env->tmpl_reg_begin;
        const int end = (int)env->tmpl_reg_end;
        const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
        const uint32_t step = env->tmpl_step;

        if (step == 0) {
            if (adj) {
                const uint64_t old_sp = env->gpr[LINX_REG_SP];
                env->gpr[LINX_REG_SP] -= adj;
                linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
                if (linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 2, old_sp, env->gpr[LINX_REG_SP]);
                }
            }
            env->tmpl_step = 1;

            if (stacksize == 0 || count == 0) {
                linx_template_clear(env);
                env->pc = next_pc;
            } else {
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* step >= 1: save one register per step. */
        {
            const int64_t off = (int64_t)stacksize - ((int64_t)step * 8);
            const int reg = (int)env->tmpl_reg_cur;

            if (off < 0) {
                linx_template_clear(env);
                env->pc = next_pc;
                linx_template_commit_and_exit(env, cs, env->pc);
            }

            if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t addr = env->gpr[LINX_REG_SP] + (uint64_t)off;
                const uint64_t v = env->gpr[reg];
                linx_trace_mem(env, true, addr, v, 0, 8);
                cpu_stq_le_data(env, (abi_ptr)addr, env->gpr[reg]);
                if (reg == LINX_REG_RA && linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 2, addr, v);
                }
            }

            if (reg == end) {
                linx_template_clear(env);
                env->pc = next_pc;
            } else {
                env->tmpl_reg_cur = (uint32_t)linx_next_fentry_reg(reg);
                env->tmpl_step = step + 1;
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }
        break;
    }

    case LINX_TEMPLATE_FEXIT:
    case LINX_TEMPLATE_FRET_RA:
    case LINX_TEMPLATE_FRET_STK: {
        const uint64_t stacksize = env->tmpl_stacksize;
        const uint64_t adj = stacksize + linx_callframe_size;
        const int begin = (int)env->tmpl_reg_begin;
        const int end = (int)env->tmpl_reg_end;
        const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
        const uint32_t step = env->tmpl_step;

        if (count && step < (uint32_t)count) {
            const int reg = (int)env->tmpl_reg_cur;
            const int64_t off = (int64_t)stacksize - ((int64_t)(step + 1) * 8);

            if (off >= 0 && reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t addr = env->gpr[LINX_REG_SP] + (uint64_t)off;
                const uint64_t v = cpu_ldq_le_data(env, (abi_ptr)addr);
                env->gpr[reg] = v;
                linx_trace_mem(env, false, addr, 0, v, 8);
                linx_trace_wb(env, (uint32_t)reg, v);
                if (reg == LINX_REG_RA && linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 3, addr, v);
                }
            }

            if (reg != end) {
                env->tmpl_reg_cur = (uint32_t)linx_next_fentry_reg(reg);
            }
            env->tmpl_step = step + 1;
            env->pc = cur_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* After restoring regs: adjust SP and either fall through or return. */
        if (adj) {
            env->gpr[LINX_REG_SP] += adj;
            linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
        }

        if (kind == LINX_TEMPLATE_FEXIT) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        {
            const uint64_t ra = env->gpr[LINX_REG_RA];
            HELPER(linx_check_bstart_target)(env, ra);
            linx_template_clear(env);
            env->pc = ra;
            linx_template_commit_and_exit(env, cs, env->pc);
        }
        break;
    }

    case LINX_TEMPLATE_MCOPY: {
        uint64_t dst = env->tmpl_mem_dst;
        uint64_t src = env->tmpl_mem_src;
        uint64_t remaining = env->tmpl_mem_remaining;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /*
         * One restartable step per helper invocation so commit-tracing can
         * treat each step like a single committed micro-op.
         *
         * Trace convention: record the destination store only (the source read
         * is internal and not representable in the single mem_* slot schema).
         */
        uint32_t sz = 1;
        if (remaining >= 8) {
            sz = 8;
        } else if (remaining >= 4) {
            sz = 4;
        } else if (remaining >= 2) {
            sz = 2;
        }

        uint64_t v = 0;
        switch (sz) {
        case 8:
            v = cpu_ldq_le_data(env, (abi_ptr)src);
            cpu_stq_le_data(env, (abi_ptr)dst, v);
            break;
        case 4:
            v = cpu_ldl_le_data(env, (abi_ptr)src);
            cpu_stl_le_data(env, (abi_ptr)dst, (uint32_t)v);
            break;
        case 2:
            v = cpu_lduw_le_data(env, (abi_ptr)src);
            cpu_stw_le_data(env, (abi_ptr)dst, (uint16_t)v);
            break;
        default:
            v = cpu_ldub_data(env, (abi_ptr)src);
            cpu_stb_data(env, (abi_ptr)dst, (uint8_t)v);
            break;
        }
        linx_trace_mem(env, true, dst, v, 0, sz);

        src += sz;
        dst += sz;
        remaining -= sz;
        env->tmpl_mem_src = src;
        env->tmpl_mem_dst = dst;
        env->tmpl_mem_remaining = remaining;
        env->tmpl_step += sz;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
        } else {
            env->pc = cur_pc;
        }
        linx_template_commit_and_exit(env, cs, env->pc);
        break;
    }

    case LINX_TEMPLATE_MSET: {
        uint64_t dst = env->tmpl_mem_dst;
        uint64_t remaining = env->tmpl_mem_remaining;
        const uint8_t v = (uint8_t)env->tmpl_mem_value;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        uint32_t sz = 1;
        if (remaining >= 8) {
            sz = 8;
        } else if (remaining >= 4) {
            sz = 4;
        } else if (remaining >= 2) {
            sz = 2;
        }

        uint64_t pat = 0;
        for (uint32_t i = 0; i < sz; i++) {
            pat |= (uint64_t)v << (i * 8u);
        }
        switch (sz) {
        case 8:
            cpu_stq_le_data(env, (abi_ptr)dst, pat);
            break;
        case 4:
            cpu_stl_le_data(env, (abi_ptr)dst, (uint32_t)pat);
            break;
        case 2:
            cpu_stw_le_data(env, (abi_ptr)dst, (uint16_t)pat);
            break;
        default:
            cpu_stb_data(env, (abi_ptr)dst, (uint8_t)pat);
            break;
        }
        linx_trace_mem(env, true, dst, pat, 0, sz);

        dst += sz;
        remaining -= sz;
        env->tmpl_mem_dst = dst;
        env->tmpl_mem_remaining = remaining;
        env->tmpl_step += sz;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
        } else {
            env->pc = cur_pc;
        }
        linx_template_commit_and_exit(env, cs, env->pc);
        break;
    }

    case LINX_TEMPLATE_ESAVE:
    case LINX_TEMPLATE_ERCOV: {
        const uint64_t base = env->tmpl_mem_dst;
        uint64_t remaining = env->tmpl_mem_remaining;
        const uint64_t ext_kind = env->tmpl_mem_value;
        uint64_t off = env->tmpl_step;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        const uint64_t addr = base + off;
        uint8_t byte = 0;

        /*
         * Bring-up ext-context blob (64 bytes, little-endian fields):
         *  [0..7]   magic "LINX_EXT"
         *  [8..15]  ext_kind (operand RegSrc2)
         *  [16..39] LB0/LB1/LB2 (u64 each)
         *  [40..63] LC0/LC1/LC2 (u64 each)
         *
         * Bytes beyond 64 are zero on ESAVE and ignored on ERCOV.
         *
         * Use a byte-at-a-time restartable transfer to keep fault/interrupt
         * restart semantics deterministic (idempotent on restart).
         */
        if (kind == LINX_TEMPLATE_ESAVE) {
            byte = linx_extctx_byte(env, ext_kind, off);
            cpu_stb_data(env, (abi_ptr)addr, byte);
            linx_trace_mem(env, true, addr, byte, 0, 1);
        } else {
            byte = cpu_ldub_data(env, (abi_ptr)addr);
            linx_trace_mem(env, false, addr, 0, byte, 1);
            linx_extctx_write_byte(env, off, byte);
        }

        off += 1;
        remaining -= 1;
        env->tmpl_step = (uint32_t)off;
        env->tmpl_mem_remaining = remaining;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
        } else {
            env->pc = cur_pc;
        }
        linx_template_commit_and_exit(env, cs, env->pc);
        break;
    }

    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        break;
    }

    g_assert_not_reached();
}

enum {
    LINX_TRAPCAUSE_CAT_IOMMU_PF = 3,
    LINX_TRAPCAUSE_ACC_LOAD    = 0,
    LINX_TRAPCAUSE_ACC_STORE   = 1,
};

static inline bool linx_iova_is_canonical(uint64_t va)
{
    const uint64_t top = (va >> 48) & 0xffffu;
    const uint64_t sign = (va >> 47) & 1u;
    return top == (sign ? 0xffffu : 0x0000u);
}

static bool linx_iommu_translate(CPULinxState *env, uint64_t iova,
                                 bool is_store, hwaddr *pa_out)
{
    const uint64_t iotcr = env->ssr_acr[1][LINX_SSR_IOTCR];
    const bool ime = (iotcr & 1u) != 0;

    if (!ime) {
        /* Bring-up: identity translation, with the NOMMU physical mask. */
        *pa_out = (hwaddr)(iova & 0x1fffffffULL);
        return true;
    }

    if (!linx_iova_is_canonical(iova)) {
        return false;
    }

    /* v0.2 bring-up subset: only 48-bit IOVA supported (SZ must be 16). */
    const uint32_t sz = (uint32_t)((iotcr >> 1) & 0x3fu);
    if (sz != 16) {
        return false;
    }

    const uint64_t iottbr = env->ssr_acr[1][LINX_SSR_IOTTBR];
    if ((iottbr & 0xfffu) != 0) {
        return false;
    }

    hwaddr table = (hwaddr)(iottbr & 0x0000fffffffff000ULL);

    for (int level = 0; level < 4; level++) {
        const uint32_t shift = 39u - (uint32_t)level * 9u;
        const uint64_t idx = (iova >> shift) & 0x1ffu;
        const hwaddr desc_addr = table + (hwaddr)(idx * 8u);
        MemTxResult result = MEMTX_OK;
        const uint64_t desc = address_space_ldq_le(&address_space_memory, desc_addr,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            return false;
        }

        const uint32_t type = (uint32_t)(desc & 0x3u);
        if (type == 0) {
            return false;
        }

        if (type == 3) {
            /* Table descriptor. */
            if ((desc & 0xffcULL) != 0) {
                return false;
            }
            if ((desc >> 48) != 0) {
                return false;
            }
            table = (hwaddr)(desc & 0x0000fffffffff000ULL);
            continue;
        }

        /* Leaf descriptor: Page at L3, Block at L1/L2 (optional). */
        if (level == 0) {
            return false;
        }

        hwaddr block_size = TARGET_PAGE_SIZE;
        if (type == 2) {
            if (level == 1) {
                block_size = (hwaddr)1ull << 30; /* 1 GiB */
            } else if (level == 2) {
                block_size = (hwaddr)1ull << 21; /* 2 MiB */
            } else {
                return false;
            }
        } else if (type == 1) {
            if (level != 3) {
                return false;
            }
        } else {
            return false;
        }

        const hwaddr out_base = (hwaddr)(desc & 0x0000fffffffff000ULL);
        if ((desc >> 48) != 0) {
            return false;
        }
        if ((out_base & (block_size - 1u)) != 0) {
            return false;
        }
        if ((desc & (3ull << 10)) != 0) {
            return false;
        }
        const uint32_t attridx = (uint32_t)((desc >> 7) & 0x7u);
        if (attridx > 2u) {
            return false;
        }
        const bool af = ((desc >> 6) & 1u) != 0;
        if (!af) {
            return false;
        }

        const bool w = ((desc >> 3) & 1u) != 0;
        const bool r = ((desc >> 2) & 1u) != 0;

        if (is_store && !w) {
            return false;
        }
        if (!is_store && !r) {
            return false;
        }

        const hwaddr pa = out_base | (hwaddr)(iova & (uint64_t)(block_size - 1u));
        if (((uint64_t)pa >> 48) != 0) {
            return false;
        }
        *pa_out = pa;
        return true;
    }

    return false;
}

static inline uint32_t linx_tile_read32(CPULinxState *env, uint64_t addr)
{
    hwaddr pa;
    if (!linx_iommu_translate(env, addr, false, &pa)) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_LOAD);
        helper_raise_exception(env, LINX_EXCP_LOAD_ACCESS_FAULT);
    }

    MemTxResult result = MEMTX_OK;
    const uint32_t v = address_space_ldl_le(&address_space_memory, pa,
                                           MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_LOAD);
        helper_raise_exception(env, LINX_EXCP_LOAD_ACCESS_FAULT);
    }
    return v;
}

static inline void linx_tile_write32(CPULinxState *env, uint64_t addr, uint32_t v)
{
    hwaddr pa;
    if (!linx_iommu_translate(env, addr, true, &pa)) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_STORE);
        helper_raise_exception(env, LINX_EXCP_STORE_ACCESS_FAULT);
    }

    MemTxResult result = MEMTX_OK;
    address_space_stl_le(&address_space_memory, pa, v,
                         MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_STORE);
        helper_raise_exception(env, LINX_EXCP_STORE_ACCESS_FAULT);
    }
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

        /* Template blocks: frame templates (0x41) and memory templates (0x31). */
        if ((insn & 0x7f) == 0x41 && ((insn >> 12) & 0x7) <= 3) {
            return true;
        }
        if ((insn & 0x7f) == 0x31 && ((insn >> 7) & 0x1f) == 0 &&
            ((insn >> 12) & 0x7) <= 1) {
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
    env->pending_trap_arg0 = target;
    env->pending_trap_cause = LINX_EBLOCK_CAUSE_BAD_BRANCH_TARGET;
    cs->exception_index = LINX_EXCP_BAD_BRANCH_TARGET;
    cpu_loop_exit_restore(cs, GETPC());
}

void HELPER(linx_watch_store)(CPULinxState *env, uint64_t pc, uint64_t addr,
                              uint64_t val, uint32_t size)
{
    CPUState *cs = env_cpu(env);
    static int do_abort = -1;
    static unsigned dump_count;

    if (do_abort < 0) {
        const char *mode = getenv("LINX_WATCH_STORE_MODE");
        do_abort = 1;
        if (mode && mode[0] && strcmp(mode, "0") != 0 &&
            (strcmp(mode, "log") == 0 || strcmp(mode, "LOG") == 0)) {
            do_abort = 0;
        }
    }

    fprintf(stderr,
            "Linx: WATCH store pc=0x%016" PRIx64 " ra=0x%016" PRIx64
            " sp=0x%016" PRIx64 " addr=0x%016" PRIx64
            " size=%u val=0x%016" PRIx64 "\n",
            pc, env->gpr[LINX_REG_RA], env->gpr[LINX_REG_SP], addr, size, val);
    if (dump_count < 4) {
        uint8_t raw[32 * 8];
        if (cpu_memory_rw_debug(cs, env->gpr[LINX_REG_SP], raw, sizeof(raw), 0) == 0) {
            for (int i = 0; i < 32; i++) {
                uint64_t w = ldl_le_p(raw + i * 8);
                w |= ((uint64_t)ldl_le_p(raw + i * 8 + 4) << 32);
                fprintf(stderr, "Linx: WATCH stack[%d]=0x%016" PRIx64 "\n", i, w);
            }
        }
        dump_count++;
    }
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

    if (linx_print_insn_count()) {
        fprintf(stderr, "LINX_INSN_COUNT=%" PRIu64 "\n", env->insn_count);
        fflush(stderr);
    }
    
    /* Request graceful shutdown of the VM */
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);

    /* Exit immediately from the execution loop. */
    cpu_loop_exit_noexc(cs);
}
