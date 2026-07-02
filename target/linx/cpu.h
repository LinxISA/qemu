/*
 * LinxISA CPU definition
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_CPU_H
#define LINX_CPU_H

#include <stdio.h>

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
    LINX_EXCP_HW_BREAKPOINT = 8, /* Hardware breakpoint */
    LINX_EXCP_HW_WATCHPOINT = 9, /* Hardware watchpoint */
    LINX_EXCP_EXEC_STATE_CHECK = 10, /* ACR_ENTER target/state invalid */
};

/*
 * v0.3 bring-up: E_BLOCK cause encoding (encoded in TRAPNO.CAUSE).
 *
 * TRAPNO layout is unchanged from the v0.2 bring-up profile:
 * - TRAPNO.CAUSE is a 24-bit field stored at TRAPNO[47:24].
 *
 * v0.3 encoding within TRAPNO.CAUSE:
 * - CAUSE[15:8]  = EC (E_BLOCK cause)
 * - CAUSE[7:4]   = reserved (0)
 * - CAUSE[3:0]   = kind (used by EC_CFI)
 */
enum {
    LINX_EBLOCK_EC_CFI      = 0x01, /* E_BLOCK(EC_CFI) */
    LINX_EBLOCK_EC_BLOCKFMT = 0x02, /* E_BLOCK(EC_BLOCKFMT) */
    LINX_EBLOCK_EC_BFETCH   = 0x03, /* E_BLOCK(EC_BFETCH) */
};

enum {
    LINX_EBLOCK_CFI_BAD_TARGET        = 0x1,
    LINX_EBLOCK_CFI_MISSING_NEXT_MARKER = 0x3,
};

/*
 * v0.3 E_BLOCK(EC_BLOCKFMT) TRAPARG0[15:0] encoding:
 *   [7:0]   = descriptor family
 *   [15:8]  = detail code
 */
enum {
    LINX_BLOCKFMT_FAMILY_NONE = 0x00,
    LINX_BLOCKFMT_FAMILY_DIM  = 0x01,
    LINX_BLOCKFMT_FAMILY_TEXT = 0x02,
    LINX_BLOCKFMT_FAMILY_ARG  = 0x03,
    LINX_BLOCKFMT_FAMILY_IOR  = 0x04,
    LINX_BLOCKFMT_FAMILY_TILE = 0x05,
};

enum {
    LINX_BLOCKFMT_DETAIL_MISSING = 0x00,
    LINX_BLOCKFMT_DETAIL_INVALID = 0x01,
    LINX_BLOCKFMT_DETAIL_ILLEGAL_COMBO = 0x02,
};

static inline uint32_t linx_eblock_cause_make(uint8_t ec, uint8_t kind)
{
    return ((uint32_t)ec << 8) | (uint32_t)(kind & 0x0fu);
}

static inline uint8_t linx_eblock_cause_ec(uint32_t cause)
{
    return (uint8_t)((cause >> 8) & 0xffu);
}

static inline uint32_t linx_eblock_cfi_cause(uint8_t kind)
{
    return linx_eblock_cause_make(LINX_EBLOCK_EC_CFI, kind);
}

static inline uint32_t linx_eblock_blockfmt_cause(void)
{
    return linx_eblock_cause_make(LINX_EBLOCK_EC_BLOCKFMT, 0);
}

static inline uint32_t linx_eblock_bfetch_cause(void)
{
    return linx_eblock_cause_make(LINX_EBLOCK_EC_BFETCH, 0);
}

static inline uint64_t linx_blockfmt_traparg_make(uint8_t family, uint8_t detail)
{
    return ((uint64_t)(detail & 0xffu) << 8) | (uint64_t)(family & 0xffu);
}

