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
#include "qemu/thread.h"

#ifdef CONFIG_USER_ONLY
#error "LinxISA does not support user mode emulation"
#endif

/* Stable Linx test-finisher MMIO contract. */
#define LINX_VIRT_FINISHER_ADDR UINT64_C(0x10009000)
#define LINX_VIRT_FINISHER_FAIL UINT64_C(0x3333)
#define LINX_VIRT_FINISHER_PASS UINT64_C(0x5555)
#define LINX_VIRT_FINISHER_RESET UINT64_C(0x7777)

#define LINX_CORE4_PE_COUNT 4
#define LINX_SHARED_TILE_COUNT 256
#define LINX_SHARED_TILE_MAX_BYTES (32 * 1024)
#define LINX_RAW_TILE_TRANSPORT_MAX 8

/* Compiler-generated S64/NORM spill metadata.  This is a model-internal
 * transport record; ordinary typed TLOAD/TSTORE never consults it. */
typedef struct LinxRawTileTransport {
    uint8_t valid;
    uint8_t elem_bytes;
    uint8_t dtype;
    uint8_t layout;
    uint16_t valid_cols;
    uint16_t valid_rows;
    uint16_t cols;
    uint16_t rows;
    uint32_t capacity;
    uint32_t bytes;
    uint64_t base;
    uint64_t transfer_rows;
    uint64_t row_bytes;
    uint64_t stride_bytes;
    uint64_t span_bytes;
} LinxRawTileTransport;

typedef struct LinxSharedTileVersion {
    /* The architectural Shared register is one aggregate tile payload.  The
     * four mask bits select fixed-offset regions in this payload; they are not
     * four independent full-tile lanes. */
    uint8_t data[LINX_SHARED_TILE_MAX_BYTES];
    /* B.IOS.TSize is the aggregate payload capacity for the core.  The
     * historical field name is retained for migration compatibility; it no
     * longer denotes a single-PE allocation. */
    uint32_t per_pe_capacity;
    uint32_t allocated_bytes;
    uint32_t dtype;
    uint8_t layout;
    uint16_t valid_cols;
    uint16_t valid_rows;
    uint16_t cols;
    uint16_t rows;
    uint64_t producer_bpc;
    uint8_t allocation_mask;
    uint8_t initialized_mask;
} LinxSharedTileVersion;

typedef struct LinxCore4State {
    QemuMutex lock;
    QemuCond collective_cond;
    LinxCPU *cpu[LINX_CORE4_PE_COUNT];
    LinxSharedTileVersion shared_tile[LINX_SHARED_TILE_COUNT];
    uint64_t collective_bpc;
    uint32_t collective_func;
    uint32_t collective_dtype;
    uint32_t collective_shared_id[2];
    uint8_t collective_shared_count;
    uint32_t collective_m;
    uint32_t collective_n;
    uint32_t collective_k;
    uint8_t collective_arrived;
    uint8_t collective_src[LINX_CORE4_PE_COUNT];
    uint8_t collective_dst[LINX_CORE4_PE_COUNT];
    uint8_t collective_peer[LINX_CORE4_PE_COUNT];
    uint8_t collective_pe_mask;
    uint8_t collective_size_code;
    uint64_t collective_resume_pc[LINX_CORE4_PE_COUNT];
} LinxCore4State;

void linx_core4_reset(LinxCore4State *core4);

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
    LINX_EXCP_TILE_FAULT = 11, /* Precise Tile legality/allocation fault */
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
    LINX_BLOCKFMT_FAMILY_IOT  = 0x05,
    LINX_BLOCKFMT_FAMILY_FPATR = 0x06,
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
#define LINX_SSR_PEID 0x802u   /* DavinciOO v5 read-only PE identifier */
#define LINX_ACR_COUNT 16u     /* ACR0..ACR15 */
#define LINX_TILE_MAX_IOR 16u
#define LINX_TILE_MAX_IOT 32u
#define LINX_TILE_MAX_SHARED_BINDERS 2u
#define LINX_TILE_HAND_COUNT 4u
#define LINX_TILE_HAND_DEPTH 16u
#define LINX_TILE_SLOT_COUNT (LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH)
#define LINX_TILE_HAND_BIT(depth) ((uint16_t)(1u << (depth)))
#define LINX_VEC_RI_MAX (LINX_TILE_MAX_IOR * 3u)
/*
 * v0.3 SIMT bring-up: LLVM autovec may reference VT indices up to VT#31 in
 * workloads (TSVC), so the emulator must model at least that depth.
 */
