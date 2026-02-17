/*
 * QEMU LinxISA CPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "cpu.h"
#include "migration/vmstate.h"
#include "exec/cputlb.h"
#include "exec/memattrs.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "exec/log.h"
#include "fpu/softfloat-helpers.h"
#include "tcg/debug-assert.h"
#include "accel/accel-cpu-ops.h"
#include "accel/tcg/cpu-ops.h"
#include "system/runstate.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/memory.h"

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
    LINX_SSR_TIMECMP  = 0xF21,

    /* ACR1 privileged MMU/IOMMU registers (see linxisa manual). */
    LINX_SSR_TTBR0    = 0xF10,
    LINX_SSR_TTBR1    = 0xF11,
    LINX_SSR_TCR      = 0xF12,
    LINX_SSR_MAIR     = 0xF13,
    LINX_SSR_IOTTBR   = 0xF14,
    LINX_SSR_IOTCR    = 0xF15,
    LINX_SSR_IOMAIR   = 0xF16,

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

/* Common (non-banked) SSR indices. */
enum {
    LINX_SSR_CSTATE = 0x0020,
};

/* CSTATE bits (keep in sync with target/linx/helper.c). */
#define LINX_CSTATE_ACR_MASK 0xFULL
#define LINX_CSTATE_I_BIT    (1ULL << 4)

/* ECSTATE bits (v0.2 bring-up profile; mirrors key CSTATE fields). */
#define LINX_ECSTATE_BI_BIT        (1ULL << 62)

/* TRAPNO encoding (v0.2 bring-up profile). */
#define LINX_TRAPNO_E_BIT          (1ULL << 63) /* 1=async interrupt */
#define LINX_TRAPNO_ARGV_BIT       (1ULL << 62)
#define LINX_TRAPNO_CAUSE_SHIFT    24u
#define LINX_TRAPNO_CAUSE_MASK     0xFFFFFFu
#define LINX_TRAPNO_TRAPNUM_MASK   0x3Fu

enum {
    /* v0.2 bring-up trap major classes (TRAPNO.TRAPNUM). */
    LINX_TRAPNUM_EXEC_STATE_CHECK = 0,
    LINX_TRAPNUM_ILLEGAL_INST     = 4,
    LINX_TRAPNUM_BLOCK_TRAP       = 5,
    LINX_TRAPNUM_SYSCALL          = 6,
    LINX_TRAPNUM_INST_PC_FAULT    = 32,
    LINX_TRAPNUM_INST_PAGE_FAULT  = 33,
    LINX_TRAPNUM_DATA_ALIGN_FAULT = 34,
    LINX_TRAPNUM_DATA_PAGE_FAULT  = 35,
    LINX_TRAPNUM_INTERRUPT        = 44,
    LINX_TRAPNUM_HW_BREAKPOINT    = 49,
    LINX_TRAPNUM_SW_BREAKPOINT    = 50,
    LINX_TRAPNUM_HW_WATCHPOINT    = 51,
};

enum {
    LINX_TRAPCAUSE_CAT_NONE      = 0,
    LINX_TRAPCAUSE_CAT_MMU_PF    = 1,
    LINX_TRAPCAUSE_CAT_MMU_PERM  = 2,
    LINX_TRAPCAUSE_CAT_IOMMU_PF  = 3,
};

enum {
    LINX_TRAPCAUSE_ACC_LOAD  = 0,
    LINX_TRAPCAUSE_ACC_STORE = 1,
    LINX_TRAPCAUSE_ACC_INST  = 2,
};

static inline uint8_t linx_trapcause_make(uint8_t cat, uint8_t acc)
{
    return (uint8_t)((cat << 4) | (acc & 0xfu));
}

static inline uint64_t linx_trapno_make(bool async, bool argv, uint32_t cause, uint8_t trapnum)
{
    const uint64_t e = async ? LINX_TRAPNO_E_BIT : 0;
    const uint64_t a = argv ? LINX_TRAPNO_ARGV_BIT : 0;
    const uint64_t c = ((uint64_t)(cause & LINX_TRAPNO_CAUSE_MASK)) << LINX_TRAPNO_CAUSE_SHIFT;
    const uint64_t t = (uint64_t)(trapnum & LINX_TRAPNO_TRAPNUM_MASK);
    return e | a | c | t;
}

static bool linx_disable_timer_irq_inited;
static bool linx_disable_timer_irq;

static inline bool linx_timer_irq_enabled(void)
{
    if (!linx_disable_timer_irq_inited) {
        const char *v = getenv("LINX_DISABLE_TIMER_IRQ");
        linx_disable_timer_irq = v && v[0] && strcmp(v, "0") != 0;
        linx_disable_timer_irq_inited = true;
    }
    return !linx_disable_timer_irq;
}