/* Legacy v0.2 block fault codes (kept temporarily for internal translation). */
enum {
    LINX_EBLOCK_LEGACY_BAD_BRANCH_TARGET = 1,
    LINX_EBLOCK_LEGACY_MISSING_BODY_TPC  = 2,
    LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY   = 3,
    LINX_EBLOCK_LEGACY_ILLEGAL_IN_HEADER = 4,
    LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK = 5,
    LINX_EBLOCK_LEGACY_ACRC_MISSING_BSTOP = 6,
    LINX_EBLOCK_LEGACY_CALL_MISSING_SETRET = 7,
    LINX_EBLOCK_LEGACY_CALL_INVALID_SEQUENCE = 8,
    LINX_EBLOCK_LEGACY_RET_MISSING_SETCTGT = 9,
};
/* Compatibility aliases used by v0.3 helper/translate paths. */
enum {
    LINX_EBLOCK_CAUSE_BAD_BRANCH_TARGET = LINX_EBLOCK_LEGACY_BAD_BRANCH_TARGET,
    LINX_EBLOCK_CAUSE_MISSING_BODY_TPC  = LINX_EBLOCK_LEGACY_MISSING_BODY_TPC,
    LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY   = LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY,
    LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER = LINX_EBLOCK_LEGACY_ILLEGAL_IN_HEADER,
    LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK = LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
    LINX_EBLOCK_CAUSE_ACRC_MISSING_BSTOP = LINX_EBLOCK_LEGACY_ACRC_MISSING_BSTOP,
    LINX_EBLOCK_CAUSE_CALL_MISSING_SETRET = LINX_EBLOCK_LEGACY_CALL_MISSING_SETRET,
    LINX_EBLOCK_CAUSE_CALL_INVALID_SEQUENCE = LINX_EBLOCK_LEGACY_CALL_INVALID_SEQUENCE,
    LINX_EBLOCK_CAUSE_RET_MISSING_SETCTGT = LINX_EBLOCK_LEGACY_RET_MISSING_SETCTGT,
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
    LINX_REG_S0   = 11,
    LINX_REG_S1   = 12,
    LINX_REG_S2   = 13,
    LINX_REG_S3   = 14,
    LINX_REG_S4   = 15,
    LINX_REG_S5   = 16,
    LINX_REG_S6   = 17,
    LINX_REG_S7   = 18,
    LINX_REG_S8   = 19,
    LINX_REG_X0   = 20,
    LINX_REG_X1   = 21,
    LINX_REG_X2   = 22,
    LINX_REG_X3   = 23,

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
    LINX_TEMPLATE_ESAVE    = 6,
    LINX_TEMPLATE_ERCOV    = 7,
} LinxTemplateKind;

#define LINX_SSR_COUNT 0x1000u /* SSR_ID[11:0] */
#define LINX_ACR_COUNT 16u     /* ACR0..ACR15 */
#define LINX_TILE_MAX_IOR 16u
#define LINX_TILE_MAX_ITP 32u
#define LINX_TILE_MAX_OTA 32u
#define LINX_VEC_RI_MAX (LINX_TILE_MAX_IOR * 3u)
/*
 * v0.3 SIMT bring-up: LLVM autovec may reference VT indices up to VT#31 in
 * workloads (TSVC), so the emulator must model at least that depth.
 */
#define LINX_VEC_QUEUE_DEPTH 32u
#define LINX_COSIM_MAX_RANGES 256u
#define LINX_COSIM_PATH_MAX 512u
#define LINX_BSTART_CACHE_SIZE 1024u

#define LINX_TB_FLAG_IN_BODY (1u << 0)
#define LINX_TB_FLAG_USER_MMU (1u << 1)
#define LINX_TB_FLAG_DBG_ACTIVE (1u << 2)
#define LINX_TB_FLAG_COSIM_PRECHECK (1u << 3)

/*
 * EBARG preservation stack (bring-up).
 *
 * Linux relies on the manager-bank EBARG group being restored after nested
 * async interrupts so kernel code does not observe EBARG "state pollution"
 * when timer IRQs are enabled.
 */
#define LINX_SSR_EBARG_BASE 0xF40u
#define LINX_SSR_EBARG_COUNT 0x11u /* 0xF40..0xF50 inclusive */
#define LINX_EBARG_STACK_DEPTH 8u