#define LINX_VEC_QUEUE_DEPTH 32u
#define LINX_COSIM_MAX_RANGES 256u
#define LINX_COSIM_PATH_MAX 512u
#define LINX_BSTART_CACHE_SIZE 8192u
#define LINX_MMU_CACHE_SIZE 2048u
#define LINX_TLB_FILL_HOT_SLOTS 16u
#define LINX_TLB_INV_HOT_SLOTS 16u
#define LINX_FRAME_SHAPE_HOT_SLOTS 64u

#define LINX_TB_FLAG_IN_BODY (1u << 0)
#define LINX_TB_FLAG_USER_MMU (1u << 1)
#define LINX_TB_FLAG_DBG_ACTIVE (1u << 2)
#define LINX_TB_FLAG_COSIM_PRECHECK (1u << 3)
#define LINX_TB_FLAG_FUSED_ICALL (1u << 4)

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
#define LINX_TILE_CELL_BYTES 128u
#define LINX_TILE_PE_CAPACITY_BYTES (256u * 1024u)
#define LINX_TILE_MAX_BYTES (64u * 1024u)
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
    uint64_t fused_icall_target;
    uint32_t fused_icall_target_valid;

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
    uint32_t tile_iot_valid;
    uint32_t tile_iot_flags;
    uint32_t tile_iot_dst;
    uint32_t tile_iot_grp;
    uint32_t tile_iot_src0;
    uint32_t tile_iot_src1;
    uint32_t tile_iot_reg;
    uint32_t tile_iot_size;

    uint32_t tile_arg_format;
    uint32_t tile_attr_raw;
    uint32_t tile_attr_pad;
    uint32_t tile_attr_dtype;
    uint32_t tile_fpatr_raw;
    uint32_t tile_fpatr_valid;
    uint32_t tile_ior_count;
    uint64_t tile_ior_desc[LINX_TILE_MAX_IOR];
    uint32_t vec_ri_count;
    uint64_t vec_ri_value[LINX_VEC_RI_MAX];
    uint32_t tile_iot_count;
    uint32_t tile_shared_binder_count;
    uint32_t tile_shared_binder[LINX_TILE_MAX_SHARED_BINDERS];
    uint64_t tile_iot_desc[LINX_TILE_MAX_IOT];
    uint8_t tile_iot_src_valid[LINX_TILE_MAX_IOT];
    uint8_t tile_iot_src_phys[LINX_TILE_MAX_IOT][2];
    uint8_t tile_iot_output_valid[LINX_TILE_MAX_IOT];
    uint8_t tile_iot_output_phys[LINX_TILE_MAX_IOT];

    /* CUBE ACC is implicit architectural state and follows its ACR. */
    uint32_t tile_acc[LINX_TILE_MAX_WORDS];
    uint32_t tile_acc_bytes;
    uint8_t tile_acc_dtype;
    uint8_t tile_acc_valid;
    uint16_t tile_acc_cols;
    uint16_t tile_acc_rows;
} LinxAcrBlockState;

typedef struct LinxCosimRange {
    uint64_t base;
    uint64_t size;
} LinxCosimRange;

typedef struct LinxBodyRange {
    uint64_t start;
    uint64_t end;
} LinxBodyRange;