/* Simple timer interrupt ID (bring-up). */
enum {
    LINX_IRQ_TIMER0 = 0,
};

static bool linx_mmu_translate(CPUState *cs, CPULinxState *env, vaddr va,
                               MMUAccessType access_type, int mmu_idx,
                               hwaddr *pa_out, int *prot_out,
                               hwaddr *tlb_size_out, uint8_t *cause_out);

static inline hwaddr linx_nommu_phys_addr(vaddr va)
{
    /*
     * NOMMU bring-up profile:
     * - keep identity mapping for normal low addresses (including MMIO windows),
     * - fold only sign-extended legacy 29-bit addresses back into the low
     *   physical region.
     */
    const uint64_t low_mask = 0x1fffffffULL;
    const uint64_t high_mask = ~low_mask;
    const uint64_t raw = (uint64_t)va;

    if ((raw & high_mask) == high_mask) {
        return (hwaddr)(raw & low_mask);
    }
    return (hwaddr)raw;
}

static inline uint64_t linx_cstate_set_acr(uint64_t cstate, uint32_t acr)
{
    return (cstate & ~LINX_CSTATE_ACR_MASK) | ((uint64_t)acr & LINX_CSTATE_ACR_MASK);
}

static inline bool linx_irq_allowed(const CPULinxState *env, uint32_t dst_acr)
{
    const uint32_t cur_acr = env->acr & 0xF;
    const uint64_t cstate = env->ssr[LINX_SSR_CSTATE];
    const bool ie = (cstate & LINX_CSTATE_I_BIT) != 0;

    if (dst_acr < cur_acr) {
        return true;
    }
    if (dst_acr == cur_acr) {
        return ie;
    }
    return ie;
}

static inline void linx_irq_kick_if_allowed(CPUState *cs, CPULinxState *env,
                                            uint32_t dst_acr)
{
    if (env->ssr_acr[dst_acr][LINX_SSR_IPENDING] == 0) {
        return;
    }
    /*
     * Latch CPU_INTERRUPT_HARD whenever a source is pending.
     *
     * Permission checks (CSTATE.I / ring rules) run in cpu_exec_interrupt();
     * keeping the request latched prevents pending IRQ loss across ACR changes.
     *
     * cpu_interrupt() requires the BQL. The timer callback can run without the
     * BQL, so use the lock-free helper.
     */
    generic_handle_interrupt(cs, CPU_INTERRUPT_HARD);
}

static void linx_timer_cb(void *opaque)
{
    CPUState *cs = opaque;
    LinxCPU *cpu = LINX_CPU(cs);
    CPULinxState *env = &cpu->env;

    if (!linx_timer_irq_enabled()) {
        return;
    }

    /* Set pending bit and raise a hard interrupt. */
    env->ssr_acr[1][LINX_SSR_IPENDING] |= (1ull << LINX_IRQ_TIMER0);
    linx_irq_kick_if_allowed(cs, env, 1);
}

static hwaddr linx_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    LinxCPU *cpu = LINX_CPU(cs);
    CPULinxState *env = &cpu->env;
    const uint64_t tcr = env->ssr_acr[1][LINX_SSR_TCR];
    const bool mme = (tcr & 1u) != 0;

    if (!mme) {
        return linx_nommu_phys_addr(addr);
    }

    /* Debug translation: attempt a best-effort walk using the current ACR. */
    hwaddr phys = 0;
    int prot = 0;
    hwaddr tlb_size = TARGET_PAGE_SIZE;
    uint8_t cause = 0;
    const int mmu_idx = ((env->acr & 0xFu) == 2) ? 1 : 0;

    if (!linx_mmu_translate(cs, env, addr, MMU_DATA_LOAD, mmu_idx,
                            &phys, &prot, &tlb_size, &cause)) {
        return (hwaddr)-1;
    }

    return phys & TARGET_PAGE_MASK;
}

static void linx_cpu_do_interrupt(CPUState *cs);

static void linx_cpu_set_pc(CPUState *cs, vaddr value)
{
    LinxCPU *cpu = LINX_CPU(cs);
    cpu->env.pc = value;
}

static vaddr linx_cpu_get_pc(CPUState *cs)
{
    LinxCPU *cpu = LINX_CPU(cs);
    return cpu->env.pc;
}

#define LINX_TB_FLAG_IN_BODY (1u << 0)

static TCGTBCPUState linx_get_tb_cpu_state(CPUState *cs)
{
    CPULinxState *env = cpu_env(cs);
    uint32_t flags = 0;
    if (env->in_body) {
        flags |= LINX_TB_FLAG_IN_BODY;
    }
    return (TCGTBCPUState){ .pc = env->pc, .flags = flags };
}

