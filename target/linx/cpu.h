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

#define LINX_SSR_COUNT 0x1000u /* SSR_ID[11:0] */
#define LINX_ACR_COUNT 16u     /* ACR0..ACR15 */

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

    /* Block argument registers (set via B.DIM / C.B.DIM*). */
    uint64_t lb[3]; /* LB0..LB2 */

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

    uint64_t pc;

    /* Dynamic instruction counter (for benchmarking/bring-up). */
    uint64_t insn_count;

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