typedef struct LinxMmuCacheEntry {
    uint64_t tag;
    uint64_t pbase;
    uint64_t tlb_size;
    uint32_t prot;
    uint8_t valid;
    uint8_t mmu_idx;
} LinxMmuCacheEntry;

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
    uint64_t fused_icall_target;
    uint32_t fused_icall_target_valid;

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

    /* Tile block header state and frozen multi-B.IOT queue bindings. */
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

    uint32_t tile_arg_format;
    uint32_t tile_attr_raw;
    uint32_t tile_attr_pad;
    uint32_t tile_attr_dtype;
    uint32_t tile_fpatr_raw;
    uint32_t tile_fpatr_valid;
    uint32_t tile_ior_count;
    uint64_t tile_ior_desc[LINX_TILE_MAX_IOR];
    uint32_t vec_ri_count;
    uint64_t vec_ri_value[LINX_VEC_RI_MAX];
    uint32_t tile_iot_count;
    uint32_t tile_shared_binder_count;
    uint32_t tile_shared_binder[LINX_TILE_MAX_SHARED_BINDERS];
    uint64_t tile_iot_desc[LINX_TILE_MAX_IOT];
    uint8_t tile_iot_src_valid[LINX_TILE_MAX_IOT];
    uint8_t tile_iot_src_phys[LINX_TILE_MAX_IOT][2];
    uint8_t tile_iot_output_valid[LINX_TILE_MAX_IOT];
    uint8_t tile_iot_output_phys[LINX_TILE_MAX_IOT];

    /*
     * Shared 4x16 tile backing and allocation state for the current model.
     * These fields are deliberately not ACR-switched: tile_reg[] is
     * shared as well, so restoring per-ACR liveness would describe stale or
     * overwritten global backing.
     */
    uint16_t tile_hand_live[LINX_TILE_HAND_COUNT];
    uint16_t tile_hand_reserved[LINX_TILE_HAND_COUNT];
    /* Per-hand architectural rank (index 0 is #1/newest) -> physical tile. */
    uint8_t tile_hand_order[LINX_TILE_HAND_COUNT][LINX_TILE_HAND_DEPTH];
    uint8_t tile_hand_count[LINX_TILE_HAND_COUNT];
    /* ACR owner bits pin header-frozen physical sources across ACR switches. */
    uint16_t tile_pin_owner[LINX_TILE_SLOT_COUNT];
    uint8_t tile_acc_carrier_valid;
    uint8_t tile_acc_carrier;
    uint8_t tile_acc_sources_valid;
    uint8_t tile_acc_src0;
    uint8_t tile_acc_src1;

    /* Emulated tile backing store: 4 hands x 16 depth = 64 tiles. */
    uint32_t tile_reg[LINX_TILE_SLOT_COUNT][LINX_TILE_MAX_WORDS];
    uint32_t tile_reg_capacity[LINX_TILE_SLOT_COUNT];
    uint32_t tile_reg_bytes[LINX_TILE_SLOT_COUNT];
    uint8_t tile_reg_elem_bytes[LINX_TILE_SLOT_COUNT];
    uint8_t tile_reg_dtype[LINX_TILE_SLOT_COUNT];
    uint8_t tile_reg_layout[LINX_TILE_SLOT_COUNT];
    uint16_t tile_reg_valid_cols[LINX_TILE_SLOT_COUNT];
    uint16_t tile_reg_valid_rows[LINX_TILE_SLOT_COUNT];
    uint16_t tile_reg_cols[LINX_TILE_SLOT_COUNT];
    uint16_t tile_reg_rows[LINX_TILE_SLOT_COUNT];

    LinxRawTileTransport raw_tile_transport[LINX_RAW_TILE_TRANSPORT_MAX];
    uint8_t raw_tile_transport_active;

    /* Accumulator backing store (separate scratch). */
    uint32_t tile_acc[LINX_TILE_MAX_WORDS];
    uint32_t tile_acc_bytes;
    uint8_t tile_acc_dtype;
    uint8_t tile_acc_valid;
    uint16_t tile_acc_cols;
    uint16_t tile_acc_rows;

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
    uint32_t trace_capture_disabled_fast;

    /* LR/SC reservation state (bring-up model). */
    uint64_t lr_addr;
    uint32_t lr_size;
    uint32_t lr_valid;

    /* Direct-mapped hot-path cache for bstart target validation. */
    uint64_t bstart_cache_tag[LINX_BSTART_CACHE_SIZE];
    uint8_t bstart_cache_mmu_idx[LINX_BSTART_CACHE_SIZE];
    uint8_t bstart_cache_valid[LINX_BSTART_CACHE_SIZE];

    /* Page-walk result cache for QEMU TLB miss handling. */
    LinxMmuCacheEntry mmu_cache[LINX_MMU_CACHE_SIZE];
    uint8_t mmu_cache_next_way[LINX_MMU_CACHE_SIZE / 2u];
    LinxMmuCacheEntry mmu_cache_victim;
    uint64_t mmu_cache_hits;
    uint64_t mmu_cache_misses;
    uint64_t mmu_cache_fills;
    uint64_t mmu_cache_flushes;
    uint64_t mmu_cache_page_flushes;
    uint64_t mmu_cache_collisions;
    uint64_t mmu_cache_victim_hits;
    uint64_t mmu_cache_victim_fills;
    uint64_t mmu_cache_hit_4k;
    uint64_t mmu_cache_hit_2m;
    uint64_t mmu_cache_hit_1g;
    uint64_t mmu_cache_hit_512g;
    uint64_t mmu_cache_fill_4k;
    uint64_t mmu_cache_fill_2m;
    uint64_t mmu_cache_fill_1g;
    uint64_t mmu_cache_fill_512g;
    uint64_t mmu_cache_collision_4k;
    uint64_t mmu_cache_collision_2m;
    uint64_t mmu_cache_collision_1g;
    uint64_t mmu_cache_collision_512g;

    /* Opt-in TLB invalidation counters for Linux/SPEC throughput triage. */
    uint64_t tlb_inv_iall;
    uint64_t tlb_inv_ia;
    uint64_t tlb_inv_iv;
    uint64_t tlb_inv_iav;
    uint64_t tlb_inv_last_count;
    uint64_t tlb_inv_last_pc;
    uint64_t tlb_inv_last_bpc;
    uint64_t tlb_inv_last_operand;
    uint8_t tlb_inv_last_acr;

    /*
     * Optional source-PC sketch for SPEC/QEMU TLB invalidation triage.
     * Populated only when LINX_QEMU_TLB_INV_HOT is enabled.
     */
    uint8_t tlb_inv_hot_active;
    uint8_t tlb_inv_hot_valid[LINX_TLB_INV_HOT_SLOTS];
    uint8_t tlb_inv_hot_op[LINX_TLB_INV_HOT_SLOTS];
    uint8_t tlb_inv_hot_acr[LINX_TLB_INV_HOT_SLOTS];
    uint64_t tlb_inv_hot_count[LINX_TLB_INV_HOT_SLOTS];
    uint64_t tlb_inv_hot_emit_count[LINX_TLB_INV_HOT_SLOTS];
    uint64_t tlb_inv_hot_pc[LINX_TLB_INV_HOT_SLOTS];
    uint64_t tlb_inv_hot_last_bpc[LINX_TLB_INV_HOT_SLOTS];
    uint64_t tlb_inv_hot_last_operand[LINX_TLB_INV_HOT_SLOTS];
    uint64_t tlb_inv_hot_last_page[LINX_TLB_INV_HOT_SLOTS];
    uint64_t tlb_inv_hot_evictions;

    /* Opt-in TLB fill counters for Linux/SPEC throughput triage. */
    uint64_t tlb_fill_total;
    uint64_t tlb_fill_fetch;
    uint64_t tlb_fill_load;
    uint64_t tlb_fill_store;
    uint64_t tlb_fill_probe;
    uint64_t tlb_fill_ok;
    uint64_t tlb_fill_fault;
    uint64_t tlb_fill_user;
    uint64_t tlb_fill_user_fetch;
    uint64_t tlb_fill_user_load;
    uint64_t tlb_fill_user_store;
    uint64_t tlb_fill_kernel;
    uint64_t tlb_fill_kernel_fetch;
    uint64_t tlb_fill_kernel_load;
    uint64_t tlb_fill_kernel_store;
    uint64_t tlb_fill_other;
    uint64_t tlb_fill_last_count;
    uint64_t tlb_fill_last_pc;
    uint64_t tlb_fill_last_bpc;
    uint64_t tlb_fill_last_va;
    uint64_t tlb_fill_last_pa;
    uint32_t tlb_fill_last_access;
    uint32_t tlb_fill_last_mmu_idx;
    uint32_t tlb_fill_last_prot;
    uint32_t tlb_fill_last_cause;
    uint8_t tlb_fill_last_acr;

    /*
     * Optional hot-page sketch for SPEC/QEMU TLB-fill triage. Populated only
     * when LINX_QEMU_TLB_FILL_HOT is enabled.
     */
    uint8_t tlb_fill_hot_active;
    uint8_t tlb_fill_hot_last_slot;
    uint8_t tlb_fill_hot_valid[LINX_TLB_FILL_HOT_SLOTS];
    uint8_t tlb_fill_hot_access[LINX_TLB_FILL_HOT_SLOTS];
    uint8_t tlb_fill_hot_mmu[LINX_TLB_FILL_HOT_SLOTS];
    uint8_t tlb_fill_hot_probe[LINX_TLB_FILL_HOT_SLOTS];
    uint8_t tlb_fill_hot_acr[LINX_TLB_FILL_HOT_SLOTS];
    uint32_t tlb_fill_hot_prot[LINX_TLB_FILL_HOT_SLOTS];
    uint32_t tlb_fill_hot_cause[LINX_TLB_FILL_HOT_SLOTS];
    uint64_t tlb_fill_hot_count[LINX_TLB_FILL_HOT_SLOTS];
    uint64_t tlb_fill_hot_page[LINX_TLB_FILL_HOT_SLOTS];
    uint64_t tlb_fill_hot_last_va[LINX_TLB_FILL_HOT_SLOTS];
    uint64_t tlb_fill_hot_last_pa[LINX_TLB_FILL_HOT_SLOTS];
    uint64_t tlb_fill_hot_last_pc[LINX_TLB_FILL_HOT_SLOTS];
    uint64_t tlb_fill_hot_last_bpc[LINX_TLB_FILL_HOT_SLOTS];
    uint64_t tlb_fill_hot_evictions;
    uint64_t tlb_fill_hot_inserts;
    uint64_t tlb_fill_hot_last_hits;
    uint64_t tlb_fill_hot_slot_hits;

    /*
     * Optional frame-template shape sketch for SPEC/QEMU throughput triage.
     * Populated only when LINX_QEMU_FRAME_SHAPE_HOT is enabled.
     */
    uint8_t frame_shape_hot_active;
    uint8_t frame_shape_hot_valid[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint8_t frame_shape_hot_kind[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint8_t frame_shape_hot_begin[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint8_t frame_shape_hot_end[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint8_t frame_shape_hot_reg_count[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint64_t frame_shape_hot_stacksize[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint64_t frame_shape_hot_count[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint64_t frame_shape_hot_emit_count[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint64_t frame_shape_hot_frame_slots[LINX_FRAME_SHAPE_HOT_SLOTS];
    uint64_t frame_shape_hot_evictions;

    /*
     * Runtime-specialized fast-path state.
     * These bits are folded into TB flags by cpu.c.
     */
    uint8_t tb_dbg_active;
    uint8_t tb_cosim_precheck;

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;

    /* Machine-assigned Core4 PE identity. This survives architectural reset. */
    uint32_t pe_id;

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
    s->fused_icall_target = env->fused_icall_target;
    s->fused_icall_target_valid = env->fused_icall_target_valid;

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
    s->tile_iot_valid = env->tile_iot_valid;
    s->tile_iot_flags = env->tile_iot_flags;
    s->tile_iot_dst = env->tile_iot_dst;
    s->tile_iot_grp = env->tile_iot_grp;
    s->tile_iot_src0 = env->tile_iot_src0;
    s->tile_iot_src1 = env->tile_iot_src1;
    s->tile_iot_reg = env->tile_iot_reg;
    s->tile_iot_size = env->tile_iot_size;
    s->tile_arg_format = env->tile_arg_format;
    s->tile_attr_raw = env->tile_attr_raw;
    s->tile_attr_pad = env->tile_attr_pad;
    s->tile_attr_dtype = env->tile_attr_dtype;
    s->tile_fpatr_raw = env->tile_fpatr_raw;
    s->tile_fpatr_valid = env->tile_fpatr_valid;
    s->tile_ior_count = env->tile_ior_count;
    for (i = 0; i < LINX_TILE_MAX_IOR; i++) {
        s->tile_ior_desc[i] = env->tile_ior_desc[i];
    }
    s->vec_ri_count = env->vec_ri_count;
    for (i = 0; i < LINX_VEC_RI_MAX; i++) {
        s->vec_ri_value[i] = env->vec_ri_value[i];
    }
    s->tile_iot_count = env->tile_iot_count;
    s->tile_shared_binder_count = env->tile_shared_binder_count;
    for (i = 0; i < LINX_TILE_MAX_SHARED_BINDERS; i++) {
        s->tile_shared_binder[i] = env->tile_shared_binder[i];
    }
    for (i = 0; i < LINX_TILE_MAX_IOT; i++) {
        s->tile_iot_desc[i] = env->tile_iot_desc[i];
        s->tile_iot_src_valid[i] = env->tile_iot_src_valid[i];
        s->tile_iot_src_phys[i][0] = env->tile_iot_src_phys[i][0];
        s->tile_iot_src_phys[i][1] = env->tile_iot_src_phys[i][1];
        s->tile_iot_output_valid[i] = env->tile_iot_output_valid[i];
        s->tile_iot_output_phys[i] = env->tile_iot_output_phys[i];
    }
    memcpy(s->tile_acc, env->tile_acc, sizeof(s->tile_acc));
    s->tile_acc_bytes = env->tile_acc_bytes;
    s->tile_acc_dtype = env->tile_acc_dtype;
    s->tile_acc_valid = env->tile_acc_valid;
    s->tile_acc_cols = env->tile_acc_cols;
    s->tile_acc_rows = env->tile_acc_rows;
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
    const uint16_t owner = 1u << acr;
    for (unsigned i = 0; i < s->tile_iot_count; i++) {
        for (unsigned source = 0; source < 2; source++) {
            if ((s->tile_iot_src_valid[i] & (1u << source)) != 0) {
                env->tile_pin_owner[s->tile_iot_src_phys[i][source]] &=
                    ~owner;
            }
        }
        if (s->tile_iot_output_valid[i]) {
            const unsigned tile = s->tile_iot_output_phys[i];
            env->tile_hand_reserved[tile / LINX_TILE_HAND_DEPTH] &=
                ~LINX_TILE_HAND_BIT(tile % LINX_TILE_HAND_DEPTH);
        }
    }
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
    env->fused_icall_target = s->fused_icall_target;
    env->fused_icall_target_valid = s->fused_icall_target_valid;

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
    env->tile_iot_valid = s->tile_iot_valid;
    env->tile_iot_flags = s->tile_iot_flags;
    env->tile_iot_dst = s->tile_iot_dst;
    env->tile_iot_grp = s->tile_iot_grp;
    env->tile_iot_src0 = s->tile_iot_src0;
    env->tile_iot_src1 = s->tile_iot_src1;
    env->tile_iot_reg = s->tile_iot_reg;
    env->tile_iot_size = s->tile_iot_size;
    env->tile_arg_format = s->tile_arg_format;
    env->tile_attr_raw = s->tile_attr_raw;
    env->tile_attr_pad = s->tile_attr_pad;
    env->tile_attr_dtype = s->tile_attr_dtype;
    env->tile_fpatr_raw = s->tile_fpatr_raw;
    env->tile_fpatr_valid = s->tile_fpatr_valid;
    env->tile_ior_count = s->tile_ior_count;
    for (i = 0; i < LINX_TILE_MAX_IOR; i++) {
        env->tile_ior_desc[i] = s->tile_ior_desc[i];
    }
    env->vec_ri_count = s->vec_ri_count;
    for (i = 0; i < LINX_VEC_RI_MAX; i++) {
        env->vec_ri_value[i] = s->vec_ri_value[i];
    }
    env->tile_iot_count = s->tile_iot_count;
    env->tile_shared_binder_count = s->tile_shared_binder_count;
    for (i = 0; i < LINX_TILE_MAX_SHARED_BINDERS; i++) {
        env->tile_shared_binder[i] = s->tile_shared_binder[i];
    }
    for (i = 0; i < LINX_TILE_MAX_IOT; i++) {
        env->tile_iot_desc[i] = s->tile_iot_desc[i];
        env->tile_iot_src_valid[i] = s->tile_iot_src_valid[i];
        env->tile_iot_src_phys[i][0] = s->tile_iot_src_phys[i][0];
        env->tile_iot_src_phys[i][1] = s->tile_iot_src_phys[i][1];
        env->tile_iot_output_valid[i] = s->tile_iot_output_valid[i];
        env->tile_iot_output_phys[i] = s->tile_iot_output_phys[i];
    }
    memcpy(env->tile_acc, s->tile_acc, sizeof(env->tile_acc));
    env->tile_acc_bytes = s->tile_acc_bytes;
    env->tile_acc_dtype = s->tile_acc_dtype;
    env->tile_acc_valid = s->tile_acc_valid;
    env->tile_acc_cols = s->tile_acc_cols;
    env->tile_acc_rows = s->tile_acc_rows;
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
    uint32_t boot_pe_id;

    /* Machine-owned functional state shared by all four DavinciOO PEs. */
    LinxCore4State *core4;

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
void linx_mmu_cache_flush(CPULinxState *env);
void linx_mmu_cache_flush_page(CPULinxState *env, uint64_t addr);

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