/* Bring-up tile backing store limits (TAU). */
#define LINX_TILE_MAX_BYTES (64u * 1024u) /* must cover >=16KB bring-up */
#define LINX_TILE_MAX_WORDS (LINX_TILE_MAX_BYTES / 4u)

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
    uint64_t vtq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vuq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vmq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vnq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vec_p;

    uint64_t bpc;

    uint64_t tgt;
    uint32_t cond;
    uint32_t carg;
    uint32_t brtype;
    uint32_t blocktype;
    uint32_t call_ra_set;
    uint32_t call_setret_pending;

    /* Decoupled-block state (B.TEXT out-of-line bodies). */
    uint64_t body_tpc;
    uint64_t body_end;
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
    uint64_t lc[3]; /* LC0..LC2 */

    /* Tile block state (minimal bring-up subset). */
    uint32_t tile_func;
    uint32_t tile_dtype;
    uint32_t tile_desc_valid;

    uint32_t tile_arg_format;
    uint32_t tile_attr_raw;
    uint32_t tile_attr_pad;
    uint32_t tile_attr_dtype;
    uint32_t tile_ior_count;
    uint64_t tile_ior_desc[LINX_TILE_MAX_IOR];
    uint32_t vec_ri_count;
    uint64_t vec_ri_value[LINX_VEC_RI_MAX];
    uint32_t tile_itp_count;
    uint64_t tile_itp_desc[LINX_TILE_MAX_ITP];
    uint32_t tile_ota_count;
    uint64_t tile_ota_desc[LINX_TILE_MAX_OTA];
} LinxAcrBlockState;

typedef struct LinxCosimRange {
    uint64_t base;
    uint64_t size;
} LinxCosimRange;

typedef struct LinxBodyRange {
    uint64_t start;
    uint64_t end;
} LinxBodyRange;

