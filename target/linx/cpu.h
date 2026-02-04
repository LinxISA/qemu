/*
 * LinxISA CPU definition
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_CPU_H
#define LINX_CPU_H

#include "cpu-qom.h"
#include "exec/cpu-common.h"
#include "exec/cpu-defs.h"
#include "exec/cpu-interrupt.h"
#include "exec/mmu-access-type.h"
#include "hw/core/resettable.h"

#ifdef CONFIG_USER_ONLY
#error "LinxISA does not support user mode emulation"
#endif

/* Exception types
 * Note: We start from 1, not 0, because exception_index = 0 would
 * trigger do_interrupt via replay_exception() even when there's no exception.
 */
enum {
    LINX_EXCP_BREAKPOINT = 1,  /* EBREAK instruction */
    LINX_EXCP_ILLEGAL_INST = 2, /* Illegal instruction */
    LINX_EXCP_INST_ACCESS_FAULT = 3, /* Instruction access fault */
    LINX_EXCP_LOAD_ACCESS_FAULT = 4, /* Load access fault */
    LINX_EXCP_STORE_ACCESS_FAULT = 5, /* Store access fault */
    LINX_EXCP_BAD_BRANCH_TARGET = 6, /* Branch target not at block start marker */
};
enum {
    LINX_REG_ZERO = 0,
    LINX_REG_SP   = 1,
    LINX_REG_A0   = 2,
    LINX_REG_A1   = 3,
    LINX_REG_A2   = 4,
    LINX_REG_A3   = 5,
    LINX_REG_A4   = 6,
    LINX_REG_A5   = 7,
    LINX_REG_A6   = 8,
    LINX_REG_A7   = 9,
    LINX_REG_RA   = 10,

    LINX_GPR_COUNT = 24,
};

typedef struct CPUArchState {
    uint64_t gpr[LINX_GPR_COUNT];
    uint64_t tq[4];
    uint64_t uq[4];

    uint64_t tgt;
    uint32_t cond;
    uint32_t carg;  /* Commit argument flag (set by SETC.COND) */
    uint32_t brtype;

    uint64_t pc;

    /* Small hot-path cache for bstart target validation. */
    uint64_t bstart_cache[4];
    uint32_t bstart_cache_next;

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;
} CPULinxState;

/*
 * LinxCPU:
 * @env: #CPULinxState
 */
struct ArchCPU {
    CPUState parent_obj;

    CPULinxState env;
};

struct LinxCPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

void linx_translate_init(void);
void linx_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);

#endif
