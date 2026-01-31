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

#ifdef CONFIG_USER_ONLY
#error "LinxISA does not support user mode emulation"
#endif

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

    uint64_t pc;

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
};

void linx_translate_init(void);
void linx_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);

#endif

