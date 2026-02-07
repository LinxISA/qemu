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
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "exec/log.h"
#include "fpu/softfloat-helpers.h"
#include "tcg/debug-assert.h"
#include "accel/tcg/cpu-ops.h"
#include "system/runstate.h"
#include "qemu/timer.h"

/* Managing-ACR SSR indices (low 12 bits). */
enum {
    LINX_SSR_ECSTATE  = 0xF00,
    LINX_SSR_EVBASE   = 0xF01,
    LINX_SSR_TRAPNO   = 0xF02,
    LINX_SSR_TRAPARG0 = 0xF03,
    LINX_SSR_IPENDING = 0xF08,
    LINX_SSR_EOIEI    = 0xF0A,
    LINX_SSR_EBPC     = 0xF0B,
    LINX_SSR_ETPC     = 0xF0D,
    LINX_SSR_EBPCN    = 0xF0E,
    LINX_SSR_TIMECMP  = 0xF21,
};

/* Common (non-banked) SSR indices. */
enum {
    LINX_SSR_CSTATE = 0x0020,
};

/* CSTATE bits (keep in sync with target/linx/helper.c). */
#define LINX_CSTATE_ACR_MASK 0xFULL
#define LINX_CSTATE_I_BIT    (1ULL << 4)

/* Simple timer interrupt ID (bring-up). */
enum {
    LINX_IRQ_TIMER0 = 0,
};

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
    if (!linx_irq_allowed(env, dst_acr)) {
        return;
    }
    if (env->ssr_acr[dst_acr][LINX_SSR_IPENDING] == 0) {
        return;
    }
    cpu_interrupt(cs, CPU_INTERRUPT_HARD);
}

static void linx_timer_cb(void *opaque)
{
    CPUState *cs = opaque;
    LinxCPU *cpu = LINX_CPU(cs);
    CPULinxState *env = &cpu->env;

    /* Set pending bit and raise a hard interrupt. */
    env->ssr_acr[0][LINX_SSR_IPENDING] |= (1ull << LINX_IRQ_TIMER0);
    linx_irq_kick_if_allowed(cs, env, 0);
}

static hwaddr linx_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    /* Linx currently uses simple identity mapping. */
    return (hwaddr)addr;
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

static TCGTBCPUState linx_get_tb_cpu_state(CPUState *cs)
{
    CPULinxState *env = cpu_env(cs);
    return (TCGTBCPUState){ .pc = env->pc, .flags = 0 };
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
        /* Route all external interrupts to EXCP_INTERRUPT for now. */
        cs->exception_index = EXCP_INTERRUPT;
        if (!linx_irq_allowed(cpu_env(cs), 0)) {
            /* Leave the interrupt request pending until it becomes allowed. */
            cs->exception_index = -1;
            return false;
        }
        linx_cpu_do_interrupt(cs);
        return true;
    }
    return false;
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
        /* EBREAK - used for program exit in virt machine */
        qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK - program exit at PC=0x%" PRIx64 "\n",
                      last_pc);
        cs->exception_index = -1;
        /* Request graceful shutdown of the VM */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        cpu_loop_exit(cs);
        return;

    case LINX_EXCP_BAD_BRANCH_TARGET:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: branch target violation at PC=0x%" PRIx64 "\n",
                      last_pc);
        cs->exception_index = -1;
        cpu_abort(cs, "Linx: Bad branch target");
        return;

    case LINX_EXCP_ILLEGAL_INST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: illegal instruction at PC=0x%" PRIx64 "\n",
                      last_pc);
        cs->exception_index = -1;
        cpu_abort(cs, "Linx: Illegal instruction");
        return;

    case LINX_EXCP_INST_ACCESS_FAULT:
    case LINX_EXCP_LOAD_ACCESS_FAULT:
    case LINX_EXCP_STORE_ACCESS_FAULT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: memory access fault at PC=0x%" PRIx64 "\n",
                      last_pc);
        cs->exception_index = -1;
        cpu_abort(cs, "Linx: Memory access fault");
        return;

    case EXCP_INTERRUPT:
        /*
         * Hardware interrupt (bring-up).
         *
         * Model this as an asynchronous SERVICE_REQUEST routed to ACR0:
         * save minimal trap state into ACR0's managing SSR bank and vector
         * to EVBASE_ACR0.
         */
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);

        const uint64_t evbase = env->ssr_acr[0][LINX_SSR_EVBASE];

        /* Save trap source state into ACR0 bank. */
        env->ssr_acr[0][LINX_SSR_ECSTATE] = env->ssr[LINX_SSR_CSTATE];
        env->ssr_acr[0][LINX_SSR_EBPC] = last_pc;
        env->ssr_acr[0][LINX_SSR_ETPC] = last_pc;
        env->ssr_acr[0][LINX_SSR_EBPCN] = last_pc;
        env->ssr_acr[0][LINX_SSR_TRAPNO] = 0; /* profile-defined */
        env->ssr_acr[0][LINX_SSR_TRAPARG0] = LINX_IRQ_TIMER0;

        /* Switch to ACR0 and vector. */
        env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
        env->acr = 0;
        env->ssr[LINX_SSR_CSTATE] = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], 0);
        env->pc = evbase ? evbase : last_pc;
        cs->exception_index = -1;
        return;

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
    return 0;
}

static bool linx_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    /* Simple identity mapping: virtual address = physical address */
    hwaddr phys_addr = addr;
    vaddr page = addr & TARGET_PAGE_MASK;
    hwaddr phys_page = phys_addr & TARGET_PAGE_MASK;
    int prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    tlb_set_page(cs, page, phys_page, prot, mmu_idx, TARGET_PAGE_SIZE);
    return true;
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
    .version_id = 5,
    .minimum_version_id = 5,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(env.pc, LinxCPU),
        VMSTATE_UINT32(env.cond, LinxCPU),
        VMSTATE_UINT64(env.tgt, LinxCPU),
        VMSTATE_UINT32(env.carg, LinxCPU),
        VMSTATE_UINT32(env.brtype, LinxCPU),
        VMSTATE_UINT32(env.blocktype, LinxCPU),
        VMSTATE_UINT32(env.fcsr, LinxCPU),
        VMSTATE_UINT32(env.acr, LinxCPU),
        VMSTATE_UINT64_ARRAY(env.gpr, LinxCPU, LINX_GPR_COUNT),
        VMSTATE_UINT64_ARRAY(env.tq, LinxCPU, 4),
        VMSTATE_UINT64_ARRAY(env.uq, LinxCPU, 4),
        VMSTATE_UINT64_ARRAY(env.lb, LinxCPU, 3),
        VMSTATE_UINT64_ARRAY(env.ssr, LinxCPU, LINX_SSR_COUNT),
        VMSTATE_UINT64_2DARRAY(env.ssr_acr, LinxCPU, LINX_ACR_COUNT, LINX_SSR_COUNT),
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