static void linx_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    LinxCPU *cpu = LINX_CPU(cs);

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.pc = tb->pc;
}

static void linx_restore_state_to_opc(CPUState *cs,
                                      const TranslationBlock *tb,
                                      const uint64_t *data)
{
    LinxCPU *cpu = LINX_CPU(cs);
    cpu->env.pc = data[0];
}

static bool linx_cpu_has_work(CPUState *cs)
{
    /*
     * Linx currently has no WFI/idle instruction: if the CPU is not halted,
     * it always has work. If it is halted, only interrupts/reset should wake it.
     */
    if (!cs->halted) {
        return true;
    }
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_RESET);
}

static bool linx_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        CPULinxState *env = cpu_env(cs);

        /*
         * The hard-interrupt request bit can lag behind IPENDING updates.
         * Do not deliver EXCP_INTERRUPT without a live pending source.
         */
        if (env->ssr_acr[1][LINX_SSR_IPENDING] == 0) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
            return false;
        }

        /* Route all external interrupts to EXCP_INTERRUPT for now. */
        cs->exception_index = EXCP_INTERRUPT;
        if (!linx_irq_allowed(env, 1)) {
            /* Leave the interrupt request pending until it becomes allowed. */
            cs->exception_index = -1;
            return false;
        }
        linx_cpu_do_interrupt(cs);
        return true;
    }
    return false;
}

static inline uint64_t linx_pack_u16x3(uint64_t a, uint64_t b, uint64_t c)
{
    return ((a & 0xffffu) << 0) | ((b & 0xffffu) << 16) | ((c & 0xffffu) << 32);
}

static void linx_deliver_sync_trap(CPUState *cs, CPULinxState *env,
                                   uint64_t tpc, uint64_t tpc_next,
                                   uint8_t trapnum,
                                   bool argv, bool is_trap, bool bi)
{
    /*
     * Deliver a synchronous exception via the bring-up trap SSRs and EVBASE.
     *
     * Note: this is a simplified model that routes all synchronous exceptions
     * (except those from ACR0) to ACR1, matching the bring-up defaults.
     */
    const uint32_t src_acr = env->acr & 0xFu;
    const uint32_t dst_acr = (src_acr == 0) ? 0 : 1;

    /* Capture trapped-from state before switching to the managing ACR. */
    uint64_t src_cstate = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], src_acr);
    if (bi) {
        src_cstate |= LINX_ECSTATE_BI_BIT;
    } else {
        src_cstate &= ~LINX_ECSTATE_BI_BIT;
    }
    const uint64_t src_bpc = env->bpc;

    if (getenv("LINX_TRACE_TRAP")) {
        fprintf(stderr,
                "Linx: deliver_sync_trap trapnum=%u src_acr=%u dst_acr=%u"
                " tpc=0x%016" PRIx64 " bpc=0x%016" PRIx64
                " cstate=0x%016" PRIx64 "\n",
                trapnum, src_acr, dst_acr, tpc, src_bpc, src_cstate);
        fflush(stderr);
    }

    linx_acr_save_block_state(env, src_acr);
    const LinxAcrBlockState *src_state = &env->acr_block_state[src_acr];
    linx_acr_restore_block_state(env, dst_acr);

    const uint64_t evbase = env->ssr_acr[dst_acr][LINX_SSR_EVBASE];

    env->ssr_acr[dst_acr][LINX_SSR_ECSTATE] = src_cstate;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG0] = (uint64_t)(src_state->blocktype & 0x1fu);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_CUR] = src_bpc;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_TGT] = tpc_next;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TPC] = is_trap ? tpc_next : tpc;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LRA] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ0] = src_state->tq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ1] = src_state->tq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ2] = src_state->tq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ3] = src_state->tq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ0] = src_state->uq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ1] = src_state->uq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ2] = src_state->uq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ3] = src_state->uq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LB] = linx_pack_u16x3(src_state->lb[0], src_state->lb[1], src_state->lb[2]);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LC] = linx_pack_u16x3(src_state->lc[0], src_state->lc[1], src_state->lc[2]);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_PTR] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_META] = 0;

    env->ssr_acr[dst_acr][LINX_SSR_TRAPNO] =
        linx_trapno_make(false, argv, env->pending_trap_cause, trapnum);
    env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = env->pending_trap_arg0;

    env->pending_trap_arg0 = 0;
    env->pending_trap_cause = 0;

    env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
    env->acr = dst_acr;
    env->ssr[LINX_SSR_CSTATE] =
        linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], dst_acr);
    env->pc = evbase ? evbase : tpc;
    cs->exception_index = -1;
}