typedef struct CPUArchState {
    uint64_t gpr[LINX_GPR_COUNT];
    uint64_t tq[4];
    uint64_t uq[4];
    uint64_t vtq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vuq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vmq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vnq[LINX_VEC_QUEUE_DEPTH];
    uint64_t vec_p;

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
    uint32_t call_ra_set;
    uint32_t call_setret_pending;

    /* Decoupled-block state (B.TEXT out-of-line bodies). */
    uint64_t body_tpc;
    uint64_t body_end;
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
    uint64_t lc[3]; /* LC0..LC2 */

    /* Saved block/queue state per ACR for trap/return correctness. */
    LinxAcrBlockState acr_block_state[LINX_ACR_COUNT];

    /*
     * Nested same-ACR traps (timer IRQ inside a syscall, etc.) overwrite the
     * managing ring's EBARG snapshot. Preserve and restore the full EBARG
     * group so software can treat it as stable across async interrupt returns.
     */
    uint32_t ebarg_stack_depth;
    uint64_t ebarg_stack[LINX_EBARG_STACK_DEPTH][LINX_SSR_EBARG_COUNT];

    /*
     * Tile block state (TAU bring-up).
     *
     * v0.57 block operand descriptors are split into B.ITP source-pair
     * descriptors and B.OTA destination-allocation descriptors.
     */
    uint32_t tile_func;
    uint32_t tile_dtype;
    uint32_t tile_desc_valid;

    uint32_t tile_arg_format;
    uint32_t tile_attr_raw;
    uint32_t tile_attr_pad;
    uint32_t tile_attr_dtype;
    uint32_t tile_ior_count;
    uint64_t tile_ior_desc[LINX_TILE_MAX_IOR];
    uint32_t vec_ri_count;
    uint64_t vec_ri_value[LINX_VEC_RI_MAX];
    uint32_t tile_itp_count;
    uint64_t tile_itp_desc[LINX_TILE_MAX_ITP];
    uint32_t tile_ota_count;
    uint64_t tile_ota_desc[LINX_TILE_MAX_OTA];
    uint32_t tile_meta_valid;
    uint32_t tile_meta_mode;
    uint64_t tile_meta_value;

    /* Emulated tile backing store: 4 hands x 8 depth = 32 tiles. */
    uint32_t tile_reg[32][LINX_TILE_MAX_WORDS];
    uint32_t tile_reg_bytes[32]; /* per-tile footprint in bytes */
    uint32_t tile_reg_rows[32];  /* logical FIXP rows, when known */
    uint32_t tile_reg_cols[32];  /* logical FIXP columns, when known */

    /* Accumulator backing store (separate scratch). */
    uint32_t tile_acc[LINX_TILE_MAX_WORDS];
    uint32_t tile_acc_bytes;

    /* Current block start marker address (BPC) for trap reporting. */
    uint64_t bpc;

    /* Next instruction PC (set at instruction start for precise trap reporting). */
    uint64_t insn_pc_next;

    uint64_t pc;

    /* Dynamic instruction counter (for benchmarking/bring-up). */
    uint64_t insn_count;
    uint64_t heartbeat_next_count;

    /* Pending trap reporting for synchronous faults (MMU/IOMMU). */
    uint64_t pending_trap_arg0;
    uint32_t pending_trap_cause;

    /* Debug-only syscall trace pairing state. */
    uint32_t syscall_trace_pending;
    uint32_t syscall_trace_entry_emitted;
    uint64_t syscall_trace_nr;
    uint64_t syscall_trace_bpc;
    uint64_t syscall_trace_tpc;
    uint64_t syscall_trace_pc_next;
    uint64_t syscall_trace_args[6];
    uint64_t syscall_trace_sp;
    uint64_t syscall_trace_ra;
    uint64_t syscall_trace_cstate;

    /*
     * External interrupt line levels per managing ACR (bit-per-IRQ).
     *
     * IPENDING is an interrupt-pending latch that software clears via EOIEI.
     * The level bitmap lets EOIEI re-pend level-high sources immediately.
     */
    uint64_t irq_level_acr[LINX_ACR_COUNT];

    /* Commit trace scratch (written by TCG, consumed by helper). */
    uint64_t trace_pc;
    uint64_t trace_insn;
    uint32_t trace_len;
    uint32_t trace_wb_valid;
    uint32_t trace_wb_rd;
    uint64_t trace_wb_data;
    uint32_t trace_src0_valid;
    uint32_t trace_src0_reg;
    uint64_t trace_src0_data;
    uint32_t trace_src1_valid;
    uint32_t trace_src1_reg;
    uint64_t trace_src1_data;
    uint32_t trace_dst_valid;
    uint32_t trace_dst_reg;
    uint64_t trace_dst_data;
    uint32_t trace_mem_valid;
    uint32_t trace_mem_is_store;
    uint64_t trace_mem_addr;
    uint64_t trace_mem_wdata;
    uint64_t trace_mem_rdata;
    uint32_t trace_mem_size;
    uint32_t trace_trap_valid;
    uint32_t trace_trap_cause; /* low16: (cause<<8)|trapnum */
    uint64_t trace_traparg0;

    /* LR/SC reservation state (bring-up model). */
    uint64_t lr_addr;
    uint32_t lr_size;
    uint32_t lr_valid;

    /* Direct-mapped hot-path cache for bstart target validation. */
    uint64_t bstart_cache_tag[LINX_BSTART_CACHE_SIZE];
    uint8_t bstart_cache_valid[LINX_BSTART_CACHE_SIZE];

    /*
     * Runtime-specialized fast-path state.
     * These bits are folded into TB flags by cpu.c.
     */
    uint8_t tb_dbg_active;
    uint8_t tb_cosim_precheck;

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;

    /* Loader-provided B.TEXT body extent metadata. Not reset-cleared. */
    uint32_t body_range_count;
    LinxBodyRange *body_ranges;
    uint32_t call_continuation_count;
    uint64_t *call_continuations;

    /* JSONL commit tracing (bring-up difftest). Not reset-cleared. */
    struct {
        uint8_t inited;
        uint8_t enabled;
        uint8_t pc_filter_enabled;
        uint8_t stop_after_commit;
        uint64_t pc_lo;
        uint64_t pc_hi;
        uint64_t cycle;
        FILE *fp;
    } commit_trace;

    struct {
        uint8_t inited;
        uint8_t enabled;
        uint8_t pc_filter_enabled;
        uint8_t stop_after_commit;
        uint8_t pc_bias_valid;
        uint8_t pending_block_kind;
        uint8_t active_block_kind;
        uint64_t pc_lo;
        uint64_t pc_hi;
        uint64_t pc_bias;
        uint64_t cycle;
        FILE *fp;
    } minst_trace;

    /* QEMU <-> Janus lockstep co-simulation state (M1 bring-up). */
    struct {
        uint8_t inited;
        uint8_t enabled;
        uint8_t active;
        uint8_t ended;
        uint64_t trigger_pc;
        uint64_t terminate_pc;
        uint64_t max_commits;
        uint64_t seq;
        int sock_fd;
        uint32_t range_count;
        char socket_path[LINX_COSIM_PATH_MAX];
        char snapshot_path[LINX_COSIM_PATH_MAX];
        LinxCosimRange ranges[LINX_COSIM_MAX_RANGES];
    } cosim;

    /* Per-CPU virtual timer for TIMER_TIMECMP (bring-up). */
    struct QEMUTimer *timer;
} CPULinxState;

