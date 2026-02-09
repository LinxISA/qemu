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
#include "fpu/softfloat.h"
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
    LINX_EXCP_BLOCK_FAULT = 7, /* Block-format violation (header/body legality, missing B.TEXT, etc.) */
};

/*
 * Bring-up exception cause codes for E_BLOCK (encoded in TRAPNO.CAUSE).
 *
 * These are profile-defined and only used for debug/reporting today.
 */
enum {
    LINX_EBLOCK_CAUSE_BAD_BRANCH_TARGET = 1,
    LINX_EBLOCK_CAUSE_MISSING_BODY_TPC  = 2,
    LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY   = 3,
    LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER = 4,
    LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK = 5,
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

/*
 * Template block kinds (bring-up subset).
 *
 * These are standalone blocks (block start markers) that execute via the
 * restartable template generator model.
 */
typedef enum LinxTemplateKind {
    LINX_TEMPLATE_FENTRY   = 0,
    LINX_TEMPLATE_FEXIT    = 1,
    LINX_TEMPLATE_FRET_RA  = 2,
    LINX_TEMPLATE_FRET_STK = 3,
    LINX_TEMPLATE_MCOPY    = 4,
    LINX_TEMPLATE_MSET     = 5,
} LinxTemplateKind;

#define LINX_SSR_COUNT 0x1000u /* SSR_ID[11:0] */
#define LINX_ACR_COUNT 16u     /* ACR0..ACR15 */

/*
 * Block/queue state that must be preserved across ACR transitions.
 *
 * The Linx Block ISA defines architectural commit at block boundaries. Traps
 * (SERVICE_REQUEST/interrupts) can occur mid-block and return to the trapped
 * context at the next PC, so the block commit metadata and hand queues must be
 * restored when switching back to the target ACR.
 */
typedef struct LinxAcrBlockState {
    uint64_t tq[4];
    uint64_t uq[4];

    uint64_t bpc;

    uint64_t tgt;
    uint32_t cond;
    uint32_t carg;
    uint32_t brtype;
    uint32_t blocktype;

    /* Decoupled-block state (B.TEXT out-of-line bodies). */
    uint64_t body_tpc;
    uint64_t return_pc;
    uint32_t in_body;

    /* Restartable template state (bring-up subset). */
    uint64_t tmpl_pc;
    uint32_t tmpl_kind;
    uint32_t tmpl_step;
    uint32_t tmpl_reg_cur;
    uint32_t tmpl_reg_begin;
    uint32_t tmpl_reg_end;
    uint64_t tmpl_stacksize;
    uint64_t tmpl_mem_dst;
    uint64_t tmpl_mem_src;
    uint64_t tmpl_mem_remaining;
    uint64_t tmpl_mem_value;

    uint64_t lb[3]; /* LB0..LB2 */

    /* Tile block state (minimal bring-up subset). */
    uint32_t tile_func;
    uint32_t tile_dtype;
    uint32_t tile_iot_valid;
    uint32_t tile_iot_flags;
    uint32_t tile_iot_dst;
    uint32_t tile_iot_grp;
    uint32_t tile_iot_src0;
    uint32_t tile_iot_src1;
    uint32_t tile_iot_reg;
    uint32_t tile_iot_size;
} LinxAcrBlockState;

typedef struct CPUArchState {
    uint64_t gpr[LINX_GPR_COUNT];
    uint64_t tq[4];
    uint64_t uq[4];

    /*
     * System Status Registers (SSR).
     *
     * Base SSR access instructions encode only SSR_ID[11:0], so model a 4K SSR
     * file indexed by SSR_ID[11:0]. For privileged/ACR-scoped families (IDs in
     * the 0xF00..0xFFF range), the effective register is selected by the
     * managing ACR encoded in the full SSR ID (0xnfxx). The base 12-bit forms
     * can address only ACR0's 0x0fxx subset; HL.SSRGET/HL.SSRSET are required
     * for other managing ACR banks.
     */
    uint64_t ssr[LINX_SSR_COUNT];       /* non-ACR-scoped SSRs */
    uint64_t ssr_acr[LINX_ACR_COUNT][LINX_SSR_COUNT]; /* managing-ACR banks */

    /* Current Access Control Ring (ACR) level: 0..15. */
    uint32_t acr;

    /* Floating-point control/state (bring-up hard-float). */
    float_status fp_status;
    uint32_t fcsr;

    uint64_t tgt;
    uint32_t cond;
    uint32_t carg;  /* Commit argument flag (set by SETC.COND) */
    uint32_t brtype;
    uint32_t blocktype;

    /* Decoupled-block state (B.TEXT out-of-line bodies). */
    uint64_t body_tpc;
    uint64_t return_pc;
    uint32_t in_body;

    /* Restartable template state (bring-up subset). */
    uint64_t tmpl_pc;
    uint32_t tmpl_kind;
    uint32_t tmpl_step;
    uint32_t tmpl_reg_cur;
    uint32_t tmpl_reg_begin;
    uint32_t tmpl_reg_end;
    uint64_t tmpl_stacksize;
    uint64_t tmpl_mem_dst;
    uint64_t tmpl_mem_src;
    uint64_t tmpl_mem_remaining;
    uint64_t tmpl_mem_value;

    /* Block argument registers (set via B.DIM / C.B.DIM*). */
    uint64_t lb[3]; /* LB0..LB2 */

    /* Saved block/queue state per ACR for trap/return correctness. */
    LinxAcrBlockState acr_block_state[LINX_ACR_COUNT];

    /*
     * Tile block state (TAU bring-up).
     *
     * For now this models a minimal single-B.IOT descriptor per block. The
     * implementation is intentionally small and is primarily used for PTO ISA
     * bring-up (matmul demo).
     */
    uint32_t tile_func;
    uint32_t tile_dtype;
    uint32_t tile_iot_valid;
    uint32_t tile_iot_flags;
    uint32_t tile_iot_dst;
    uint32_t tile_iot_grp;
    uint32_t tile_iot_src0;
    uint32_t tile_iot_src1;
    uint32_t tile_iot_reg;
    uint32_t tile_iot_size;

    /* Emulated tile register file: 4 hands x 8 depth = 32 tiles. */
    uint32_t tile_reg[32][1024]; /* 4KB per tile (1024 x i32 words). */
    uint32_t tile_acc[1024];     /* 4KB accumulator (bring-up). */

    /* Current block start marker address (BPC) for trap reporting. */
    uint64_t bpc;

    uint64_t pc;

    /* Dynamic instruction counter (for benchmarking/bring-up). */
    uint64_t insn_count;

    /* Pending trap reporting for synchronous faults (MMU/IOMMU). */
    uint64_t pending_trap_arg0;
    uint32_t pending_trap_cause;

    /* LR/SC reservation state (bring-up model). */
    uint64_t lr_addr;
    uint32_t lr_size;
    uint32_t lr_valid;

    /* Small hot-path cache for bstart target validation. */
    uint64_t bstart_cache[4];
    uint32_t bstart_cache_next;

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;

    /* Per-CPU virtual timer for TIMER_TIMECMP (bring-up). */
    struct QEMUTimer *timer;
} CPULinxState;

static inline void linx_acr_save_block_state(CPULinxState *env, uint32_t acr)
{
    LinxAcrBlockState *s;
    int i;

    if (acr >= LINX_ACR_COUNT) {
        return;
    }
    s = &env->acr_block_state[acr];

    for (i = 0; i < 4; i++) {
        s->tq[i] = env->tq[i];
        s->uq[i] = env->uq[i];
    }

    s->bpc = env->bpc;

    s->tgt = env->tgt;
    s->cond = env->cond;
    s->carg = env->carg;
    s->brtype = env->brtype;
    s->blocktype = env->blocktype;

    s->body_tpc = env->body_tpc;
    s->return_pc = env->return_pc;
    s->in_body = env->in_body;

    s->tmpl_pc = env->tmpl_pc;
    s->tmpl_kind = env->tmpl_kind;
    s->tmpl_step = env->tmpl_step;
    s->tmpl_reg_cur = env->tmpl_reg_cur;
    s->tmpl_reg_begin = env->tmpl_reg_begin;
    s->tmpl_reg_end = env->tmpl_reg_end;
    s->tmpl_stacksize = env->tmpl_stacksize;
    s->tmpl_mem_dst = env->tmpl_mem_dst;
    s->tmpl_mem_src = env->tmpl_mem_src;
    s->tmpl_mem_remaining = env->tmpl_mem_remaining;
    s->tmpl_mem_value = env->tmpl_mem_value;

    for (i = 0; i < 3; i++) {
        s->lb[i] = env->lb[i];
    }

    s->tile_func = env->tile_func;
    s->tile_dtype = env->tile_dtype;
    s->tile_iot_valid = env->tile_iot_valid;
    s->tile_iot_flags = env->tile_iot_flags;
    s->tile_iot_dst = env->tile_iot_dst;
    s->tile_iot_grp = env->tile_iot_grp;
    s->tile_iot_src0 = env->tile_iot_src0;
    s->tile_iot_src1 = env->tile_iot_src1;
    s->tile_iot_reg = env->tile_iot_reg;
    s->tile_iot_size = env->tile_iot_size;
}

static inline void linx_acr_restore_block_state(CPULinxState *env, uint32_t acr)
{
    const LinxAcrBlockState *s;
    int i;

    if (acr >= LINX_ACR_COUNT) {
        return;
    }
    s = &env->acr_block_state[acr];

    for (i = 0; i < 4; i++) {
        env->tq[i] = s->tq[i];
        env->uq[i] = s->uq[i];
    }

    env->bpc = s->bpc;

    env->tgt = s->tgt;
    env->cond = s->cond;
    env->carg = s->carg;
    env->brtype = s->brtype;
    env->blocktype = s->blocktype;

    env->body_tpc = s->body_tpc;
    env->return_pc = s->return_pc;
    env->in_body = s->in_body;

    env->tmpl_pc = s->tmpl_pc;
    env->tmpl_kind = s->tmpl_kind;
    env->tmpl_step = s->tmpl_step;
    env->tmpl_reg_cur = s->tmpl_reg_cur;
    env->tmpl_reg_begin = s->tmpl_reg_begin;
    env->tmpl_reg_end = s->tmpl_reg_end;
    env->tmpl_stacksize = s->tmpl_stacksize;
    env->tmpl_mem_dst = s->tmpl_mem_dst;
    env->tmpl_mem_src = s->tmpl_mem_src;
    env->tmpl_mem_remaining = s->tmpl_mem_remaining;
    env->tmpl_mem_value = s->tmpl_mem_value;

    for (i = 0; i < 3; i++) {
        env->lb[i] = s->lb[i];
    }

    env->tile_func = s->tile_func;
    env->tile_dtype = s->tile_dtype;
    env->tile_iot_valid = s->tile_iot_valid;
    env->tile_iot_flags = s->tile_iot_flags;
    env->tile_iot_dst = s->tile_iot_dst;
    env->tile_iot_grp = s->tile_iot_grp;
    env->tile_iot_src0 = s->tile_iot_src0;
    env->tile_iot_src1 = s->tile_iot_src1;
    env->tile_iot_reg = s->tile_iot_reg;
    env->tile_iot_size = s->tile_iot_size;
}

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