static void linx_cpu_do_interrupt(CPUState *cs)
{
    CPULinxState *env = cpu_env(cs);
    int exception = cs->exception_index;
    uint64_t last_pc = env->pc;

    qemu_log_mask(CPU_LOG_INT, "Linx: exception %d at PC=0x%" PRIx64 "\n",
                  exception, last_pc);

    switch (exception) {
    case 0:
        /* exception_index = 0 shouldn't happen - treat as invalid */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: BUG: exception_index is 0 (invalid) at PC=0x%" PRIx64 "\n",
                      last_pc);
        cs->exception_index = -1;
        cpu_abort(cs, "Linx: BUG: exception_index is 0");
        return;

    case LINX_EXCP_BREAKPOINT:
        /* Software breakpoint trap (EBREAK). */
        env->pending_trap_arg0 = last_pc;
        /* pending_trap_cause may carry the imm value (profile-defined). */
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_SW_BREAKPOINT,
                               true,  /* argv */
                               true,  /* is_trap (resume at next PC) */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_BAD_BRANCH_TARGET:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: branch target violation at PC=0x%" PRIx64 "\n",
                      last_pc);
        linx_deliver_sync_trap(cs, env, last_pc, last_pc,
                               LINX_TRAPNUM_BLOCK_TRAP,
                               true,   /* argv */
                               false,  /* fault */
                               false   /* header */
                               );
        return;

    case LINX_EXCP_ILLEGAL_INST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: illegal instruction at PC=0x%" PRIx64 "\n",
                      last_pc);
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_ILLEGAL_INST,
                               false, /* argv */
                               false, /* fault */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_BLOCK_FAULT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: block-format fault at PC=0x%" PRIx64 "\n",
                      last_pc);
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_BLOCK_TRAP,
                               true,               /* argv */
                               false,              /* fault */
                               (env->in_body != 0) /* BI best-effort */
                               );
        return;

    case LINX_EXCP_HW_BREAKPOINT:
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_HW_BREAKPOINT,
                               true,  /* argv */
                               true,  /* trap */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_HW_WATCHPOINT:
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_HW_WATCHPOINT,
                               true,  /* argv */
                               true,  /* trap */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_EXEC_STATE_CHECK:
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_EXEC_STATE_CHECK,
                               false, /* argv */
                               false, /* fault */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_INST_ACCESS_FAULT:
    case LINX_EXCP_LOAD_ACCESS_FAULT:
    case LINX_EXCP_STORE_ACCESS_FAULT:
    {
        /* MMU/IOMMU faults are delivered as synchronous v0.2 page-fault classes. */
        const uint8_t trapnum =
            (exception == LINX_EXCP_INST_ACCESS_FAULT) ? LINX_TRAPNUM_INST_PAGE_FAULT : LINX_TRAPNUM_DATA_PAGE_FAULT;
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               trapnum,
                               true,  /* argv (TRAPARG0=fault VA) */
                               false, /* fault */
                               true   /* BI */
                               );
        return;
    }

    case EXCP_INTERRUPT:
    {
        /*
         * Hardware interrupt (bring-up): asynchronous interrupt routed to ACR1.
         *
         * v0.2 resume contract:
         * - Preserve BPC as the interrupted block start marker.
         * - Resume from TPC (BI=1) for normal instruction streams.
         * - For in-flight restartable templates, resume from template PC.
         */
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);

        const uint32_t src_acr = env->acr & 0xFu;
        const uint32_t dst_acr = 1;
        /*
         * Asynchronous IRQs are taken at instruction boundaries, so env->pc is
         * already the architectural resume address. Using insn_pc_next here can
         * skip an instruction if the interrupt is recognized between TBs.
         */
        const uint64_t resume_pc = env->pc;
        const uint64_t resume_bpc = env->bpc ? env->bpc : resume_pc;

        uint64_t src_cstate = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], src_acr);
        src_cstate |= LINX_ECSTATE_BI_BIT;

        linx_acr_save_block_state(env, src_acr);
        const LinxAcrBlockState *src_state = &env->acr_block_state[src_acr];
        linx_acr_restore_block_state(env, dst_acr);

        const uint64_t evbase = env->ssr_acr[dst_acr][LINX_SSR_EVBASE];

        /* Save interrupt source state into managing ACR bank. */
        env->ssr_acr[dst_acr][LINX_SSR_ECSTATE] = src_cstate;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG0] = (uint64_t)(src_state->blocktype & 0x1fu);
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_CUR] = resume_bpc;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_TGT] = resume_pc;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TPC] = resume_pc;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_LRA] = 0;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ0] = src_state->tq[0];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ1] = src_state->tq[1];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ2] = src_state->tq[2];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ3] = src_state->tq[3];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ0] = src_state->uq[0];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ1] = src_state->uq[1];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ2] = src_state->uq[2];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ3] = src_state->uq[3];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_LB] = linx_pack_u16x3(src_state->lb[0], src_state->lb[1], src_state->lb[2]);
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_LC] = linx_pack_u16x3(src_state->lc[0], src_state->lc[1], src_state->lc[2]);
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_PTR] = 0;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_META] = 0;

        /* Find an IRQ ID from IPENDING (simple bitmap model). */
        uint32_t irq_id = LINX_IRQ_TIMER0;
        {
            const uint64_t ip = env->ssr_acr[dst_acr][LINX_SSR_IPENDING];
            if (ip) {
                irq_id = (uint32_t)ctz64(ip);
            }
        }

        env->ssr_acr[dst_acr][LINX_SSR_TRAPNO] =
            linx_trapno_make(true, true, 0, LINX_TRAPNUM_INTERRUPT);
        env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = (uint64_t)irq_id;

        /* Switch to managing ring and vector. */
        env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
        env->acr = dst_acr;
        env->ssr[LINX_SSR_CSTATE] = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], dst_acr);
        env->pc = evbase ? evbase : last_pc;
        cs->exception_index = -1;
        return;
    }

    default:
        /* Check if it's a generic QEMU exception that we should handle */
        if (exception >= 0 && exception < 0x100) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: unhandled exception %d at PC=0x%" PRIx64 "\n",
                          exception, last_pc);
            cs->exception_index = -1;
            cpu_abort(cs, "Linx: Unhandled exception");
            return;
        } else if (exception < 0) {
            /* Negative exception_index means no exception */
            cs->exception_index = -1;
            return;
        } else {
            /* Unrecognized exception >= 0x100 */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: unrecognized exception %d at PC=0x%" PRIx64 "\n",
                          exception, last_pc);
            cs->exception_index = -1;
            cpu_set_interrupt(cs, CPU_INTERRUPT_EXITTB);
            return;
        }
    }
}