static inline bool linx_ebarg_stack_push(CPULinxState *env, uint32_t acr)
{
    uint32_t d;
    uint32_t i;

    if (acr >= LINX_ACR_COUNT || acr != 1) {
        return false;
    }
    d = env->ebarg_stack_depth;
    if (d >= LINX_EBARG_STACK_DEPTH) {
        return false;
    }
    for (i = 0; i < LINX_SSR_EBARG_COUNT; i++) {
        env->ebarg_stack[d][i] = env->ssr_acr[acr][LINX_SSR_EBARG_BASE + i];
    }
    env->ebarg_stack_depth = d + 1;
    return true;
}

static inline bool linx_ebarg_stack_pop_restore(CPULinxState *env, uint32_t acr)
{
    uint32_t d;
    uint32_t i;

    if (acr >= LINX_ACR_COUNT || acr != 1) {
        return false;
    }
    d = env->ebarg_stack_depth;
    if (d == 0) {
        return false;
    }
    d -= 1;
    for (i = 0; i < LINX_SSR_EBARG_COUNT; i++) {
        env->ssr_acr[acr][LINX_SSR_EBARG_BASE + i] = env->ebarg_stack[d][i];
    }
    env->ebarg_stack_depth = d;
    return true;
}

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
    for (i = 0; i < LINX_VEC_QUEUE_DEPTH; i++) {
        s->vtq[i] = env->vtq[i];
        s->vuq[i] = env->vuq[i];
        s->vmq[i] = env->vmq[i];
        s->vnq[i] = env->vnq[i];
    }
    s->vec_p = env->vec_p;

    s->bpc = env->bpc;

    s->tgt = env->tgt;
    s->cond = env->cond;
    s->carg = env->carg;
    s->brtype = env->brtype;
    s->blocktype = env->blocktype;
    s->call_ra_set = env->call_ra_set;
    s->call_setret_pending = env->call_setret_pending;

    s->body_tpc = env->body_tpc;
    s->body_end = env->body_end;
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
    for (i = 0; i < 3; i++) {
        s->lc[i] = env->lc[i];
    }

    s->tile_func = env->tile_func;
    s->tile_dtype = env->tile_dtype;
    s->tile_desc_valid = env->tile_desc_valid;
    s->tile_arg_format = env->tile_arg_format;
    s->tile_attr_raw = env->tile_attr_raw;
    s->tile_attr_pad = env->tile_attr_pad;
    s->tile_attr_dtype = env->tile_attr_dtype;
    s->tile_ior_count = env->tile_ior_count;
    for (i = 0; i < LINX_TILE_MAX_IOR; i++) {
        s->tile_ior_desc[i] = env->tile_ior_desc[i];
    }
    s->vec_ri_count = env->vec_ri_count;
    for (i = 0; i < LINX_VEC_RI_MAX; i++) {
        s->vec_ri_value[i] = env->vec_ri_value[i];
    }
    s->tile_itp_count = env->tile_itp_count;
    for (i = 0; i < LINX_TILE_MAX_ITP; i++) {
        s->tile_itp_desc[i] = env->tile_itp_desc[i];
    }
    s->tile_ota_count = env->tile_ota_count;
    for (i = 0; i < LINX_TILE_MAX_OTA; i++) {
        s->tile_ota_desc[i] = env->tile_ota_desc[i];
    }
}

static inline void linx_acr_reset_block_state_for_header(CPULinxState *env,
                                                         uint32_t acr,
                                                         uint64_t bpc)
{
    LinxAcrBlockState *s;

    if (acr >= LINX_ACR_COUNT) {
        return;
    }

    s = &env->acr_block_state[acr];
    memset(s, 0, sizeof(*s));
    s->bpc = bpc;
    s->tile_dtype = 17; /* INT32 default in v0.3 DataType. */
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
    for (i = 0; i < LINX_VEC_QUEUE_DEPTH; i++) {
        env->vtq[i] = s->vtq[i];
        env->vuq[i] = s->vuq[i];
        env->vmq[i] = s->vmq[i];
        env->vnq[i] = s->vnq[i];
    }
    env->vec_p = s->vec_p;

    env->bpc = s->bpc;

    env->tgt = s->tgt;
    env->cond = s->cond;
    env->carg = s->carg;
    env->brtype = s->brtype;
    env->blocktype = s->blocktype;
    env->call_ra_set = s->call_ra_set;
    env->call_setret_pending = s->call_setret_pending;

    env->body_tpc = s->body_tpc;
    env->body_end = s->body_end;
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
    for (i = 0; i < 3; i++) {
        env->lc[i] = s->lc[i];
    }

    env->tile_func = s->tile_func;
    env->tile_dtype = s->tile_dtype;
    env->tile_desc_valid = s->tile_desc_valid;
    env->tile_arg_format = s->tile_arg_format;
    env->tile_attr_raw = s->tile_attr_raw;
    env->tile_attr_pad = s->tile_attr_pad;
    env->tile_attr_dtype = s->tile_attr_dtype;
    env->tile_ior_count = s->tile_ior_count;
    for (i = 0; i < LINX_TILE_MAX_IOR; i++) {
        env->tile_ior_desc[i] = s->tile_ior_desc[i];
    }
    env->vec_ri_count = s->vec_ri_count;
    for (i = 0; i < LINX_VEC_RI_MAX; i++) {
        env->vec_ri_value[i] = s->vec_ri_value[i];
    }
    env->tile_itp_count = s->tile_itp_count;
    for (i = 0; i < LINX_TILE_MAX_ITP; i++) {
        env->tile_itp_desc[i] = s->tile_itp_desc[i];
    }
    env->tile_ota_count = s->tile_ota_count;
    for (i = 0; i < LINX_TILE_MAX_OTA; i++) {
        env->tile_ota_desc[i] = s->tile_ota_desc[i];
    }
}

/*
 * LinxCPU:
 * @env: #CPULinxState
 */
struct ArchCPU {
    CPUState parent_obj;

    CPULinxState env;

    /* Machine-provided bootstrap state restored on every CPU reset. */
    uint64_t boot_pc;
    uint64_t boot_sp;
    uint64_t boot_ra;
    uint64_t boot_a0;
    uint64_t boot_a1;
    uint64_t boot_a2;

    /* Optional bring-up DFX: insert a CPU watchpoint on realize(). */
    uint64_t dfx_watch_addr;
    uint32_t dfx_watch_len;
    uint32_t dfx_watch_flags;
};

struct LinxCPUClass {
    CPUClass parent_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

void linx_translate_init(void);
void linx_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc);
void linx_call_trace_dump_recent(CPULinxState *env, const char *reason,
                                 uint64_t fault_pc);
void linx_debug_pc_watch_dump_recent(CPULinxState *env, const char *reason,
                                     uint64_t fault_pc);

static inline uint64_t linx_lookup_body_end(const CPULinxState *env,
                                            uint64_t body_tpc)
{
    uint32_t i;

    if (!env->body_ranges) {
        return 0;
    }
    for (i = 0; i < env->body_range_count; i++) {
        if (env->body_ranges[i].start == body_tpc) {
            return env->body_ranges[i].end;
        }
    }
    return 0;
}

static inline bool linx_is_call_continuation(const CPULinxState *env,
                                             uint64_t pc)
{
    uint32_t i;

    if (!env->call_continuations) {
        return false;
    }
    for (i = 0; i < env->call_continuation_count; i++) {
        if (env->call_continuations[i] == pc) {
            return true;
        }
    }
    return false;
}

#endif