#if TARGET_LONG_BITS == 64
static vaddr linx_pointer_wrap(CPUState *cs, int mmu_idx, vaddr result, vaddr base)
{
    /* 64-bit addresses don't wrap */
    return result;
}
#endif

static int linx_cpu_mmu_index(CPUState *cs, bool ifunc)
{
    CPULinxState *env = cpu_env(cs);
    return ((env->acr & 0xFu) == 2) ? 1 : 0;
}

static inline bool linx_va_is_canonical(vaddr va)
{
    const uint64_t top = ((uint64_t)va >> 48) & 0xffffu;
    const uint64_t sign = ((uint64_t)va >> 47) & 1u;
    return top == (sign ? 0xffffu : 0x0000u);
}

static inline uint8_t linx_fault_acc(MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_INST_FETCH:
        return LINX_TRAPCAUSE_ACC_INST;
    case MMU_DATA_STORE:
        return LINX_TRAPCAUSE_ACC_STORE;
    case MMU_DATA_LOAD:
    default:
        return LINX_TRAPCAUSE_ACC_LOAD;
    }
}

static bool linx_mmu_translate(CPUState *cs, CPULinxState *env, vaddr va,
                               MMUAccessType access_type, int mmu_idx,
                               hwaddr *pa_out, int *prot_out,
                               hwaddr *tlb_size_out, uint8_t *cause_out)
{
    (void)cs;
    const uint64_t tcr = env->ssr_acr[1][LINX_SSR_TCR];
    const bool mme = (tcr & 1u) != 0;
    const uint8_t acc = linx_fault_acc(access_type);

    if (!mme) {
        /* Identity mapping for NOMMU / MME=0. */
        *pa_out = linx_nommu_phys_addr(va);
        *prot_out = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        *tlb_size_out = TARGET_PAGE_SIZE;
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE, acc);
        return true;
    }

    if (!linx_va_is_canonical(va)) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    /* v0.2 bring-up profile: only 48-bit VA supported (T0SZ/T1SZ must be 16). */
    const uint32_t t0sz = (uint32_t)((tcr >> 1) & 0x3fu);
    const uint32_t t1sz = (uint32_t)((tcr >> 7) & 0x3fu);
    if (t0sz != 16 || t1sz != 16) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    const uint32_t epd0 = (uint32_t)((tcr >> 13) & 1u);
    const uint32_t epd1 = (uint32_t)((tcr >> 14) & 1u);
    const bool use_ttbr1 = (((uint64_t)va >> 47) & 1u) != 0;

    if (!use_ttbr1 && epd0) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }
    if (use_ttbr1 && epd1) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    const uint64_t ttbr = use_ttbr1 ? env->ssr_acr[1][LINX_SSR_TTBR1]
                                    : env->ssr_acr[1][LINX_SSR_TTBR0];
    if ((ttbr & 0xfffu) != 0) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    hwaddr table = (hwaddr)(ttbr & 0x0000fffffffff000ULL);

    /* Walk L0..L3. */
    for (int level = 0; level < 4; level++) {
        const uint32_t shift = 39u - (uint32_t)level * 9u;
        const uint64_t idx = (((uint64_t)va) >> shift) & 0x1ffu;
        const hwaddr desc_addr = table + (hwaddr)(idx * 8u);
        MemTxResult result = MEMTX_OK;
        const uint64_t desc = address_space_ldq_le(&address_space_memory, desc_addr,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const uint32_t type = (uint32_t)(desc & 0x3u);
        if (type == 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        if (type == 3) {
            /* Table descriptor: Desc[1:0]=11. */
            if ((desc & 0xffcULL) != 0) {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
            if ((desc >> 48) != 0) {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
            table = (hwaddr)(desc & 0x0000fffffffff000ULL);
            continue;
        }

        /* Leaf descriptor: Page at L3, Block at L1/L2 (optional). */
        if (level == 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        hwaddr block_size = TARGET_PAGE_SIZE;
        if (type == 2) {
            if (level == 1) {
                block_size = (hwaddr)1ull << 30; /* 1 GiB */
            } else if (level == 2) {
                block_size = (hwaddr)1ull << 21; /* 2 MiB */
            } else {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
        } else if (type == 1) {
            if (level != 3) {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
        } else {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const hwaddr out_base = (hwaddr)(desc & 0x0000fffffffff000ULL);
        if ((desc >> 48) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }
        if ((out_base & (block_size - 1u)) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        /* Reserved bits for leaf descriptors. */
        if ((desc & (3ull << 10)) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const uint32_t attridx = (uint32_t)((desc >> 7) & 0x7u);
        if (attridx > 2u) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const bool af = ((desc >> 6) & 1u) != 0;
        if (!af) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const bool u = ((desc >> 5) & 1u) != 0;
        const bool x = ((desc >> 4) & 1u) != 0;
        const bool w = ((desc >> 3) & 1u) != 0;
        const bool r = ((desc >> 2) & 1u) != 0;

        if (mmu_idx == 1 && !u) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }
        if (access_type == MMU_INST_FETCH && !x) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }
        if (access_type == MMU_DATA_LOAD && !r) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }
        if (access_type == MMU_DATA_STORE && !w) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }

        const hwaddr pa = out_base | (hwaddr)((uint64_t)va & (uint64_t)(block_size - 1u));
        if (((uint64_t)pa >> 48) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        int prot = 0;
        if (r) {
            prot |= PAGE_READ;
        }
        if (w) {
            prot |= PAGE_WRITE;
        }
        if (x) {
            prot |= PAGE_EXEC;
        }

        *pa_out = pa;
        *prot_out = prot;
        *tlb_size_out = block_size;
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE, acc);
        return true;
    }

    *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
    return false;
}

static bool linx_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    /*
     * NOMMU uses identity translation, with a compatibility fold for
     * sign-extended legacy 29-bit physical addresses.
     */
    CPULinxState *env = cpu_env(cs);
    hwaddr pa = 0;
    int prot = 0;
    hwaddr tlb_size = TARGET_PAGE_SIZE;
    uint8_t cause = 0;

    if (linx_trace_mmu()) {
        static int count;
        if (count++ < 128) {
            const uint64_t tcr = env->ssr_acr[1][LINX_SSR_TCR];
            fprintf(stderr,
                    "linx: tlb_fill addr=0x%016" PRIx64 " access=%d mmu_idx=%d probe=%d tcr=0x%016" PRIx64 " acr=%u\n",
                    (uint64_t)addr, access_type, mmu_idx, probe ? 1 : 0,
                    tcr, env->acr & 0xFu);
            fflush(stderr);
        }
    }

    if (linx_mmu_translate(cs, env, addr, access_type, mmu_idx,
                           &pa, &prot, &tlb_size, &cause)) {
        /*
         * Bring-up: map only TARGET_PAGE_SIZE granularity in the softmmu TLB,
         * even when the page table descriptor is a larger block mapping.
         *
         * This avoids relying on large-page TLB support while the Linx MMU
         * model is still stabilizing.
         */
        hwaddr map_size = tlb_size;
        if (map_size > TARGET_PAGE_SIZE) {
            map_size = TARGET_PAGE_SIZE;
        }
        vaddr vbase = addr & ~(vaddr)(map_size - 1u);
        hwaddr pbase = pa & ~(hwaddr)(map_size - 1u);
        if (linx_trace_mmu()) {
            static int count_ok;
            if (count_ok++ < 128) {
                fprintf(stderr,
                        "linx: tlb_ok  va=0x%016" PRIx64 " -> pa=0x%016" HWADDR_PRIx
                        " size=0x%016" HWADDR_PRIx " prot=0x%x\n",
                        (uint64_t)addr, pa, map_size, prot);
                fflush(stderr);
            }
        }
        tlb_set_page(cs, vbase, pbase, prot, mmu_idx, map_size);
        if (linx_trace_mmu()) {
            static int count_set;
            if (count_set++ < 128) {
                fprintf(stderr,
                        "linx: tlb_set va_base=0x%016" PRIx64 " pa_base=0x%016" HWADDR_PRIx
                        " size=0x%016" HWADDR_PRIx "\n",
                        (uint64_t)vbase, pbase, map_size);
                fflush(stderr);
            }
        }
        return true;
    }

    if (probe) {
        return false;
    }

    env->pending_trap_arg0 = (uint64_t)addr;
    env->pending_trap_cause = (uint32_t)cause;

    switch (access_type) {
    case MMU_INST_FETCH:
        cs->exception_index = LINX_EXCP_INST_ACCESS_FAULT;
        break;
    case MMU_DATA_STORE:
        cs->exception_index = LINX_EXCP_STORE_ACCESS_FAULT;
        break;
    case MMU_DATA_LOAD:
    default:
        cs->exception_index = LINX_EXCP_LOAD_ACCESS_FAULT;
        break;
    }

    cpu_loop_exit_restore(cs, retaddr);
}

static void linx_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPULinxState *env = cpu_env(cs);
    int i;

    qemu_fprintf(f,
                 "pc=0x%016" PRIx64 " brtype=%u carg=0x%08x cond=%u tgt=0x%016" PRIx64
                 " fcsr=0x%08x\n",
                 env->pc, env->brtype, env->carg, env->cond, env->tgt, env->fcsr);
    for (i = 0; i < LINX_GPR_COUNT; i += 4) {
        qemu_fprintf(f,
                     "r%-2d=0x%016" PRIx64 " r%-2d=0x%016" PRIx64
                     " r%-2d=0x%016" PRIx64 " r%-2d=0x%016" PRIx64 "\n",
                     i, env->gpr[i], i + 1, env->gpr[i + 1],
                     i + 2, env->gpr[i + 2], i + 3, env->gpr[i + 3]);
    }
}

static void linx_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    CPULinxState *env = cpu_env(cs);

    memset(env, 0, offsetof(CPULinxState, end_reset_fields));

    env->gpr[LINX_REG_ZERO] = 0;
    env->pc = 0;
    env->fcsr = 0;
    env->acr = 0;
    set_float_exception_flags(0, &env->fp_status);
    set_float_rounding_mode(float_round_nearest_even, &env->fp_status);
    set_default_nan_mode(true, &env->fp_status);
    set_float_default_nan_pattern(0b01000000, &env->fp_status);
    cs->exception_index = -1;
    cs->halted = 0;

    /* Cancel any pending timer interrupt. */
    if (env->timer) {
        timer_del(env->timer);
    }
}

static void linx_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    LinxCPUClass *lcc = LINX_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    lcc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    cpu_exec_realizefn(cs, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    /* Create the per-CPU virtual timer after reset initialization. */
    {
        CPULinxState *env = cpu_env(cs);
        if (!env->timer) {
            env->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, linx_timer_cb, cs);
        }
    }
}

static ObjectClass *linx_cpu_class_by_name(const char *cpu_model)
{
    if (!cpu_model || !strcmp(cpu_model, "linx")) {
        return object_class_by_name(TYPE_LINX_CPU_LINX);
    }
    return NULL;
}

static void linx_cpu_init(Object *obj)
{
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps linx_sysemu_ops = {
    .has_work = linx_cpu_has_work,
    .get_phys_page_debug = linx_cpu_get_phys_page_debug,
};

static const TCGCPUOps linx_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = false,

    .initialize = linx_translate_init,
    .translate_code = linx_translate_code,
    .get_tb_cpu_state = linx_get_tb_cpu_state,
    .synchronize_from_tb = linx_cpu_synchronize_from_tb,
    .restore_state_to_opc = linx_restore_state_to_opc,
    .mmu_index = linx_cpu_mmu_index,
    .tlb_fill = linx_cpu_tlb_fill,
#if TARGET_LONG_BITS == 32
    .pointer_wrap = cpu_pointer_wrap_uint32,
#else
    .pointer_wrap = linx_pointer_wrap,
#endif
    .cpu_exec_interrupt = linx_cpu_exec_interrupt,
    .cpu_exec_halt = linx_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = linx_cpu_do_interrupt,
};

static const VMStateDescription vmstate_linx_cpu = {
    .name = "linx_cpu",
    .version_id = 10,
    .minimum_version_id = 10,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(env.pc, LinxCPU),
        VMSTATE_UINT32(env.cond, LinxCPU),
        VMSTATE_UINT64(env.tgt, LinxCPU),
        VMSTATE_UINT32(env.carg, LinxCPU),
        VMSTATE_UINT32(env.brtype, LinxCPU),
        VMSTATE_UINT32(env.blocktype, LinxCPU),
        VMSTATE_UINT64(env.body_tpc, LinxCPU),
        VMSTATE_UINT64(env.return_pc, LinxCPU),
        VMSTATE_UINT32(env.in_body, LinxCPU),
        VMSTATE_UINT64(env.tmpl_pc, LinxCPU),
        VMSTATE_UINT32(env.tmpl_kind, LinxCPU),
        VMSTATE_UINT32(env.tmpl_step, LinxCPU),
        VMSTATE_UINT32(env.tmpl_reg_cur, LinxCPU),
        VMSTATE_UINT32(env.tmpl_reg_begin, LinxCPU),
        VMSTATE_UINT32(env.tmpl_reg_end, LinxCPU),
        VMSTATE_UINT64(env.tmpl_stacksize, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_dst, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_src, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_remaining, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_value, LinxCPU),
        VMSTATE_UINT32(env.fcsr, LinxCPU),
        VMSTATE_UINT32(env.acr, LinxCPU),
        VMSTATE_UINT64_ARRAY(env.gpr, LinxCPU, LINX_GPR_COUNT),
        VMSTATE_UINT64_ARRAY(env.tq, LinxCPU, 4),
        VMSTATE_UINT64_ARRAY(env.uq, LinxCPU, 4),
        VMSTATE_UINT64_ARRAY(env.vtq, LinxCPU, LINX_VEC_QUEUE_DEPTH),
        VMSTATE_UINT64_ARRAY(env.lb, LinxCPU, 3),
        VMSTATE_UINT64_ARRAY(env.lc, LinxCPU, 3),
        VMSTATE_UINT64(env.insn_pc_next, LinxCPU),
        VMSTATE_UINT64_ARRAY(env.ssr, LinxCPU, LINX_SSR_COUNT),
        VMSTATE_UINT64_2DARRAY(env.ssr_acr, LinxCPU, LINX_ACR_COUNT, LINX_SSR_COUNT),
        VMSTATE_UINT64_ARRAY(env.irq_level_acr, LinxCPU, LINX_ACR_COUNT),
        VMSTATE_UINT64(env.lr_addr, LinxCPU),
        VMSTATE_UINT32(env.lr_size, LinxCPU),
        VMSTATE_UINT32(env.lr_valid, LinxCPU),
        VMSTATE_END_OF_LIST(),
    },
};


static void linx_cpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CPUClass *cc = CPU_CLASS(klass);
    LinxCPUClass *lcc = LINX_CPU_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_parent_realize(dc, linx_cpu_realize,
                                    &lcc->parent_realize);
    dc->vmsd = &vmstate_linx_cpu;

    resettable_class_set_parent_phases(rc, NULL, linx_cpu_reset_hold, NULL,
                                       &lcc->parent_phases);

    cc->class_by_name = linx_cpu_class_by_name;
    cc->dump_state = linx_cpu_dump_state;
    cc->set_pc = linx_cpu_set_pc;
    cc->get_pc = linx_cpu_get_pc;
    cc->sysemu_ops = &linx_sysemu_ops;
    cc->tcg_ops = &linx_tcg_ops;
}

static const TypeInfo linx_cpu_base_type_info = {
    .name = TYPE_LINX_CPU,
    .parent = TYPE_CPU,
    .instance_size = sizeof(LinxCPU),
    .instance_align = __alignof__(LinxCPU),
    .instance_init = linx_cpu_init,
    .abstract = true,
    .class_size = sizeof(LinxCPUClass),
    .class_init = linx_cpu_class_init,
};

static const TypeInfo linx_cpu_type_info = {
    .name = TYPE_LINX_CPU_LINX,
    .parent = TYPE_LINX_CPU,
};

static void linx_cpu_register_types(void)
{
    type_register_static(&linx_cpu_base_type_info);
    type_register_static(&linx_cpu_type_info);
}

type_init(linx_cpu_register_types)
