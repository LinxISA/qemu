/*
 * LinxISA translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "exec/log.h"
#include "trace.h"
#include "opcode_meta.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPULinxState *env;

    uint8_t brtype;
    vaddr brtarget;
    uint32_t cur_insn_len;
    bool in_body;
    bool decoupled_header;
    bool tgt_modified;
    bool ra_set;
} DisasContext;

enum {
    LINX_BR_FALL   = 1,
    LINX_BR_DIRECT = 2,
    LINX_BR_COND   = 3,
    LINX_BR_CALL   = 4,
    LINX_BR_IND    = 5,
    LINX_BR_ICALL  = 6,
    LINX_BR_RET    = 7,
};

static TCGv_i64 cpu_gpr[LINX_GPR_COUNT];
static TCGv_i64 cpu_tq[4];
static TCGv_i64 cpu_uq[4];
static TCGv_i64 cpu_bpc;
static TCGv_i64 cpu_tgt;
static TCGv_i32 cpu_cond;
static TCGv_i32 cpu_carg;  /* Commit argument flag */
static TCGv_i32 cpu_brtype;
static TCGv_i32 cpu_blocktype;
static TCGv_i64 cpu_body_tpc;
static TCGv_i64 cpu_return_pc;
static TCGv_i32 cpu_in_body;
static TCGv_i32 cpu_tile_func;
static TCGv_i32 cpu_tile_dtype;
static TCGv_i32 cpu_tile_iot_valid;
static TCGv_i32 cpu_tile_iot_flags;
static TCGv_i32 cpu_tile_iot_dst;
static TCGv_i32 cpu_tile_iot_grp;
static TCGv_i32 cpu_tile_iot_src0;
static TCGv_i32 cpu_tile_iot_src1;
static TCGv_i32 cpu_tile_iot_reg;
static TCGv_i32 cpu_tile_iot_size;
static TCGv_i32 cpu_tile_attr_pad;
static TCGv_i32 cpu_tile_attr_dtype;
static TCGv_i64 cpu_lb[3];
static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_insn_pc_next;
static TCGv_i64 cpu_insn_count;
static TCGv_i64 cpu_pending_trap_arg0;
static TCGv_i32 cpu_pending_trap_cause;

/* Commit-trace scratch (JSONL). */
static TCGv_i64 cpu_trace_pc;
static TCGv_i64 cpu_trace_insn;
static TCGv_i32 cpu_trace_len;
static TCGv_i32 cpu_trace_wb_valid;
static TCGv_i32 cpu_trace_wb_rd;
static TCGv_i64 cpu_trace_wb_data;
static TCGv_i32 cpu_trace_mem_valid;
static TCGv_i32 cpu_trace_mem_is_store;
static TCGv_i64 cpu_trace_mem_addr;
static TCGv_i64 cpu_trace_mem_wdata;
static TCGv_i64 cpu_trace_mem_rdata;
static TCGv_i32 cpu_trace_mem_size;
static TCGv_i32 cpu_trace_trap_valid;
static TCGv_i32 cpu_trace_trap_cause;
static TCGv_i64 cpu_trace_traparg0;

static bool linx_commit_trace_enabled;
static bool linx_opcode_meta_strict = true;

static unsigned linx_insn_len(uint16_t hw);

static bool linx_watch_store_enabled;
static uint64_t linx_watch_store_lo;
static uint64_t linx_watch_store_hi;
static bool linx_watch_store_pc_filter_enabled;
static uint64_t linx_watch_store_pc_lo;
static uint64_t linx_watch_store_pc_hi;

static bool linx_watch_load_enabled;
static uint64_t linx_watch_load_lo;
static uint64_t linx_watch_load_hi;
static bool linx_watch_load_pc_filter_enabled;
static uint64_t linx_watch_load_pc_lo;
static uint64_t linx_watch_load_pc_hi;

static bool linx_trace_ra_enabled;
static bool linx_trace_ra_pc_enabled;
static uint64_t linx_trace_ra_pc;

static bool linx_trace_reg_enabled;
static uint32_t linx_trace_reg;
static bool linx_trace_reg_pc_filter_enabled;
static uint64_t linx_trace_reg_pc_lo;
static uint64_t linx_trace_reg_pc_hi;
static bool linx_illegal(DisasContext *ctx);

/*
 * Optional compatibility addend for frame templates.
 *
 * Default LinxISA template semantics use stacksize directly for SP adjust:
 *   f.entry:   sp -= stacksize
 *   f.exit/f.ret.*: sp += stacksize
 *
 * Set LINX_CALLFRAME_SIZE to a non-zero multiple of 8 only when running older
 * binaries that relied on an additional fixed outgoing-call frame.
 */
uint64_t linx_callframe_size = 0;

static inline bool linx_trace_ra_pc_match(vaddr pc)
{
    return !linx_trace_ra_pc_enabled || (uint64_t)pc == linx_trace_ra_pc;
}

static inline MemOp linx_mo_endian(void)
{
    return MO_LE;
}

static bool linx_validate_opcode_meta(DisasContext *ctx, vaddr pc, uint64_t insn_raw, unsigned len)
{
    const LinxOpcodeMeta *meta = linx_opcode_meta_lookup(insn_raw, len);
    if (meta) {
        return true;
    }
    if (!linx_opcode_meta_strict) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: opcode metadata missing @ PC=0x%" VADDR_PRIx " len=%u insn=0x%016" PRIx64 "\n",
                  pc, len, insn_raw);
    linx_illegal(ctx);
    return false;
}

static inline void linx_lr_invalidate(void)
{
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(CPULinxState, lr_valid));
}

static TCGv_i64 linx_get_reg(unsigned code)
{
    if (code == LINX_REG_ZERO) {
        return tcg_constant_i64(0);
    }
    if (code < LINX_GPR_COUNT) {
        return cpu_gpr[code];
    }
    if (code < 28) {
        return cpu_tq[code - 24];
    }
    return cpu_uq[code - 28];
}

static void linx_push_t(TCGv_i64 v)
{
    tcg_gen_mov_i64(cpu_tq[3], cpu_tq[2]);
    tcg_gen_mov_i64(cpu_tq[2], cpu_tq[1]);
    tcg_gen_mov_i64(cpu_tq[1], cpu_tq[0]);
    tcg_gen_mov_i64(cpu_tq[0], v);
}

static void linx_push_u(TCGv_i64 v)
{
    tcg_gen_mov_i64(cpu_uq[3], cpu_uq[2]);
    tcg_gen_mov_i64(cpu_uq[2], cpu_uq[1]);
    tcg_gen_mov_i64(cpu_uq[1], cpu_uq[0]);
    tcg_gen_mov_i64(cpu_uq[0], v);
}

static void linx_set_dest(unsigned dst, TCGv_i64 v)
{
    if (dst == 0) {
        return;
    }
    if (dst == 31) {
        linx_push_t(v);
        return;
    }
    if (dst == 30) {
        linx_push_u(v);
        return;
    }
    if (dst < LINX_GPR_COUNT) {
        tcg_gen_mov_i64(cpu_gpr[dst], v);
        if (linx_commit_trace_enabled) {
            tcg_gen_movi_i32(cpu_trace_wb_valid, 1);
            tcg_gen_movi_i32(cpu_trace_wb_rd, (int32_t)dst);
            tcg_gen_mov_i64(cpu_trace_wb_data, v);
        }
    }
}

static inline void linx_trace_begin(vaddr pc, uint64_t insn_raw, unsigned len)
{
    if (!linx_commit_trace_enabled) {
        return;
    }

    tcg_gen_movi_i64(cpu_trace_pc, pc);
    tcg_gen_movi_i64(cpu_trace_insn, insn_raw);
    tcg_gen_movi_i32(cpu_trace_len, (int32_t)len);

    tcg_gen_movi_i32(cpu_trace_wb_valid, 0);
    tcg_gen_movi_i32(cpu_trace_wb_rd, 0);
    tcg_gen_movi_i64(cpu_trace_wb_data, 0);

    tcg_gen_movi_i32(cpu_trace_mem_valid, 0);
    tcg_gen_movi_i32(cpu_trace_mem_is_store, 0);
    tcg_gen_movi_i64(cpu_trace_mem_addr, 0);
    tcg_gen_movi_i64(cpu_trace_mem_wdata, 0);
    tcg_gen_movi_i64(cpu_trace_mem_rdata, 0);
    tcg_gen_movi_i32(cpu_trace_mem_size, 0);

    tcg_gen_movi_i32(cpu_trace_trap_valid, 0);
    tcg_gen_movi_i32(cpu_trace_trap_cause, 0);
    tcg_gen_movi_i64(cpu_trace_traparg0, 0);

    gen_helper_linx_trace_operands_begin(tcg_env, tcg_constant_i64(insn_raw), tcg_constant_i32((int32_t)len));
}

static void linx_block_begin(DisasContext *ctx, uint8_t brtype, vaddr initial_target)
{
    int i;
    tcg_gen_movi_i64(cpu_bpc, ctx->base.pc_first);
    for (i = 0; i < 4; i++) {
        tcg_gen_movi_i64(cpu_tq[i], 0);
        tcg_gen_movi_i64(cpu_uq[i], 0);
    }
    tcg_gen_movi_i32(cpu_cond, 0);
    tcg_gen_movi_i32(cpu_carg, 0);
    tcg_gen_movi_i32(cpu_brtype, brtype);
    tcg_gen_movi_i32(cpu_blocktype, 0);
    tcg_gen_movi_i64(cpu_body_tpc, 0);
    tcg_gen_movi_i64(cpu_return_pc, 0);
    tcg_gen_movi_i32(cpu_in_body, 0);
    tcg_gen_movi_i32(cpu_tile_func, 0);
    tcg_gen_movi_i32(cpu_tile_dtype, 17); /* INT32 default in v0.3 DataType */
    tcg_gen_movi_i32(cpu_tile_iot_valid, 0);
    tcg_gen_movi_i32(cpu_tile_iot_flags, 0);
    tcg_gen_movi_i32(cpu_tile_iot_dst, 0);
    tcg_gen_movi_i32(cpu_tile_iot_grp, 0);
    tcg_gen_movi_i32(cpu_tile_iot_src0, 0);
    tcg_gen_movi_i32(cpu_tile_iot_src1, 0);
    tcg_gen_movi_i32(cpu_tile_iot_reg, 0);
    tcg_gen_movi_i32(cpu_tile_iot_size, 0);
    gen_helper_linx_tile_set_attr(tcg_env, tcg_constant_i32(0));
    gen_helper_linx_tile_reset_block(tcg_env);
    tcg_gen_movi_i64(cpu_lb[0], 0);
    tcg_gen_movi_i64(cpu_lb[1], 0);
    tcg_gen_movi_i64(cpu_lb[2], 0);
    ctx->tgt_modified = false;
    ctx->decoupled_header = false;
    ctx->ra_set = false;
    
    /* For COND blocks: set diverted target in bpc (cpu_tgt) */
    /* For DIRECT/CALL blocks: set target in bpc (cpu_tgt) */
    /* For FALL blocks: cpu_tgt remains 0 (unused) */
    if (brtype == LINX_BR_COND || brtype == LINX_BR_DIRECT || brtype == LINX_BR_CALL) {
        tcg_gen_movi_i64(cpu_tgt, initial_target);
    } else {
        tcg_gen_movi_i64(cpu_tgt, 0);
    }

    ctx->brtype = brtype;
    ctx->brtarget = initial_target; /* Keep for fallback/fallthrough calculation */
}

/* Helper to check if an address points to a block-start instruction.
 *
 * In addition to explicit BSTART encodings, LinxISA uses certain macro
 * instructions (FENTRY/FEXIT/FRET.*) as standalone blocks in the bring-up
 * toolchain.
 */
static bool linx_is_bstart_at_pc(CPULinxState *env, vaddr pc)
{
    CPUState *cs = env_cpu(env);
    uint8_t buf[8];

    if (cpu_memory_rw_debug(cs, pc, buf, 2, 0) != 0) {
        return false;
    }

    const uint16_t hw = lduw_le_p(buf);
    const unsigned len = linx_insn_len(hw);

    if (len == 2) {
        /* C.BSTART.STD / C.BSTART.FP: mask=0xc7ff, BrType in bits [13:11]. */
        if ((hw & 0xc7ff) == 0x0000 || (hw & 0xc7ff) == 0x0080) {
            const uint8_t brtype = (hw >> 11) & 0x7;
            if (brtype != 0) {
                return true;
            }
        }

        /* C.BSTART DIRECT/COND: check low nibble. */
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
        const uint32_t insn = ldl_le_p(buf);

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

        const uint16_t prefix = lduw_le_p(buf);
        const uint32_t main32 = ldl_le_p(buf + 2);
        if ((prefix & 0xf) != 0xe) {
            return false;
        }

        /* HL.BSTART.*: encoded as 16-bit prefix + 32-bit BSTART main part. */
        if ((main32 & 0xff) == 0x01 && ((main32 >> 12) & 0x7) != 0) {
            return true;
        }
        return false;
    }

    return false;
}


static void linx_gen_goto_tb(DisasContext *ctx, int slot, vaddr dest,
                             bool validate_target)
{
    if (validate_target) {
        /*
         * Validate branch targets at runtime so demand-paged text can fault-in
         * naturally. Fallthrough paths do not require explicit BSTART markers.
         */
        gen_helper_linx_check_bstart_target(tcg_env, tcg_constant_i64(dest));
    }

    if (linx_commit_trace_enabled) {
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(dest));
    }

    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(slot);
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, slot);
    } else {
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}


static void linx_gen_block_end(DisasContext *ctx, vaddr fallthrough)
{
    if (ctx->in_body) {
        /*
         * Decoupled body terminator:
         * - For SIMT/vector decoupled blocks, replay the body until the LB/LC
         *   loop nest completes, then return to the header continuation.
         * - For other decoupled bodies, return to the header continuation.
         */
        TCGLabel *done = gen_new_label();
        TCGv_i32 cont = tcg_temp_new_i32();
        gen_helper_linx_vec_body_next(cont, tcg_env);
        tcg_gen_brcondi_i32(TCG_COND_EQ, cont, 0, done);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_body_tpc);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_body_tpc);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        gen_set_label(done);

        tcg_gen_movi_i32(cpu_in_body, 0);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_return_pc);
        }
        gen_helper_linx_check_bstart_target(tcg_env, cpu_return_pc);
        tcg_gen_mov_i64(cpu_pc, cpu_return_pc);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }

    if (ctx->decoupled_header) {
        /*
         * Decoupled header terminator: jump to the out-of-line body specified
         * by B.TEXT, then resume at the header continuation (fallthrough).
         */
        TCGLabel *have_body = gen_new_label();
        tcg_gen_brcondi_i64(TCG_COND_NE, cpu_body_tpc, 0, have_body);
        tcg_gen_movi_i64(cpu_pending_trap_arg0, fallthrough);
        tcg_gen_movi_i32(cpu_pending_trap_cause, LINX_EBLOCK_CAUSE_MISSING_BODY_TPC);
        if (linx_commit_trace_enabled) {
            tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
            tcg_gen_movi_i32(cpu_trace_trap_cause,
                             (int32_t)((LINX_EBLOCK_CAUSE_MISSING_BODY_TPC << 8) | 5));
            tcg_gen_movi_i64(cpu_trace_traparg0, fallthrough);
            gen_helper_linx_commit_trace(tcg_env, cpu_bpc);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_bpc);
        gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_BLOCK_FAULT));
        tcg_gen_exit_tb(NULL, 0);
        gen_set_label(have_body);

        tcg_gen_movi_i64(cpu_return_pc, fallthrough);
        tcg_gen_movi_i32(cpu_in_body, 1);
        gen_helper_linx_vec_body_begin(tcg_env);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_body_tpc);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_body_tpc);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }

    /* Coupled block: commit any tile-block side effects before control-flow commit. */
    gen_helper_linx_tile_commit(tcg_env);

    switch (ctx->brtype & 0x7) {
    case LINX_BR_FALL:
        /* Always fall through */
        linx_gen_goto_tb(ctx, 0, fallthrough, false);
        break;
    case LINX_BR_DIRECT:
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            /*
             * Fast path: direct/call blocks with a fixed PC-relative target and no
             * SETC.TGT override can be emitted as a direct TB branch.
             */
            linx_gen_goto_tb(ctx, 0, ctx->brtarget, false);
        } else {
            /* Jump to cpu_tgt (diverted target from BSTART, or set target from SETC.TGT). */
            gen_helper_linx_check_bstart_target(tcg_env, cpu_tgt);
            if (linx_commit_trace_enabled) {
                gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
            }
            tcg_gen_mov_i64(cpu_pc, cpu_tgt);
            tcg_gen_lookup_and_goto_ptr();
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        break;
    case LINX_BR_CALL:
        /* CALL blocks return to the next block start unless SETRET overrode RA. */
        if (!ctx->ra_set) {
            linx_set_dest(LINX_REG_RA, tcg_constant_i64(fallthrough));
        }
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            linx_gen_goto_tb(ctx, 0, ctx->brtarget, false);
        } else {
            gen_helper_linx_check_bstart_target(tcg_env, cpu_tgt);
            if (linx_commit_trace_enabled) {
                gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
            }
            tcg_gen_mov_i64(cpu_pc, cpu_tgt);
            tcg_gen_lookup_and_goto_ptr();
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        break;
    case LINX_BR_COND: {
        /* Conditional jump: 
         * - If cpu_cond is set (and CARG), jump to diverted target (cpu_tgt)
         * - Otherwise fall through
         * Note: cpu_tgt may have been updated by SETC.TGT to override diverted target
         */
        TCGLabel *taken = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
        /* Condition not set: fall through */
        linx_gen_goto_tb(ctx, 1, fallthrough, false);
        gen_set_label(taken);
        /* Condition set: jump to diverted/set target */
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            /* Fixed target: enable TB chaining for the taken edge. */
            linx_gen_goto_tb(ctx, 0, ctx->brtarget, false);
        } else {
            if (linx_commit_trace_enabled) {
                gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
            }
            tcg_gen_mov_i64(cpu_pc, cpu_tgt);
            tcg_gen_lookup_and_goto_ptr();
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        break;
    }
    case LINX_BR_RET: {
        /* Return: if cpu_cond is set, jump to cpu_tgt, else fall through */
        TCGLabel *taken = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
        linx_gen_goto_tb(ctx, 1, fallthrough, false);
        gen_set_label(taken);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    }
    case LINX_BR_IND:
        /* Indirect jump: jump to cpu_tgt (must be set by SETC.TGT) */
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    case LINX_BR_ICALL:
        /*
         * Indirect call: like IND, but set RA to the fall-through block start
         * marker for return.
         */
        linx_set_dest(LINX_REG_RA, tcg_constant_i64(fallthrough));
        /* Indirect jump/call: jump to cpu_tgt (must be set by SETC.TGT) */
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    default:
        /* Unhandled block kind: fall through. */
        linx_gen_goto_tb(ctx, 0, fallthrough, false);
        break;
    }
}

static inline uint64_t G_GNUC_UNUSED linx_zext32(uint64_t x)
{
    return (uint32_t)x;
}

static inline uint64_t G_GNUC_UNUSED linx_sext32(uint64_t x)
{
    return (int64_t)(int32_t)x;
}

static TCGv_i64 linx_srcR_addsub(DisasContext *ctx, unsigned srcR,
                                 unsigned srcRType, unsigned shamt)
{
    TCGv_i64 r = linx_get_reg(srcR);
    TCGv_i64 tmp = tcg_temp_new_i64();

    switch (srcRType & 0x3) {
    case 0: /* .sw */
        tcg_gen_ext32s_i64(tmp, r);
        break;
    case 1: /* .uw */
        tcg_gen_ext32u_i64(tmp, r);
        break;
    case 2: /* .neg */
        tcg_gen_neg_i64(tmp, r);
        break;
    default:
        tcg_gen_mov_i64(tmp, r);
        break;
    }
    if (shamt) {
        tcg_gen_shli_i64(tmp, tmp, shamt & 0x3f);
    }
    return tmp;
}

static TCGv_i64 linx_srcR_logic(DisasContext *ctx, unsigned srcR,
                                unsigned srcRType, unsigned shamt)
{
    TCGv_i64 r = linx_get_reg(srcR);
    TCGv_i64 tmp = tcg_temp_new_i64();

    switch (srcRType & 0x3) {
    case 0: /* .sw */
        tcg_gen_ext32s_i64(tmp, r);
        break;
    case 1: /* .uw */
        tcg_gen_ext32u_i64(tmp, r);
        break;
    case 2: /* .not */
        tcg_gen_not_i64(tmp, r);
        break;
    default:
        tcg_gen_mov_i64(tmp, r);
        break;
    }
    if (shamt) {
        tcg_gen_shli_i64(tmp, tmp, shamt & 0x3f);
    }
    return tmp;
}

static vaddr linx_pcrel_target(vaddr pc, int64_t simm_hw)
{
    return pc + ((vaddr)simm_hw << 1);
}

static bool linx_illegal(DisasContext *ctx)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    qemu_log_mask(LOG_GUEST_ERROR, "Linx: illegal instruction @ 0x%" VADDR_PRIx " (insn_len=%u)\n",
                  pc, ctx->cur_insn_len);
    trace_linx_insn_decode_fail(ctx->base.pc_next, 0, ctx->cur_insn_len);
    tcg_gen_movi_i64(cpu_pending_trap_arg0, 0);
    tcg_gen_movi_i32(cpu_pending_trap_cause, 0);
    if (linx_commit_trace_enabled) {
        /* Trapnum=ILLEGAL_INST(4), cause=0. */
        tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
        tcg_gen_movi_i32(cpu_trace_trap_cause, 4);
        tcg_gen_movi_i64(cpu_trace_traparg0, 0);
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(pc));
    }
    tcg_gen_movi_i64(cpu_pc, pc);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_ILLEGAL_INST));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool linx_block_fault(DisasContext *ctx, uint32_t cause, uint64_t arg0)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: block-format fault @ 0x%" VADDR_PRIx " cause=%u\n",
                  pc, cause);
    tcg_gen_movi_i64(cpu_pending_trap_arg0, arg0);
    tcg_gen_movi_i32(cpu_pending_trap_cause, cause);
    if (linx_commit_trace_enabled) {
        /* Trapnum=BLOCK_TRAP(5), cause=block-format cause. */
        tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
        tcg_gen_movi_i32(cpu_trace_trap_cause,
                         (int32_t)(((cause & 0xffu) << 8) | 5));
        tcg_gen_movi_i64(cpu_trace_traparg0, arg0);
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(pc));
    }
    tcg_gen_movi_i64(cpu_pc, pc);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_BLOCK_FAULT));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* Include the auto-generated decoders. */
#include "decode-insn16.c.inc"
#include "decode-insn32.c.inc"
#include "decode-insn48.c.inc"
#include "decode-insn64.c.inc"

/* C.BSTART.STD handler - called explicitly from linx_tr_translate_insn */
static bool trans_c_bstart_std(DisasContext *ctx, uint8_t brtype)
{
    /* pc_next has already been advanced past the current insn, so we need to
     * check if the CURRENT instruction (pc_next - cur_insn_len) is at pc_first */
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        /* BSTART in the middle of a translation block - end the previous block */
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, brtype, 0);
    return true;
}

static bool trans_c_bstart_direct(DisasContext *ctx, arg_c_bstart_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_DIRECT, linx_pcrel_target(current_pc, a->simm12));
    return true;
}

static bool trans_c_bstart_cond(DisasContext *ctx, arg_c_bstart_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_COND, linx_pcrel_target(current_pc, a->simm12));
    return true;
}

static bool trans_c_bstop(DisasContext *ctx, arg_c_bstop *a)
{
    /* pc_next has already been advanced, so fallthrough is just pc_next */
    linx_gen_block_end(ctx, ctx->base.pc_next);
    return true;
}

static bool trans_bstop(DisasContext *ctx, arg_bstop *a)
{
    /* pc_next has already been advanced, so fallthrough is just pc_next */
    linx_gen_block_end(ctx, ctx->base.pc_next);
    return true;
}

static bool trans_bstart_call(DisasContext *ctx, arg_bstart_call *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_CALL, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_direct(DisasContext *ctx, arg_bstart_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_DIRECT, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_cond(DisasContext *ctx, arg_bstart_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_COND, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_ind(DisasContext *ctx, arg_bstart_ind *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_IND, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_icall(DisasContext *ctx, arg_bstart_icall *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_ICALL, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_ret(DisasContext *ctx, arg_bstart_ret *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_RET, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_par_common(DisasContext *ctx, uint32_t dtype, uint32_t op)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    op &= 0x3ffu;
    dtype &= 0x1fu;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_tile_dtype, dtype);

    switch (op) {
    case 33u:  /* TLOAD */
        tcg_gen_movi_i32(cpu_blocktype, 2); /* TMA */
        tcg_gen_movi_i32(cpu_tile_func, 0);
        break;
    case 65u:  /* TSTORE */
        tcg_gen_movi_i32(cpu_blocktype, 2); /* TMA */
        tcg_gen_movi_i32(cpu_tile_func, 1);
        break;
    case 66u:  /* MAMULB.ACC */
        tcg_gen_movi_i32(cpu_blocktype, 6); /* CUBE */
        tcg_gen_movi_i32(cpu_tile_func, 2);
        break;
    case 163u: /* PAR conversion helper in sampled streams */
        tcg_gen_movi_i32(cpu_blocktype, 2); /* TMA-like */
        tcg_gen_movi_i32(cpu_tile_func, 31); /* dedicated compatibility slot */
        break;
    case 258u: /* ACCCVT */
        tcg_gen_movi_i32(cpu_blocktype, 6); /* CUBE */
        tcg_gen_movi_i32(cpu_tile_func, 8);
        break;
    default:
        /*
         * Strict-v0.3 canonical TEPL path:
         * route packed PAR TileOp10 forms to TEPL block execution.
         */
        tcg_gen_movi_i32(cpu_blocktype, 7); /* TEPL */
        tcg_gen_movi_i32(cpu_tile_func, op & 0x3ff);
        break;
    }

    /* v0.3 bring-up: treat tile blocks as coupled template-style blocks. */
    ctx->decoupled_header = false;
    return true;
}

static bool trans_bstart_par(DisasContext *ctx, arg_bstart_par *a)
{
    return trans_bstart_par_common(ctx, a->dtype, a->op);
}

static bool trans_bstart_vpar(DisasContext *ctx, arg_bstart_vpar *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 4); /* VPAR */
    ctx->decoupled_header = true;
    return true;
}

static bool trans_bstart_vseq(DisasContext *ctx, arg_bstart_vseq *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 5); /* VSEQ */
    ctx->decoupled_header = true;
    return true;
}

static bool trans_bstart_mseq(DisasContext *ctx, arg_bstart_mseq *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 1); /* MSEQ: sequential vector with memory */
    ctx->decoupled_header = true;
    return true;
}

static bool trans_bstart_mpar(DisasContext *ctx, arg_bstart_mpar *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 0); /* MPAR: parallel vector with memory */
    ctx->decoupled_header = true;
    return true;
}

static bool trans_bstart_tma(DisasContext *ctx, arg_bstart_tma *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 2); /* TMA */
    tcg_gen_movi_i32(cpu_tile_func, a->func & 0x1f);
    tcg_gen_movi_i32(cpu_tile_dtype, a->dtype & 0x1f);
    ctx->decoupled_header = false;
    return true;
}

static bool trans_bstart_cube(DisasContext *ctx, arg_bstart_cube *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 6); /* CUBE */
    tcg_gen_movi_i32(cpu_tile_func, a->func & 0x1f);
    tcg_gen_movi_i32(cpu_tile_dtype, a->dtype & 0x1f);
    ctx->decoupled_header = false;
    return true;
}

static bool trans_b_dim_common(DisasContext *ctx, uint32_t reg, uint32_t uimm,
                               uint32_t lb)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    if (lb > 2) {
        return linx_illegal(ctx);
    }

    TCGv_i64 src = linx_get_reg(reg);
    const int64_t imm = (int64_t)(uimm & 0x1ffffu);
    tcg_gen_addi_i64(cpu_lb[lb], src, imm);
    return true;
}

static bool trans_c_b_dimi(DisasContext *ctx, arg_c_b_dimi *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    if (a->loopnest > 2u) {
        return linx_illegal(ctx);
    }

    tcg_gen_movi_i64(cpu_lb[a->loopnest], (uint64_t)(a->imm8 & 0xffu));
    return true;
}

static bool trans_b_dim_lb0(DisasContext *ctx, arg_b_dim_lb0 *a)
{
    return trans_b_dim_common(ctx, a->reg, a->uimm, 0);
}

static bool trans_b_dim_lb1(DisasContext *ctx, arg_b_dim_lb1 *a)
{
    return trans_b_dim_common(ctx, a->reg, a->uimm, 1);
}

static bool trans_b_dim_lb2(DisasContext *ctx, arg_b_dim_lb2 *a)
{
    return trans_b_dim_common(ctx, a->reg, a->uimm, 2);
}

static bool trans_b_arg(DisasContext *ctx, arg_b_arg *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }

    const uint32_t arg = a->format & 0x1f;
    if ((arg & 0x7u) > 4u) {
        return linx_illegal(ctx);
    }
    gen_helper_linx_tile_set_arg(tcg_env, tcg_constant_i32(arg));
    return true;
}

static bool trans_b_iot(DisasContext *ctx, arg_b_iot *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }

    uint32_t flags = 0;
    if (a->s0v) {
        flags |= 1u << 0;
    }
    if (a->s1v) {
        flags |= 1u << 1;
    }
    if (a->s0r) {
        flags |= 1u << 2;
    }
    if (a->s1r) {
        flags |= 1u << 3;
    }

    tcg_gen_movi_i32(cpu_tile_iot_valid, 1);
    tcg_gen_movi_i32(cpu_tile_iot_flags, flags);
    tcg_gen_movi_i32(cpu_tile_iot_dst, a->dst & 0x7);
    tcg_gen_movi_i32(cpu_tile_iot_grp, a->grp & 0x1);
    tcg_gen_movi_i32(cpu_tile_iot_src0, a->src0 & 0x1f);
    tcg_gen_movi_i32(cpu_tile_iot_src1, a->src1 & 0x1f);
    tcg_gen_movi_i32(cpu_tile_iot_reg, a->reg & 0x1f);

    const uint64_t desc =
        ((uint64_t)(a->src0 & 0x1f) << 0) |
        ((uint64_t)(a->src1 & 0x1f) << 5) |
        ((uint64_t)(a->dst & 0x7) << 10) |
        ((uint64_t)(a->grp & 0x1) << 13) |
        ((uint64_t)(flags & 0xf) << 14) |
        ((uint64_t)(a->reg & 0x1f) << 18) |
        ((uint64_t)0 << 23) |
        ((uint64_t)0 << 28); /* has_size=0 */
    gen_helper_linx_tile_append_iot(tcg_env, tcg_constant_i64(desc));
    return true;
}

static bool trans_b_ioti(DisasContext *ctx, arg_b_ioti *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }

    uint32_t flags = 0;
    if (a->s0v) {
        flags |= 1u << 0;
    }
    if (a->s1v) {
        flags |= 1u << 1;
    }
    if (a->s0r) {
        flags |= 1u << 2;
    }
    if (a->s1r) {
        flags |= 1u << 3;
    }

    tcg_gen_movi_i32(cpu_tile_iot_valid, 1);
    tcg_gen_movi_i32(cpu_tile_iot_flags, flags);
    tcg_gen_movi_i32(cpu_tile_iot_dst, a->dst & 0x7);
    tcg_gen_movi_i32(cpu_tile_iot_grp, a->grp & 0x1);
    tcg_gen_movi_i32(cpu_tile_iot_src0, a->src0 & 0x1f);
    tcg_gen_movi_i32(cpu_tile_iot_src1, a->src1 & 0x1f);
    tcg_gen_movi_i32(cpu_tile_iot_size, a->size & 0x1f);

    const uint64_t desc =
        ((uint64_t)(a->src0 & 0x1f) << 0) |
        ((uint64_t)(a->src1 & 0x1f) << 5) |
        ((uint64_t)(a->dst & 0x7) << 10) |
        ((uint64_t)(a->grp & 0x1) << 13) |
        ((uint64_t)(flags & 0xf) << 14) |
        ((uint64_t)0 << 18) |
        ((uint64_t)(a->size & 0x1f) << 23) |
        ((uint64_t)1 << 28); /* has_size=1 */
    gen_helper_linx_tile_append_iot(tcg_env, tcg_constant_i64(desc));
    return true;
}

static bool trans_b_text(DisasContext *ctx, arg_b_text *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr body_tpc = linx_pcrel_target(pc, a->simm25);

    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }

    tcg_gen_movi_i64(cpu_body_tpc, body_tpc);
    return true;
}

static bool trans_b_ior(DisasContext *ctx, arg_b_ior *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    const uint64_t desc =
        ((uint64_t)(a->RegDst & 0x1f) << 0) |
        ((uint64_t)(a->SrcL & 0x1f) << 5) |
        ((uint64_t)(a->SrcR & 0x1f) << 10) |
        ((uint64_t)(a->SrcD & 0x1f) << 15);
    gen_helper_linx_tile_append_ior(tcg_env, tcg_constant_i64(desc));
    return true;
}

static bool trans_b_attr(DisasContext *ctx, arg_b_attr *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    const uint32_t packed =
        ((uint32_t)(a->c & 0x1u) << 0) |
        ((uint32_t)(a->dr & 0x1u) << 1) |
        ((uint32_t)(a->layout & 0x1fu) << 2) |
        ((uint32_t)(a->dtype & 0x1fu) << 7) |
        ((uint32_t)(a->pad & 0x1fu) << 12) |
        ((uint32_t)(a->t & 0x1u) << 17) |
        ((uint32_t)(a->aq & 0x1u) << 18) |
        ((uint32_t)(a->atom & 0x1u) << 19) |
        ((uint32_t)(a->far_ & 0x1u) << 20) |
        ((uint32_t)(a->rl & 0x1u) << 21);
    gen_helper_linx_tile_set_attr(tcg_env, tcg_constant_i32(packed));
    return true;
}

static bool trans_b_hint(DisasContext *ctx, arg_b_hint *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    (void)a;
    return true;
}

static bool trans_b_hint_trace(DisasContext *ctx, arg_b_hint_trace *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    (void)a;
    return true;
}

static bool trans_setret(DisasContext *ctx, arg_setret *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr tgt = pc + ((vaddr)a->imm20 << 1);
    linx_set_dest(LINX_REG_RA, tcg_constant_i64(tgt));
    ctx->ra_set = true;
    return true;
}

static bool trans_c_setc_tgt(DisasContext *ctx, arg_c_setc_tgt *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    ctx->tgt_modified = true;
    return true;
}

static bool trans_setc_tgt(DisasContext *ctx, arg_setc_tgt *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    ctx->tgt_modified = true;
    return true;
}

static bool trans_setc_cmp(DisasContext *ctx, TCGCond c, TCGv_i64 l, TCGv_i64 r);

static bool trans_setc_cmp_imm(DisasContext *ctx, TCGCond c, TCGv_i64 l, int64_t imm)
{
    TCGv_i64 r = tcg_temp_new_i64();
    tcg_gen_movi_i64(r, (uint64_t)imm);
    return trans_setc_cmp(ctx, c, l, r);
}

static bool trans_c_setc_eq(DisasContext *ctx, arg_c_setc_eq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    return trans_setc_cmp(ctx, TCG_COND_EQ, l, r);
}

static bool trans_c_setc_ne(DisasContext *ctx, arg_c_setc_ne *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    return trans_setc_cmp(ctx, TCG_COND_NE, l, r);
}

static bool trans_setc_eq(DisasContext *ctx, arg_setc_eq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_EQ, l, r);
}

static bool trans_setc_eqi(DisasContext *ctx, arg_setc_eqi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_EQ, l, simm);
}

static bool trans_setc_ne(DisasContext *ctx, arg_setc_ne *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_NE, l, r);
}

static bool trans_setc_nei(DisasContext *ctx, arg_setc_nei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, l, simm);
}

static bool trans_setc_and(DisasContext *ctx, arg_setc_and *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_andi(DisasContext *ctx, arg_setc_andi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    tcg_gen_andi_i64(out, l, simm);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_or(DisasContext *ctx, arg_setc_or *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_ori(DisasContext *ctx, arg_setc_ori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    tcg_gen_ori_i64(out, l, simm);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_cmp(DisasContext *ctx, TCGCond c, TCGv_i64 l, TCGv_i64 r)
{
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_DESC_OUTSIDE_BLOCK, 0);
    }
    /*
     * SETC updates block commit arguments and is frequently used to drive
     * BSTART COND and predicate-controlled commits. Lower it branchlessly so
     * common compare patterns don't force extra host control-flow.
     */
    TCGv_i64 cond64 = tcg_temp_new_i64();
    TCGv_i32 cond32 = tcg_temp_new_i32();
    tcg_gen_setcond_i64(c, cond64, l, r);
    tcg_gen_extrl_i64_i32(cond32, cond64);
    tcg_gen_mov_i32(cpu_cond, cond32);
    tcg_gen_mov_i32(cpu_carg, cond32);
    return true;
}

static bool trans_setc_lt(DisasContext *ctx, arg_setc_lt *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_LT, l, r);
}

static bool trans_setc_lti(DisasContext *ctx, arg_setc_lti *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_LT, l, simm);
}

static bool trans_setc_ltu(DisasContext *ctx, arg_setc_ltu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_LTU, l, r);
}

static bool trans_setc_ltui(DisasContext *ctx, arg_setc_ltui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    uint64_t uimm = (uint64_t)a->uimm12 << a->shamt;
    return trans_setc_cmp_imm(ctx, TCG_COND_LTU, l, (int64_t)uimm);
}

static bool trans_setc_ge(DisasContext *ctx, arg_setc_ge *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_GE, l, r);
}

static bool trans_setc_gei(DisasContext *ctx, arg_setc_gei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_GE, l, simm);
}

static bool trans_setc_geu(DisasContext *ctx, arg_setc_geu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_GEU, l, r);
}

static bool trans_setc_geui(DisasContext *ctx, arg_setc_geui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    uint64_t uimm = (uint64_t)a->uimm12 << a->shamt;
    return trans_setc_cmp_imm(ctx, TCG_COND_GEU, l, (int64_t)uimm);
}

static bool trans_alu_binop(DisasContext *ctx, unsigned dst, TCGv_i64 res)
{
    linx_set_dest(dst, res);
    return true;
}

static bool trans_add(DisasContext *ctx, arg_add *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_add_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_sub(DisasContext *ctx, arg_sub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sub_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_and(DisasContext *ctx, arg_and *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_or(DisasContext *ctx, arg_or *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_xor(DisasContext *ctx, arg_xor *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xor_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_addi(DisasContext *ctx, arg_addi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_subi(DisasContext *ctx, arg_subi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_andi(DisasContext *ctx, arg_andi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_ori(DisasContext *ctx, arg_ori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_xori(DisasContext *ctx, arg_xori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool linx_binop_w(DisasContext *ctx, unsigned dst, TCGv_i64 out32)
{
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(out, out32);
    linx_set_dest(dst, out);
    return true;
}

static bool trans_addw(DisasContext *ctx, arg_addw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_add_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_subw(DisasContext *ctx, arg_subw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sub_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_andw(DisasContext *ctx, arg_andw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_orw(DisasContext *ctx, arg_orw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_xorw(DisasContext *ctx, arg_xorw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xor_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_addiw(DisasContext *ctx, arg_addiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_subiw(DisasContext *ctx, arg_subiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_andiw(DisasContext *ctx, arg_andiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_oriw(DisasContext *ctx, arg_oriw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_xoriw(DisasContext *ctx, arg_xoriw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_mul(DisasContext *ctx, arg_mul *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_mul_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_madd(DisasContext *ctx, arg_madd *a)
{
    TCGv_i64 acc = linx_get_reg(a->SrcD);
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 prod = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    tcg_gen_mul_i64(prod, l, r);
    tcg_gen_add_i64(out, acc, prod);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_div_like(DisasContext *ctx, unsigned dst,
                           TCGv_i64 l, TCGv_i64 r, bool is_div, bool is_signed,
                           bool is_word)
{
    TCGv_i64 out = tcg_temp_new_i64();

    if (is_word) {
        TCGv_i64 l32 = tcg_temp_new_i64();
        TCGv_i64 r32 = tcg_temp_new_i64();
        tcg_gen_ext32s_i64(l32, l);
        tcg_gen_ext32s_i64(r32, r);
        if (!is_signed) {
            tcg_gen_ext32u_i64(l32, l);
            tcg_gen_ext32u_i64(r32, r);
        }
        if (is_div) {
            if (is_signed) {
                tcg_gen_div_i64(out, l32, r32);
            } else {
                tcg_gen_divu_i64(out, l32, r32);
            }
        } else {
            if (is_signed) {
                tcg_gen_rem_i64(out, l32, r32);
            } else {
                tcg_gen_remu_i64(out, l32, r32);
            }
        }
        return linx_binop_w(ctx, dst, out);
    }

    if (is_div) {
        if (is_signed) {
            tcg_gen_div_i64(out, l, r);
        } else {
            tcg_gen_divu_i64(out, l, r);
        }
    } else {
        if (is_signed) {
            tcg_gen_rem_i64(out, l, r);
        } else {
            tcg_gen_remu_i64(out, l, r);
        }
    }

    return trans_alu_binop(ctx, dst, out);
}

static bool trans_div(DisasContext *ctx, arg_div *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, true, false);
}

static bool trans_divu(DisasContext *ctx, arg_divu *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, false, false);
}

static bool trans_divw(DisasContext *ctx, arg_divw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, true, true);
}

static bool trans_divuw(DisasContext *ctx, arg_divuw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, false, true);
}

static bool trans_rem(DisasContext *ctx, arg_rem *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, true, false);
}

static bool trans_remu(DisasContext *ctx, arg_remu *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, false, false);
}

static bool trans_remw(DisasContext *ctx, arg_remw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, true, true);
}

static bool trans_remuw(DisasContext *ctx, arg_remuw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, false, true);
}

static bool trans_fabs(DisasContext *ctx, arg_fabs *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fabs(out, tcg_env, v, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fadd(DisasContext *ctx, arg_fadd *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fadd(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fsub(DisasContext *ctx, arg_fsub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fsub(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fmul(DisasContext *ctx, arg_fmul *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fmul(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fdiv(DisasContext *ctx, arg_fdiv *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fdiv(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_feq(DisasContext *ctx, arg_feq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_feq(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_flt(DisasContext *ctx, arg_flt *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_flt(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fge(DisasContext *ctx, arg_fge *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fge(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvt(DisasContext *ctx, arg_fcvt *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvt(out, tcg_env, v, tcg_constant_i32(a->DstType),
                         tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvtz(DisasContext *ctx, arg_fcvtz *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvtz(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_scvtf(DisasContext *ctx, arg_scvtf *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_scvtf(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_ucvtf(DisasContext *ctx, arg_ucvtf *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_ucvtf(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_mulw(DisasContext *ctx, arg_mulw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();

    tcg_gen_mul_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_maddw(DisasContext *ctx, arg_maddw *a)
{
    TCGv_i64 acc = linx_get_reg(a->SrcD);
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 prod = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    tcg_gen_mul_i64(prod, l, r);
    tcg_gen_add_i64(out, acc, prod);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_cmp_out(DisasContext *ctx, unsigned dst,
                          TCGCond c, TCGv_i64 l, TCGv_i64 r)
{
    TCGv_i64 out = tcg_temp_new_i64();
    TCGLabel *t = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_movi_i64(out, 0);
    tcg_gen_brcond_i64(c, l, r, t);
    tcg_gen_br(done);
    gen_set_label(t);
    tcg_gen_movi_i64(out, 1);
    gen_set_label(done);
    linx_set_dest(dst, out);
    return true;
}

static bool trans_cmp_eq(DisasContext *ctx, arg_cmp_eq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_EQ, l, r);
}

static bool trans_cmp_ne(DisasContext *ctx, arg_cmp_ne *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_NE, l, r);
}

static bool trans_cmp_and(DisasContext *ctx, arg_cmp_and *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_and_i64(tmp, l, r);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_NE, tmp, tcg_constant_i64(0));
}

static bool trans_cmp_or(DisasContext *ctx, arg_cmp_or *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_or_i64(tmp, l, r);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_NE, tmp, tcg_constant_i64(0));
}

static bool trans_cmp_lt(DisasContext *ctx, arg_cmp_lt *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_LT, l, r);
}

static bool trans_cmp_ltu(DisasContext *ctx, arg_cmp_ltu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_LTU, l, r);
}

static bool trans_cmp_ge(DisasContext *ctx, arg_cmp_ge *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_GE, l, r);
}

static bool trans_cmp_geu(DisasContext *ctx, arg_cmp_geu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_GEU, l, r);
}

static bool trans_csel(DisasContext *ctx, arg_csel *a)
{
    /* LinxISA csel: csel SrcP, SrcL, SrcR<.modifier>, ->{t, u, Rd}
     * Semantics: if SrcP != 0 (true), output = SrcR; else output = SrcL
     * The SrcRType modifier applies to SrcR (e.g., .sw = sign-extend word)
     */
    TCGv_i64 pred = linx_get_reg(a->SrcP);
    TCGv_i64 fval = linx_get_reg(a->SrcL);
    TCGv_i64 tval = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 out = tcg_temp_new_i64();
    TCGLabel *done = gen_new_label();

    tcg_gen_mov_i64(out, fval);               /* default to false case (SrcL) */
    tcg_gen_brcondi_i64(TCG_COND_EQ, pred, 0, done);  /* if pred == 0, done */
    tcg_gen_mov_i64(out, tval);               /* else use true case (SrcR) */
    gen_set_label(done);

    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_lui(DisasContext *ctx, arg_lui *a)
{
    int32_t imm = sextract32(a->imm20, 0, 20);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, (int64_t)imm << 12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_lui(DisasContext *ctx, arg_hl_lui *a)
{
    int32_t imm = (int32_t)a->imm;
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, (int64_t)imm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_sll(DisasContext *ctx, arg_sll *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(shamt, sh, 0x3f);
    tcg_gen_shl_i64(out, l, shamt);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_srl(DisasContext *ctx, arg_srl *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(shamt, sh, 0x3f);
    tcg_gen_shr_i64(out, l, shamt);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_sra(DisasContext *ctx, arg_sra *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(shamt, sh, 0x3f);
    tcg_gen_sar_i64(out, l, shamt);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_slli(DisasContext *ctx, arg_slli *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shli_i64(out, l, a->shamt & 0x3f);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_srli(DisasContext *ctx, arg_srli *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shri_i64(out, l, a->shamt & 0x3f);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_srai(DisasContext *ctx, arg_srai *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sari_i64(out, l, a->shamt & 0x3f);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_sllw(DisasContext *ctx, arg_sllw *a)
{
    /* Word shifts operate on the low 32 bits only, regardless of the upper
     * bits in the source register (which are often undefined for 32-bit C
     * values). Mask the shift amount to 0..31.
     */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_andi_i64(shamt, sh, 0x1f);
    tcg_gen_shl_i64(out, l32, shamt);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_srlw(DisasContext *ctx, arg_srlw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_andi_i64(shamt, sh, 0x1f);
    tcg_gen_shr_i64(out, l32, shamt);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_sraw(DisasContext *ctx, arg_sraw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(l32, l);
    tcg_gen_andi_i64(shamt, sh, 0x1f);
    tcg_gen_sar_i64(out, l32, shamt);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_slliw(DisasContext *ctx, arg_slliw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_shli_i64(out, l32, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_srliw(DisasContext *ctx, arg_srliw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_shri_i64(out, l32, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_sraiw(DisasContext *ctx, arg_sraiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(l32, l);
    tcg_gen_sari_i64(out, l32, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_c_movr(DisasContext *ctx, arg_c_movr *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    linx_set_dest(a->RegDst, v);
    return true;
}

static bool trans_c_movi(DisasContext *ctx, arg_c_movi *a)
{
    /* C.SETRET is a special case of C.MOVI when RegDst == RA */
    if (a->RegDst == LINX_REG_RA) {
        /* C.SETRET: RA = PC + (uimm5 << 1)
         * Note: simm5 field is reinterpreted as uimm5 for SETRET */
        vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
        vaddr tgt = pc + ((vaddr)(a->simm5 & 0x1F) << 1);
        TCGv_i64 tgt_v = tcg_constant_i64(tgt);
        if (linx_trace_ra_enabled && linx_trace_ra_pc_match(pc)) {
            TCGv_i64 old = tcg_temp_new_i64();
            tcg_gen_mov_i64(old, cpu_gpr[LINX_REG_RA]);
            gen_helper_linx_trace_ra(tcg_env, tcg_constant_i64(pc),
                                     tcg_constant_i32(1), old,
                                     tgt_v);
        }
        linx_set_dest(LINX_REG_RA, tgt_v);
        ctx->ra_set = true;
        return true;
    }

    /* Normal C.MOVI */
    TCGv_i64 v = tcg_temp_new_i64();
    tcg_gen_movi_i64(v, (int64_t)a->simm5);
    linx_set_dest(a->RegDst, v);
    return true;
}

static bool trans_c_addi(DisasContext *ctx, arg_c_addi *a)
{
    /* C.ADDI: SrcL + simm5 -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (int64_t)a->simm5);
    linx_push_t(out);
    return true;
}

static bool trans_c_add(DisasContext *ctx, arg_c_add *a)
{
    /* C.ADD: SrcL + SrcR -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_add_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static bool trans_c_sub(DisasContext *ctx, arg_c_sub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sub_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static bool trans_c_and(DisasContext *ctx, arg_c_and *a)
{
    /* C.AND: SrcL & SrcR -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static bool trans_c_or(DisasContext *ctx, arg_c_or *a)
{
    /* C.OR: SrcL | SrcR -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static TCGv linx_addr_from_i64(TCGv_i64 a64)
{
#if TARGET_LONG_BITS == 32
    TCGv a = tcg_temp_new();
    tcg_gen_trunc_i64_tl(a, a64);
    return a;
#else
    return a64;
#endif
}

static TCGv_i64 linx_addr_add_imm(DisasContext *ctx, unsigned base, int64_t off)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 b = linx_get_reg(base);
    tcg_gen_addi_i64(addr, b, off);
    return addr;
}

static TCGv_i64 linx_addr_add_reg(DisasContext *ctx, unsigned base,
                                  unsigned idx, unsigned idx_type,
                                  unsigned shamt)
{
    TCGv_i64 b = linx_get_reg(base);
    TCGv_i64 i = linx_get_reg(idx);
    TCGv_i64 t = tcg_temp_new_i64();
    TCGv_i64 addr = tcg_temp_new_i64();

    switch (idx_type & 0x3) {
    case 0: /* .sw */
        tcg_gen_ext32s_i64(t, i);
        break;
    case 1: /* .uw */
        tcg_gen_ext32u_i64(t, i);
        break;
    case 2: /* .neg */
        tcg_gen_neg_i64(t, i);
        break;
    default: /* raw 64-bit register */
        tcg_gen_mov_i64(t, i);
        break;
    }
    if (shamt) {
        tcg_gen_shli_i64(t, t, shamt & 0x3f);
    }
    tcg_gen_add_i64(addr, b, t);
    return addr;
}

static bool linx_load_to_dest(DisasContext *ctx, unsigned dst, TCGv addr,
                              MemOp mop)
{
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    TCGv_i64 out = tcg_temp_new_i64();

    {
        const unsigned size = memop_size(mop);
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        gen_helper_linx_dbg_check_load(tcg_env, tcg_constant_i64(pc), addr64,
                                       tcg_constant_i32((int32_t)size));
    }
    tcg_gen_qemu_ld_i64(out, addr, 0, mop | linx_mo_endian());

    if (linx_commit_trace_enabled) {
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        tcg_gen_movi_i32(cpu_trace_mem_valid, 1);
        tcg_gen_movi_i32(cpu_trace_mem_is_store, 0);
        tcg_gen_mov_i64(cpu_trace_mem_addr, addr64);
        tcg_gen_movi_i64(cpu_trace_mem_wdata, 0);
        tcg_gen_mov_i64(cpu_trace_mem_rdata, out);
        tcg_gen_movi_i32(cpu_trace_mem_size, (int32_t)memop_size(mop));
    }

    if (linx_watch_load_enabled) {
        if (!linx_watch_load_pc_filter_enabled ||
            ((uint64_t)pc >= linx_watch_load_pc_lo &&
             (uint64_t)pc <= linx_watch_load_pc_hi)) {
            const unsigned size = memop_size(mop);
            TCGLabel *skip = gen_new_label();
            TCGv_i64 addr64;
            TCGv_i64 end64;

#if TARGET_LONG_BITS == 32
            addr64 = tcg_temp_new_i64();
            tcg_gen_extu_tl_i64(addr64, addr);
#else
            addr64 = (TCGv_i64)addr;
#endif

            tcg_gen_brcondi_i64(TCG_COND_GTU, addr64,
                                (int64_t)linx_watch_load_hi, skip);
            if (size > 1) {
                end64 = tcg_temp_new_i64();
                tcg_gen_addi_i64(end64, addr64, (int64_t)size - 1);
                tcg_gen_brcondi_i64(TCG_COND_LTU, end64,
                                    (int64_t)linx_watch_load_lo, skip);
            } else {
                tcg_gen_brcondi_i64(TCG_COND_LTU, addr64,
                                    (int64_t)linx_watch_load_lo, skip);
            }

            gen_helper_linx_watch_load(tcg_env, tcg_constant_i64(pc), addr64,
                                       out,
                                       tcg_constant_i32((int32_t)size));
            gen_set_label(skip);
        }
    }

    linx_set_dest(dst, out);
    return true;
}

static bool trans_lbi(DisasContext *ctx, arg_lbi *a)
{
    int64_t off = (int64_t)a->simm12;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_lbui(DisasContext *ctx, arg_lbui *a)
{
    int64_t off = (int64_t)a->simm12;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_lhi(DisasContext *ctx, arg_lhi *a)
{
    int64_t off = (int64_t)a->simm12 * 2;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lhui(DisasContext *ctx, arg_lhui *a)
{
    int64_t off = (int64_t)a->simm12 * 2;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_lwi(DisasContext *ctx, arg_lwi *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_lb_pcr(DisasContext *ctx, arg_lb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_lbu_pcr(DisasContext *ctx, arg_lbu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_lh_pcr(DisasContext *ctx, arg_lh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lhu_pcr(DisasContext *ctx, arg_lhu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_lw_pcr(DisasContext *ctx, arg_lw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_lwu_pcr(DisasContext *ctx, arg_lwu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_ld_pcr(DisasContext *ctx, arg_ld_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_hl_lb_pcr(DisasContext *ctx, arg_hl_lb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_hl_lbu_pcr(DisasContext *ctx, arg_hl_lbu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_hl_lh_pcr(DisasContext *ctx, arg_hl_lh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_hl_lhu_pcr(DisasContext *ctx, arg_hl_lhu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_hl_lw_pcr(DisasContext *ctx, arg_hl_lw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_hl_lwu_pcr(DisasContext *ctx, arg_hl_lwu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_hl_ld_pcr(DisasContext *ctx, arg_hl_ld_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_lwui(DisasContext *ctx, arg_lwui *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_ldi(DisasContext *ctx, arg_ldi *a)
{
    int64_t off = (int64_t)a->simm12 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_lb(DisasContext *ctx, arg_lb *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_lbu(DisasContext *ctx, arg_lbu *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_lh(DisasContext *ctx, arg_lh *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lhu(DisasContext *ctx, arg_lhu *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_lw(DisasContext *ctx, arg_lw *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_lwu(DisasContext *ctx, arg_lwu *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_ld(DisasContext *ctx, arg_ld *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool linx_store_from_reg(DisasContext *ctx, TCGv addr, TCGv_i64 val,
                                MemOp mop)
{
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    {
        const unsigned size = memop_size(mop);
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        gen_helper_linx_dbg_check_store(tcg_env, tcg_constant_i64(pc), addr64,
                                        tcg_constant_i32((int32_t)size));
    }

    if (linx_watch_store_enabled) {
        if (!linx_watch_store_pc_filter_enabled ||
            ((uint64_t)pc >= linx_watch_store_pc_lo &&
             (uint64_t)pc <= linx_watch_store_pc_hi)) {
            const unsigned size = memop_size(mop);
            TCGLabel *skip = gen_new_label();
            TCGv_i64 addr64;
            TCGv_i64 end64;

#if TARGET_LONG_BITS == 32
            addr64 = tcg_temp_new_i64();
            tcg_gen_extu_tl_i64(addr64, addr);
#else
            addr64 = (TCGv_i64)addr;
#endif

            tcg_gen_brcondi_i64(TCG_COND_GTU, addr64, (int64_t)linx_watch_store_hi, skip);
            if (size > 1) {
                end64 = tcg_temp_new_i64();
                tcg_gen_addi_i64(end64, addr64, (int64_t)size - 1);
                tcg_gen_brcondi_i64(TCG_COND_LTU, end64,
                                    (int64_t)linx_watch_store_lo, skip);
            } else {
                tcg_gen_brcondi_i64(TCG_COND_LTU, addr64,
                                    (int64_t)linx_watch_store_lo, skip);
            }

            gen_helper_linx_watch_store(tcg_env, tcg_constant_i64(pc), addr64, val,
                                        tcg_constant_i32((int32_t)size));
            gen_set_label(skip);
        }
    }

    if (linx_commit_trace_enabled) {
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        tcg_gen_movi_i32(cpu_trace_mem_valid, 1);
        tcg_gen_movi_i32(cpu_trace_mem_is_store, 1);
        tcg_gen_mov_i64(cpu_trace_mem_addr, addr64);
        tcg_gen_mov_i64(cpu_trace_mem_wdata, val);
        tcg_gen_movi_i64(cpu_trace_mem_rdata, 0);
        tcg_gen_movi_i32(cpu_trace_mem_size, (int32_t)memop_size(mop));
    }

    linx_lr_invalidate();
    tcg_gen_qemu_st_i64(val, addr, 0, mop | linx_mo_endian());
    return true;
}

static bool trans_sbi(DisasContext *ctx, arg_sbi *a)
{
    int64_t off = (int64_t)a->simm12;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UB);
}

static bool trans_shi(DisasContext *ctx, arg_shi *a)
{
    int64_t off = (int64_t)a->simm12 * 2;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_swi(DisasContext *ctx, arg_swi *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UL);
}

static inline int32_t linx_decode_pcr17_store_imm(uint32_t enc_imm)
{
    /*
     * lld encodePcr17Store packs simm17 as:
     *   simm[11:0]  -> insn[31:20]
     *   simm[16:12] -> insn[11:7]
     *
     * decode-insn32 currently exposes these bits in the opposite concatenation
     * order for arg_*_pcr::imm, so remap before sign-extension.
     */
    uint32_t uimm = ((enc_imm & 0x1fu) << 12) | ((enc_imm >> 5) & 0x0fffu);
    return (int32_t)(uimm << 15) >> 15;
}

static bool trans_sb_pcr(DisasContext *ctx, arg_sb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UB);
}

static bool trans_sh_pcr(DisasContext *ctx, arg_sh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_sw_pcr(DisasContext *ctx, arg_sw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UL);
}

static bool trans_sd_pcr(DisasContext *ctx, arg_sd_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UQ);
}

static bool trans_hl_sb_pcr(DisasContext *ctx, arg_hl_sb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UB);
}

static bool trans_hl_sh_pcr(DisasContext *ctx, arg_hl_sh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_hl_sw_pcr(DisasContext *ctx, arg_hl_sw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UL);
}

static bool trans_hl_sd_pcr(DisasContext *ctx, arg_hl_sd_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UQ);
}

/* ===================== HL writeback + pair memory (48-bit) ===================== */

static bool linx_hl_load_wb_reg(DisasContext *ctx,
                                unsigned dst_val, unsigned dst_updated,
                                unsigned base, unsigned idx,
                                unsigned idx_type, unsigned shamt,
                                bool is_pre, MemOp mop)
{
    TCGv_i64 base64 = linx_get_reg(base);
    TCGv_i64 updated64 = linx_addr_add_reg(ctx, base, idx, idx_type, shamt);
    TCGv addr = linx_addr_from_i64(is_pre ? updated64 : base64);

    /* If the memory access traps, the updated writeback must not commit. */
    if (!linx_load_to_dest(ctx, dst_val, addr, mop)) {
        return false;
    }
    linx_set_dest(dst_updated, updated64);
    return true;
}

static bool linx_hl_load_wb_imm(DisasContext *ctx,
                                unsigned dst_val, unsigned dst_updated,
                                unsigned base, int64_t off,
                                bool is_pre, MemOp mop)
{
    TCGv_i64 base64 = linx_get_reg(base);
    TCGv_i64 updated64 = tcg_temp_new_i64();
    TCGv addr;

    tcg_gen_addi_i64(updated64, base64, off);
    addr = linx_addr_from_i64(is_pre ? updated64 : base64);

    if (!linx_load_to_dest(ctx, dst_val, addr, mop)) {
        return false;
    }
    linx_set_dest(dst_updated, updated64);
    return true;
}

static bool linx_hl_store_wb_reg(DisasContext *ctx,
                                 unsigned dst_updated, unsigned src_data,
                                 unsigned base, unsigned idx,
                                 unsigned idx_type, unsigned shamt,
                                 bool is_pre, MemOp mop)
{
    TCGv_i64 base64 = linx_get_reg(base);
    TCGv_i64 updated64 = linx_addr_add_reg(ctx, base, idx, idx_type, shamt);
    TCGv addr = linx_addr_from_i64(is_pre ? updated64 : base64);

    if (!linx_store_from_reg(ctx, addr, linx_get_reg(src_data), mop)) {
        return false;
    }
    linx_set_dest(dst_updated, updated64);
    return true;
}

static bool linx_hl_store_wb_imm(DisasContext *ctx,
                                 unsigned dst_updated, unsigned src_data,
                                 unsigned base, int64_t off,
                                 bool is_pre, MemOp mop)
{
    TCGv_i64 base64 = linx_get_reg(base);
    TCGv_i64 updated64 = tcg_temp_new_i64();
    TCGv addr;

    tcg_gen_addi_i64(updated64, base64, off);
    addr = linx_addr_from_i64(is_pre ? updated64 : base64);

    if (!linx_store_from_reg(ctx, addr, linx_get_reg(src_data), mop)) {
        return false;
    }
    linx_set_dest(dst_updated, updated64);
    return true;
}

static bool linx_hl_load_pair_reg(DisasContext *ctx,
                                  unsigned dst0, unsigned dst1,
                                  unsigned base, unsigned idx,
                                  unsigned idx_type, unsigned shamt,
                                  MemOp mop)
{
    const int64_t step = (int64_t)memop_size(mop);
    TCGv_i64 addr0 = linx_addr_add_reg(ctx, base, idx, idx_type, shamt);
    TCGv_i64 addr1 = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr1, addr0, step);

    if (!linx_load_to_dest(ctx, dst0, linx_addr_from_i64(addr0), mop)) {
        return false;
    }
    return linx_load_to_dest(ctx, dst1, linx_addr_from_i64(addr1), mop);
}

static bool linx_hl_load_pair_imm(DisasContext *ctx,
                                  unsigned dst0, unsigned dst1,
                                  unsigned base, int64_t off,
                                  MemOp mop)
{
    const int64_t step = (int64_t)memop_size(mop);
    TCGv_i64 addr0 = linx_addr_add_imm(ctx, base, off);
    TCGv_i64 addr1 = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr1, addr0, step);

    if (!linx_load_to_dest(ctx, dst0, linx_addr_from_i64(addr0), mop)) {
        return false;
    }
    return linx_load_to_dest(ctx, dst1, linx_addr_from_i64(addr1), mop);
}

static bool linx_hl_store_pair_reg(DisasContext *ctx,
                                   unsigned src0, unsigned src1,
                                   unsigned base, unsigned idx,
                                   unsigned idx_type, unsigned shamt,
                                   MemOp mop)
{
    const int64_t step = (int64_t)memop_size(mop);
    TCGv_i64 addr0 = linx_addr_add_reg(ctx, base, idx, idx_type, shamt);
    TCGv_i64 addr1 = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr1, addr0, step);

    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr0), linx_get_reg(src0), mop)) {
        return false;
    }
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr1), linx_get_reg(src1), mop);
}

static bool linx_hl_store_pair_imm(DisasContext *ctx,
                                   unsigned src0, unsigned src1,
                                   unsigned base, int64_t off,
                                   MemOp mop)
{
    const int64_t step = (int64_t)memop_size(mop);
    TCGv_i64 addr0 = linx_addr_add_imm(ctx, base, off);
    TCGv_i64 addr1 = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr1, addr0, step);

    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr0), linx_get_reg(src0), mop)) {
        return false;
    }
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr1), linx_get_reg(src1), mop);
}

#define DEF_HL_LOAD_WB_REG(NAME, MOP, IS_PRE) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        return linx_hl_load_wb_reg(ctx, a->RegDst0, a->RegDst1, \
                                   a->SrcL, a->SrcR, a->SrcRType, a->shamt, \
                                   (IS_PRE), (MOP)); \
    }

#define DEF_HL_LOAD_WB_IMM(NAME, MOP, SCALE, IS_PRE) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        const int64_t off = (int64_t)a->simm17 * (int64_t)(SCALE); \
        return linx_hl_load_wb_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL, off, \
                                   (IS_PRE), (MOP)); \
    }

#define DEF_HL_STORE_WB_REG(NAME, MOP, SHIFT, IS_PRE) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        return linx_hl_store_wb_reg(ctx, a->RegDst, a->SrcD, \
                                    a->SrcL, a->SrcR, a->SrcRType, (SHIFT), \
                                    (IS_PRE), (MOP)); \
    }

#define DEF_HL_STORE_WB_IMM(NAME, MOP, SCALE, IS_PRE) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        const int64_t off = (int64_t)a->simm17 * (int64_t)(SCALE); \
        return linx_hl_store_wb_imm(ctx, a->RegDst, a->SrcD, a->SrcR, off, \
                                    (IS_PRE), (MOP)); \
    }

#define DEF_HL_LOAD_PAIR_REG(NAME, MOP) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        return linx_hl_load_pair_reg(ctx, a->RegDst0, a->RegDst1, \
                                     a->SrcL, a->SrcR, a->SrcRType, a->shamt, (MOP)); \
    }

#define DEF_HL_LOAD_PAIR_IMM(NAME, MOP, SCALE) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        const int64_t off = (int64_t)a->simm17 * (int64_t)(SCALE); \
        return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL, off, (MOP)); \
    }

#define DEF_HL_STORE_PAIR_REG(NAME, MOP, SHIFT) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        return linx_hl_store_pair_reg(ctx, a->SrcD, a->SrcD1, \
                                      a->SrcL, a->SrcR, a->SrcRType, (SHIFT), (MOP)); \
    }

#define DEF_HL_STORE_PAIR_IMM(NAME, MOP, SCALE) \
    static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
    { \
        const int64_t off = (int64_t)a->simm17 * (int64_t)(SCALE); \
        return linx_hl_store_pair_imm(ctx, a->SrcD, a->SrcD1, a->SrcR, off, (MOP)); \
    }

/* HL load writeback: post-index. */
DEF_HL_LOAD_WB_REG(hl_lb_po, MO_SB, false)
DEF_HL_LOAD_WB_IMM(hl_lbi_po, MO_SB, 1, false)
DEF_HL_LOAD_WB_REG(hl_lbu_po, MO_UB, false)
DEF_HL_LOAD_WB_IMM(hl_lbui_po, MO_UB, 1, false)
DEF_HL_LOAD_WB_REG(hl_lh_po, MO_SW, false)
DEF_HL_LOAD_WB_IMM(hl_lhi_po, MO_SW, 2, false)
DEF_HL_LOAD_WB_IMM(hl_lhi_upo, MO_SW, 1, false)
DEF_HL_LOAD_WB_REG(hl_lhu_po, MO_UW, false)
DEF_HL_LOAD_WB_IMM(hl_lhui_po, MO_UW, 2, false)
DEF_HL_LOAD_WB_IMM(hl_lhui_upo, MO_UW, 1, false)
DEF_HL_LOAD_WB_REG(hl_lw_po, MO_SL, false)
DEF_HL_LOAD_WB_IMM(hl_lwi_po, MO_SL, 4, false)
DEF_HL_LOAD_WB_IMM(hl_lwi_upo, MO_SL, 1, false)
DEF_HL_LOAD_WB_REG(hl_lwu_po, MO_UL, false)
DEF_HL_LOAD_WB_IMM(hl_lwui_po, MO_UL, 4, false)
DEF_HL_LOAD_WB_IMM(hl_lwui_upo, MO_UL, 1, false)
DEF_HL_LOAD_WB_REG(hl_ld_po, MO_UQ, false)
DEF_HL_LOAD_WB_IMM(hl_ldi_po, MO_UQ, 8, false)
DEF_HL_LOAD_WB_IMM(hl_ldi_upo, MO_UQ, 1, false)

/* HL load writeback: pre-index. */
DEF_HL_LOAD_WB_REG(hl_lb_pr, MO_SB, true)
DEF_HL_LOAD_WB_IMM(hl_lbi_pr, MO_SB, 1, true)
DEF_HL_LOAD_WB_REG(hl_lbu_pr, MO_UB, true)
DEF_HL_LOAD_WB_IMM(hl_lbui_pr, MO_UB, 1, true)
DEF_HL_LOAD_WB_REG(hl_lh_pr, MO_SW, true)
DEF_HL_LOAD_WB_IMM(hl_lhi_pr, MO_SW, 2, true)
DEF_HL_LOAD_WB_IMM(hl_lhi_upr, MO_SW, 1, true)
DEF_HL_LOAD_WB_REG(hl_lhu_pr, MO_UW, true)
DEF_HL_LOAD_WB_IMM(hl_lhui_pr, MO_UW, 2, true)
DEF_HL_LOAD_WB_IMM(hl_lhui_upr, MO_UW, 1, true)
DEF_HL_LOAD_WB_REG(hl_lw_pr, MO_SL, true)
DEF_HL_LOAD_WB_IMM(hl_lwi_pr, MO_SL, 4, true)
DEF_HL_LOAD_WB_IMM(hl_lwi_upr, MO_SL, 1, true)
DEF_HL_LOAD_WB_REG(hl_lwu_pr, MO_UL, true)
DEF_HL_LOAD_WB_IMM(hl_lwui_pr, MO_UL, 4, true)
DEF_HL_LOAD_WB_IMM(hl_lwui_upr, MO_UL, 1, true)
DEF_HL_LOAD_WB_REG(hl_ld_pr, MO_UQ, true)
DEF_HL_LOAD_WB_IMM(hl_ldi_pr, MO_UQ, 8, true)
DEF_HL_LOAD_WB_IMM(hl_ldi_upr, MO_UQ, 1, true)

/* HL store writeback: post-index. */
DEF_HL_STORE_WB_REG(hl_sb_po, MO_UB, 0, false)
DEF_HL_STORE_WB_IMM(hl_sbi_po, MO_UB, 1, false)
DEF_HL_STORE_WB_REG(hl_sh_po, MO_UW, 1, false)
DEF_HL_STORE_WB_REG(hl_sh_upo, MO_UW, 0, false)
DEF_HL_STORE_WB_IMM(hl_shi_po, MO_UW, 2, false)
DEF_HL_STORE_WB_IMM(hl_shi_upo, MO_UW, 1, false)
DEF_HL_STORE_WB_REG(hl_sw_po, MO_UL, 2, false)
DEF_HL_STORE_WB_REG(hl_sw_upo, MO_UL, 0, false)
DEF_HL_STORE_WB_IMM(hl_swi_po, MO_UL, 4, false)
DEF_HL_STORE_WB_IMM(hl_swi_upo, MO_UL, 1, false)
DEF_HL_STORE_WB_REG(hl_sd_po, MO_UQ, 3, false)
DEF_HL_STORE_WB_REG(hl_sd_upo, MO_UQ, 0, false)
DEF_HL_STORE_WB_IMM(hl_sdi_po, MO_UQ, 8, false)
DEF_HL_STORE_WB_IMM(hl_sdi_upo, MO_UQ, 1, false)

/* HL store writeback: pre-index. */
DEF_HL_STORE_WB_REG(hl_sb_pr, MO_UB, 0, true)
DEF_HL_STORE_WB_IMM(hl_sbi_pr, MO_UB, 1, true)
DEF_HL_STORE_WB_REG(hl_sh_pr, MO_UW, 1, true)
DEF_HL_STORE_WB_REG(hl_sh_upr, MO_UW, 0, true)
DEF_HL_STORE_WB_IMM(hl_shi_pr, MO_UW, 2, true)
DEF_HL_STORE_WB_IMM(hl_shi_upr, MO_UW, 1, true)
DEF_HL_STORE_WB_REG(hl_sw_pr, MO_UL, 2, true)
DEF_HL_STORE_WB_REG(hl_sw_upr, MO_UL, 0, true)
DEF_HL_STORE_WB_IMM(hl_swi_pr, MO_UL, 4, true)
DEF_HL_STORE_WB_IMM(hl_swi_upr, MO_UL, 1, true)
DEF_HL_STORE_WB_REG(hl_sd_pr, MO_UQ, 3, true)
DEF_HL_STORE_WB_REG(hl_sd_upr, MO_UQ, 0, true)
DEF_HL_STORE_WB_IMM(hl_sdi_pr, MO_UQ, 8, true)
DEF_HL_STORE_WB_IMM(hl_sdi_upr, MO_UQ, 1, true)

/* HL Load Pair. */
DEF_HL_LOAD_PAIR_IMM(hl_lbip, MO_SB, 1)
DEF_HL_LOAD_PAIR_REG(hl_lbp, MO_SB)
DEF_HL_LOAD_PAIR_IMM(hl_lbuip, MO_UB, 1)
DEF_HL_LOAD_PAIR_REG(hl_lbup, MO_UB)
DEF_HL_LOAD_PAIR_IMM(hl_lhip, MO_SW, 2)
DEF_HL_LOAD_PAIR_IMM(hl_lhip_u, MO_SW, 1)
DEF_HL_LOAD_PAIR_REG(hl_lhp, MO_SW)
DEF_HL_LOAD_PAIR_IMM(hl_lhuip, MO_UW, 2)
DEF_HL_LOAD_PAIR_IMM(hl_lhuip_u, MO_UW, 1)
DEF_HL_LOAD_PAIR_REG(hl_lhup, MO_UW)
DEF_HL_LOAD_PAIR_IMM(hl_lwip, MO_SL, 4)
DEF_HL_LOAD_PAIR_IMM(hl_lwip_u, MO_SL, 1)
DEF_HL_LOAD_PAIR_REG(hl_lwp, MO_SL)
DEF_HL_LOAD_PAIR_IMM(hl_lwuip, MO_UL, 4)
DEF_HL_LOAD_PAIR_IMM(hl_lwuip_u, MO_UL, 1)
DEF_HL_LOAD_PAIR_REG(hl_lwup, MO_UL)
DEF_HL_LOAD_PAIR_IMM(hl_ldip, MO_UQ, 8)
DEF_HL_LOAD_PAIR_IMM(hl_ldip_u, MO_UQ, 1)
DEF_HL_LOAD_PAIR_REG(hl_ldp, MO_UQ)

/* HL Store Pair. */
DEF_HL_STORE_PAIR_IMM(hl_sbip, MO_UB, 1)
DEF_HL_STORE_PAIR_REG(hl_sbp, MO_UB, 0)
DEF_HL_STORE_PAIR_IMM(hl_ship, MO_UW, 2)
DEF_HL_STORE_PAIR_IMM(hl_ship_u, MO_UW, 1)
DEF_HL_STORE_PAIR_REG(hl_shp, MO_UW, 1)
DEF_HL_STORE_PAIR_REG(hl_shp_u, MO_UW, 0)
DEF_HL_STORE_PAIR_IMM(hl_swip, MO_UL, 4)
DEF_HL_STORE_PAIR_IMM(hl_swip_u, MO_UL, 1)
DEF_HL_STORE_PAIR_REG(hl_swp, MO_UL, 2)
DEF_HL_STORE_PAIR_REG(hl_swp_u, MO_UL, 0)
DEF_HL_STORE_PAIR_IMM(hl_sdip, MO_UQ, 8)
DEF_HL_STORE_PAIR_IMM(hl_sdip_u, MO_UQ, 1)
DEF_HL_STORE_PAIR_REG(hl_sdp, MO_UQ, 3)
DEF_HL_STORE_PAIR_REG(hl_sdp_u, MO_UQ, 0)

#undef DEF_HL_STORE_PAIR_IMM
#undef DEF_HL_STORE_PAIR_REG
#undef DEF_HL_LOAD_PAIR_IMM
#undef DEF_HL_LOAD_PAIR_REG
#undef DEF_HL_STORE_WB_IMM
#undef DEF_HL_STORE_WB_REG
#undef DEF_HL_LOAD_WB_IMM
#undef DEF_HL_LOAD_WB_REG

static bool trans_sdi(DisasContext *ctx, arg_sdi *a)
{
    int64_t off = (int64_t)a->simm12 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UQ);
}

static bool trans_sb(DisasContext *ctx, arg_sb *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UB);
}

static bool trans_sh(DisasContext *ctx, arg_sh *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 1);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UW);
}

static bool trans_sw(DisasContext *ctx, arg_sw *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 2);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UL);
}

static bool trans_sd(DisasContext *ctx, arg_sd *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 3);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UQ);
}

static bool trans_sh_u(DisasContext *ctx, arg_sh_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UW);
}

static bool trans_sw_u(DisasContext *ctx, arg_sw_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UL);
}

static bool trans_sd_u(DisasContext *ctx, arg_sd_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UQ);
}

static bool trans_c_lwi(DisasContext *ctx, arg_c_lwi *a)
{
    int64_t off = (int64_t)a->simm5 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, 31, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_c_ldi(DisasContext *ctx, arg_c_ldi *a)
{
    int64_t off = (int64_t)a->simm5 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, 31, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_c_swi(DisasContext *ctx, arg_c_swi *a)
{
    int64_t off = (int64_t)a->simm5 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), cpu_tq[0], MO_UL);
}

static bool trans_c_sdi(DisasContext *ctx, arg_c_sdi *a)
{
    int64_t off = (int64_t)a->simm5 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), cpu_tq[0], MO_UQ);
}

/* ===================== v0.3 SIMT/Vector (64-bit) ===================== */

static bool linx_require_in_body(DisasContext *ctx)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    return true;
}

static bool trans_v_add(DisasContext *ctx, arg_v_add *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_add(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR),
                          tcg_constant_i32((int32_t)a->srctype),
                          tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_sub(DisasContext *ctx, arg_v_sub *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_sub(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR),
                          tcg_constant_i32((int32_t)a->srctype),
                          tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_mul(DisasContext *ctx, arg_v_mul *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_mul(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_eq(DisasContext *ctx, arg_v_cmp_eq *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_eq(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_ne(DisasContext *ctx, arg_v_cmp_ne *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ne(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_lt(DisasContext *ctx, arg_v_cmp_lt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_lt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_ltu(DisasContext *ctx, arg_v_cmp_ltu *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ltu(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_ge(DisasContext *ctx, arg_v_cmp_ge *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ge(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_geu(DisasContext *ctx, arg_v_cmp_geu *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_geu(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_feq(DisasContext *ctx, arg_v_feq *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_feq(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fne(DisasContext *ctx, arg_v_fne *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fne(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_flt(DisasContext *ctx, arg_v_flt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_flt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fge(DisasContext *ctx, arg_v_fge *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fge(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_csel(DisasContext *ctx, arg_v_csel *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_csel(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           /* Predicate lives in the SrcD field for V.CSEL. */
                           tcg_constant_i32((int32_t)a->SrcD),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR),
                           tcg_constant_i32((int32_t)a->srctype));
    return true;
}

static bool trans_v_fadd(DisasContext *ctx, arg_v_fadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fsub(DisasContext *ctx, arg_v_fsub *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fsub(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fmul(DisasContext *ctx, arg_v_fmul *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fmul(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fdiv(DisasContext *ctx, arg_v_fdiv *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fdiv(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fabs(DisasContext *ctx, arg_v_fabs *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fabs(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdadd(DisasContext *ctx, arg_v_rdadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdand(DisasContext *ctx, arg_v_rdand *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdand(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdfadd(DisasContext *ctx, arg_v_rdfadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdfadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdor(DisasContext *ctx, arg_v_rdor *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdor(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdxor(DisasContext *ctx, arg_v_rdxor *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdxor(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdmax(DisasContext *ctx, arg_v_rdmax *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdmax(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdmin(DisasContext *ctx, arg_v_rdmin *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdmin(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdfmax(DisasContext *ctx, arg_v_rdfmax *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdfmax(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdfmin(DisasContext *ctx, arg_v_rdfmin *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdfmin(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_lw(DisasContext *ctx, arg_v_lw *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_lw_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_lw_brg(DisasContext *ctx, arg_v_lw_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_lw_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_lw_brg(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_sw(DisasContext *ctx, arg_v_sw *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_sw_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_sw_brg(DisasContext *ctx, arg_v_sw_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_sw_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_sw_brg(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

/* ===================== PC-relative Instructions ===================== */

static bool trans_addtpc(DisasContext *ctx, arg_addtpc *a)
{
    /* ADDTPC: PC-relative page base
     * rd = (PC & ~0xFFF) + (sext(imm20) << 12) */
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr pc_page = current_pc & ~(vaddr)0xfff;
    int64_t imm = (int64_t)(int32_t)(a->imm20 << 12) >> 12; /* sign-extend 20-bit */
    imm <<= 12;
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, pc_page + imm);
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Function Entry/Exit Macro Instructions ===================== */

/*
 * LinxISA Function Prologue/Epilogue Instructions
 * 
 * These are hardware macro instructions that expand to register save/restore
 * sequences. The register range [Begin ~ End] specifies which registers to
 * save/restore (can wrap around from R23 to R2).
 * 
 * Stack size encoding:
 *   uimm[14:10] in instruction bits [11:7]
 *   uimm[9:3] in instruction bits [31:25]
 *   uimm[2:0] implicitly 0 (8-byte aligned)
 *   Actual stack size = (uimm_hi << 10) | (uimm_lo << 3)
 */

/* FENTRY: Function entry - save registers [Begin ~ End], adjust SP */
static bool trans_fentry(DisasContext *ctx, arg_fentry *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(0), /* FENTRY */
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->reg_begin),
                                  tcg_constant_i32(a->reg_end),
                                  tcg_constant_i64(stacksize));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* FEXIT: Function exit - restore registers, adjust SP (used with IND block for indirect return) */
static bool trans_fexit(DisasContext *ctx, arg_fexit *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(1), /* FEXIT */
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->reg_begin),
                                  tcg_constant_i32(a->reg_end),
                                  tcg_constant_i64(stacksize));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* FRET.RA: Function return via RA - restore registers, adjust SP, return to RA */
static bool trans_fret_ra(DisasContext *ctx, arg_fret_ra *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(2), /* FRET.RA */
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->reg_begin),
                                  tcg_constant_i32(a->reg_end),
                                  tcg_constant_i64(stacksize));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* FRET.STK: Function return via stack - restore registers, adjust SP, return */
static bool trans_fret_stk(DisasContext *ctx, arg_fret_stk *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(3), /* FRET.STK */
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->reg_begin),
                                  tcg_constant_i32(a->reg_end),
                                  tcg_constant_i64(stacksize));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* MCOPY: restartable bulk memory copy template (standalone block). */
static bool trans_mcopy(DisasContext *ctx, arg_mcopy *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_MCOPY),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* MSET: restartable bulk memory set template (standalone block). */
static bool trans_mset(DisasContext *ctx, arg_mset *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_MSET),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* ESAVE: restartable extended-state save template (standalone block). */
static bool trans_esave(DisasContext *ctx, arg_esave *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_ESAVE),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* ERCOV: restartable extended-state restore template (standalone block). */
static bool trans_ercov(DisasContext *ctx, arg_ercov *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_ERCOV),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* ===================== Min/Max Instructions ===================== */

static bool trans_max(DisasContext *ctx, arg_max *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_GE, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_maxu(DisasContext *ctx, arg_maxu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_GEU, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_min(DisasContext *ctx, arg_min *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_LE, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_minu(DisasContext *ctx, arg_minu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_LEU, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Bit Manipulation Instructions ===================== */

static bool trans_clz(DisasContext *ctx, arg_clz *a)
{
    /* CLZ: Count leading zeros in the N-bit field starting at bit M in SrcL.
     * Encoding notes: M=imms, N=imml+1 (ISA manual).
     */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 field = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (width == 0 || lsb >= 64 || lsb + width > 64) {
        tcg_gen_movi_i64(out, 0);
        linx_set_dest(a->RegDst, out);
        return true;
    }

    if (lsb) {
        tcg_gen_shri_i64(field, src, lsb);
    } else {
        tcg_gen_mov_i64(field, src);
    }

    if (width != 64) {
        uint64_t mask = (1ULL << width) - 1ULL;
        tcg_gen_andi_i64(field, field, mask);
    }

    tcg_gen_clzi_i64(out, field, width);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_ctz(DisasContext *ctx, arg_ctz *a)
{
    /* CTZ: Count trailing zeros in the N-bit field starting at bit M in SrcL.
     * Encoding notes: M=imms, N=imml+1 (ISA manual).
     */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 field = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (width == 0 || lsb >= 64 || lsb + width > 64) {
        tcg_gen_movi_i64(out, 0);
        linx_set_dest(a->RegDst, out);
        return true;
    }

    if (lsb) {
        tcg_gen_shri_i64(field, src, lsb);
    } else {
        tcg_gen_mov_i64(field, src);
    }

    if (width != 64) {
        uint64_t mask = (1ULL << width) - 1ULL;
        tcg_gen_andi_i64(field, field, mask);
    }

    tcg_gen_ctzi_i64(out, field, width);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bcnt(DisasContext *ctx, arg_bcnt *a)
{
    /* BCNT: Count set bits in the N-bit field starting at bit M in SrcL.
     * Encoding notes: M=imms, N=imml+1 (ISA manual).
     */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 field = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (width == 0 || lsb >= 64 || lsb + width > 64) {
        tcg_gen_movi_i64(out, 0);
        linx_set_dest(a->RegDst, out);
        return true;
    }

    if (lsb) {
        tcg_gen_shri_i64(field, src, lsb);
    } else {
        tcg_gen_mov_i64(field, src);
    }

    if (width != 64) {
        uint64_t mask = (1ULL << width) - 1ULL;
        tcg_gen_andi_i64(field, field, mask);
    }

    tcg_gen_ctpop_i64(out, field);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_rev(DisasContext *ctx, arg_rev *a)
{
    /* REV: Bit reversal */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Byte swap first, then reverse bits within each byte */
    tcg_gen_bswap64_i64(out, src);
    /* For full bit reversal, we need additional operations - simplified for now */
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Bit Extract/Insert Instructions ===================== */

static bool trans_bxs(DisasContext *ctx, arg_bxs *a)
{
    /* BXS: Bit extract signed - extract bit field and sign-extend */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_mov_i64(out, src);
        } else {
        tcg_gen_sextract_i64(out, src, lsb, width);
        }
    } else {
        tcg_gen_movi_i64(out, 0);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bxu(DisasContext *ctx, arg_bxu *a)
{
    /* BXU: Bit extract unsigned - extract bit field, zero-extend */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_mov_i64(out, src);
        } else {
            tcg_gen_extract_i64(out, src, lsb, width);
        }
    } else {
        tcg_gen_movi_i64(out, 0);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bic(DisasContext *ctx, arg_bic *a)
{
    /* BIC: Bit clear - clear bits in range [lsb, lsb+width) */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_movi_i64(out, 0);
        } else {
            uint64_t mask = ~(((1ULL << width) - 1) << lsb);
            tcg_gen_andi_i64(out, src, mask);
        }
    } else {
        tcg_gen_mov_i64(out, src);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bis(DisasContext *ctx, arg_bis *a)
{
    /* BIS: Bit set - set bits in range [lsb, lsb+width) */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_movi_i64(out, -1);
        } else {
            uint64_t mask = ((1ULL << width) - 1) << lsb;
            tcg_gen_ori_i64(out, src, mask);
        }
    } else {
        tcg_gen_mov_i64(out, src);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Compare Immediate Instructions ===================== */

static bool trans_cmp_eqi(DisasContext *ctx, arg_cmp_eqi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_EQ, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_nei(DisasContext *ctx, arg_cmp_nei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_NE, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_andi(DisasContext *ctx, arg_cmp_andi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(tmp, l, (int64_t)a->simm12);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_ori(DisasContext *ctx, arg_cmp_ori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(tmp, l, (int64_t)a->simm12);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_lti(DisasContext *ctx, arg_cmp_lti *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LT, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_gei(DisasContext *ctx, arg_cmp_gei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GE, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_ltui(DisasContext *ctx, arg_cmp_ltui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LTU, out, l, (uint64_t)a->uimm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_geui(DisasContext *ctx, arg_cmp_geui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GEU, out, l, (uint64_t)a->uimm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Branch on Zero/Non-Zero Instructions ===================== */

/* Internal relative control flow (used in decoupled bodies). */
static bool trans_b_eq(DisasContext *ctx, arg_b_eq *a)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm12 << 1);
    tcg_gen_brcond_i64(TCG_COND_EQ, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR), taken);
    tcg_gen_br(done);
    gen_set_label(taken);
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(done);
    return true;
}

static bool trans_b_ne(DisasContext *ctx, arg_b_ne *a)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm12 << 1);
    tcg_gen_brcond_i64(TCG_COND_NE, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR), taken);
    tcg_gen_br(done);
    gen_set_label(taken);
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(done);
    return true;
}

static bool trans_b_lt(DisasContext *ctx, arg_b_lt *a)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm12 << 1);
    tcg_gen_brcond_i64(TCG_COND_LT, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR), taken);
    tcg_gen_br(done);
    gen_set_label(taken);
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(done);
    return true;
}

static bool trans_b_ge(DisasContext *ctx, arg_b_ge *a)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm12 << 1);
    tcg_gen_brcond_i64(TCG_COND_GE, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR), taken);
    tcg_gen_br(done);
    gen_set_label(taken);
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(done);
    return true;
}

static bool trans_b_ltu(DisasContext *ctx, arg_b_ltu *a)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm12 << 1);
    tcg_gen_brcond_i64(TCG_COND_LTU, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR), taken);
    tcg_gen_br(done);
    gen_set_label(taken);
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(done);
    return true;
}

static bool trans_b_geu(DisasContext *ctx, arg_b_geu *a)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm12 << 1);
    tcg_gen_brcond_i64(TCG_COND_GEU, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR), taken);
    tcg_gen_br(done);
    gen_set_label(taken);
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    gen_set_label(done);
    return true;
}

static bool trans_j(DisasContext *ctx, arg_j *a)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_HEADER, 0);
    }
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm22 << 1);
    tcg_gen_movi_i64(cpu_pc, target);
    ctx->base.is_jmp = DISAS_NORETURN;
    tcg_gen_exit_tb(NULL, 0);
    return true;
}

static bool trans_b_z(DisasContext *ctx, arg_b_z *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    /* B.Z: Branch if condition flag is zero */
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm22 << 1);
    
    tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_cond, 0, taken);
    tcg_gen_br(done);
    
    gen_set_label(taken);
    gen_helper_linx_check_bstart_target(tcg_env, tcg_constant_i64(target));
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    
    gen_set_label(done);
    return true;
}

static bool trans_b_nz(DisasContext *ctx, arg_b_nz *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ILLEGAL_IN_BODY, 0);
    }
    /* B.NZ: Branch if condition flag is non-zero */
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm22 << 1);
    
    tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
    tcg_gen_br(done);
    
    gen_set_label(taken);
    gen_helper_linx_check_bstart_target(tcg_env, tcg_constant_i64(target));
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    
    gen_set_label(done);
    return true;
}

/* ===================== 16-bit Sign/Zero Extension ===================== */

static bool trans_c_sext_b(DisasContext *ctx, arg_c_sext_b *a)
{
    /* C.SEXT.B: Sign-extend byte to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext8s_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_sext_h(DisasContext *ctx, arg_c_sext_h *a)
{
    /* C.SEXT.H: Sign-extend halfword to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext16s_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_sext_w(DisasContext *ctx, arg_c_sext_w *a)
{
    /* C.SEXT.W: Sign-extend word to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_zext_b(DisasContext *ctx, arg_c_zext_b *a)
{
    /* C.ZEXT.B: Zero-extend byte to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext8u_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_zext_h(DisasContext *ctx, arg_c_zext_h *a)
{
    /* C.ZEXT.H: Zero-extend halfword to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext16u_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_zext_w(DisasContext *ctx, arg_c_zext_w *a)
{
    /* C.ZEXT.W: Zero-extend word to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

/* ===================== 16-bit Shift Instructions ===================== */

static bool trans_c_cmp_eqi(DisasContext *ctx, arg_c_cmp_eqi *a)
{
    /* C.CMP.EQI: (T-hand == imm) -> T-hand */
    TCGv_i64 src = cpu_tq[0];
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_EQ, out, src, (int64_t)a->simm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_cmp_nei(DisasContext *ctx, arg_c_cmp_nei *a)
{
    /* C.CMP.NEI: (T-hand != imm) -> T-hand */
    TCGv_i64 src = cpu_tq[0];
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_NE, out, src, (int64_t)a->simm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_slli(DisasContext *ctx, arg_c_slli *a)
{
    /* C.SLLI: Shift T-hand left by immediate, result to T-hand */
    TCGv_i64 src = cpu_tq[0];  /* T-hand as source */
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shli_i64(out, src, a->uimm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_srli(DisasContext *ctx, arg_c_srli *a)
{
    /* C.SRLI: Shift T-hand right logical by immediate, result to T-hand */
    TCGv_i64 src = cpu_tq[0];  /* T-hand as source */
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shri_i64(out, src, a->uimm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

/* ===================== 48-bit Instructions ===================== */

static bool trans_hl_addtpc(DisasContext *ctx, arg_hl_addtpc *a)
{
    /* HL.ADDTPC: PC-relative with 32-bit offset */
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr pc_page = current_pc & ~(vaddr)0xfff;
    int64_t offset = (int64_t)(int32_t)a->imm;
    offset <<= 12;
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, pc_page + offset);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_addi(DisasContext *ctx, arg_hl_addi *a)
{
    /* HL.ADDI: Add with 24-bit unsigned immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_addiw(DisasContext *ctx, arg_hl_addiw *a)
{
    /* HL.ADDIW: Add with 24-bit unsigned immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm24);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_subi(DisasContext *ctx, arg_hl_subi *a)
{
    /* HL.SUBI: Subtract with 24-bit unsigned immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_subiw(DisasContext *ctx, arg_hl_subiw *a)
{
    /* HL.SUBIW: Subtract with 24-bit unsigned immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm24);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_andi(DisasContext *ctx, arg_hl_andi *a)
{
    /* HL.ANDI: AND with 24-bit signed immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_andiw(DisasContext *ctx, arg_hl_andiw *a)
{
    /* HL.ANDIW: AND with 24-bit signed immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_ori(DisasContext *ctx, arg_hl_ori *a)
{
    /* HL.ORI: OR with 24-bit signed immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_oriw(DisasContext *ctx, arg_hl_oriw *a)
{
    /* HL.ORIW: OR with 24-bit signed immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_xori(DisasContext *ctx, arg_hl_xori *a)
{
    /* HL.XORI: XOR with 24-bit signed immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_xoriw(DisasContext *ctx, arg_hl_xoriw *a)
{
    /* HL.XORIW: XOR with 24-bit signed immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_cmp_eqi(DisasContext *ctx, arg_hl_cmp_eqi *a)
{
    /* HL.CMP.EQI: (SrcL == simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_EQ, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_nei(DisasContext *ctx, arg_hl_cmp_nei *a)
{
    /* HL.CMP.NEI: (SrcL != simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_NE, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_andi(DisasContext *ctx, arg_hl_cmp_andi *a)
{
    /* HL.CMP.ANDI: ((SrcL & simm24) != 0) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(tmp, l, (int64_t)a->simm);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_ori(DisasContext *ctx, arg_hl_cmp_ori *a)
{
    /* HL.CMP.ORI: ((SrcL | simm24) != 0) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(tmp, l, (int64_t)a->simm);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_lti(DisasContext *ctx, arg_hl_cmp_lti *a)
{
    /* HL.CMP.LTI: (SrcL < simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LT, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_gei(DisasContext *ctx, arg_hl_cmp_gei *a)
{
    /* HL.CMP.GEI: (SrcL >= simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GE, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_ltui(DisasContext *ctx, arg_hl_cmp_ltui *a)
{
    /* HL.CMP.LTUI: (SrcL < uimm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LTU, out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_geui(DisasContext *ctx, arg_hl_cmp_geui *a)
{
    /* HL.CMP.GEUI: (SrcL >= uimm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GEU, out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

/* 48-bit BSTART instructions with extended range */
static bool trans_hl_bstart_std_fall(DisasContext *ctx, arg_hl_bstart_std_fall *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    return true;
}

static bool trans_hl_bstart_std_direct(DisasContext *ctx, arg_hl_bstart_std_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    vaddr target = current_pc + ((int64_t)a->simm << 1);
    linx_block_begin(ctx, LINX_BR_DIRECT, target);
    return true;
}

static bool trans_hl_bstart_std_cond(DisasContext *ctx, arg_hl_bstart_std_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    vaddr target = current_pc + ((int64_t)a->simm << 1);
    linx_block_begin(ctx, LINX_BR_COND, target);
    return true;
}

static bool trans_hl_bstart_std_call(DisasContext *ctx, arg_hl_bstart_std_call *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    vaddr target = current_pc + ((int64_t)a->simm << 1);
    linx_block_begin(ctx, LINX_BR_CALL, target);
    return true;
}

/* ===================== System Instructions ===================== */

static bool trans_ssrget(DisasContext *ctx, arg_ssrget *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);

    gen_helper_linx_ssr_read(val, tcg_env, ssrid);
    linx_set_dest(a->RegDst, val);

    return true;
}

static bool trans_hl_ssrget(DisasContext *ctx, arg_hl_ssrget *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);

    gen_helper_linx_ssr_read(val, tcg_env, ssrid);
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_acrc(DisasContext *ctx, arg_acrc *a)
{
    /*
     * ACRC: request a synchronous system-call trap.
     *
     * v0.2 bring-up rule: ACRC MUST be followed by an explicit BSTOP/C.BSTOP in
     * the same block. The trap resume PC is the instruction after ACRC (bring-up:
     * the explicit terminator).
     */
    vaddr tpc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr bpc = ctx->base.pc_first;

    /* Enforce "ACRC followed by (C.)BSTOP" (bring-up). */
    {
        uint16_t hw_next = translator_lduw_end(ctx->env, &ctx->base, ctx->base.pc_next, MO_LE);
        if (hw_next != 0x0000) {
            return linx_block_fault(ctx, LINX_EBLOCK_CAUSE_ACRC_MISSING_BSTOP, ctx->base.pc_next);
        }
    }

    gen_helper_linx_service_request(
        tcg_env,
        tcg_constant_i32(a->rst),
        tcg_constant_i64(bpc),
        tcg_constant_i64(tpc),
        tcg_constant_i64(ctx->base.pc_next));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_acre(DisasContext *ctx, arg_acre *a)
{
    /* ACRE: request an ACR_ENTER (trap return / handoff) at block commit. */
    gen_helper_linx_acr_enter(tcg_env, tcg_constant_i32(a->rra));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_lr_w(DisasContext *ctx, arg_lr_w *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_lr_w(val, tcg_env, addr);
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_lr_d(DisasContext *ctx, arg_lr_d *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_lr_d(val, tcg_env, addr);
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_sc_w(DisasContext *ctx, arg_sc_w *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcR);
    TCGv_i64 val64 = linx_get_reg(a->SrcL);
    TCGv_i64 res = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, val64);
    gen_helper_linx_sc_w(res, tcg_env, addr, val32);
    linx_set_dest(a->RegDst, res);
    return true;
}

static bool trans_sc_d(DisasContext *ctx, arg_sc_d *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcR);
    TCGv_i64 val = linx_get_reg(a->SrcL);
    TCGv_i64 res = tcg_temp_new_i64();
    gen_helper_linx_sc_d(res, tcg_env, addr, val);
    linx_set_dest(a->RegDst, res);
    return true;
}

static bool trans_swapw(DisasContext *ctx, arg_swapw *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val64 = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, val64);
    gen_helper_linx_swapw(old, tcg_env, addr, val32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_swapd(DisasContext *ctx, arg_swapd *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    gen_helper_linx_swapd(old, tcg_env, addr, val);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_lw_add(DisasContext *ctx, arg_lw_add *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val64 = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, val64);
    gen_helper_linx_lw_add(old, tcg_env, addr, val32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_ld_add(DisasContext *ctx, arg_ld_add *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    gen_helper_linx_ld_add(old, tcg_env, addr, val);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_fence_d(DisasContext *ctx, arg_fence_d *a)
{
    (void)a;
    tcg_gen_mb(TCG_MO_ALL);
    return true;
}

static bool trans_fence_i(DisasContext *ctx, arg_fence_i *a)
{
    (void)a;
    tcg_gen_mb(TCG_MO_ALL);
    return true;
}

static bool trans_tlb_iall(DisasContext *ctx, arg_tlb_iall *a)
{
    (void)ctx;
    (void)a;
    gen_helper_linx_tlb_iall(tcg_env);
    return true;
}

static bool trans_ssrset(DisasContext *ctx, arg_ssrset *a)
{
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);

    gen_helper_linx_ssr_write(tcg_env, ssrid, src);
    return true;
}

static bool trans_hl_ssrset(DisasContext *ctx, arg_hl_ssrset *a)
{
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);
    gen_helper_linx_ssr_write(tcg_env, ssrid, src);
    return true;
}

static bool trans_ssrswap(DisasContext *ctx, arg_ssrswap *a)
{
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);
    TCGv_i64 old = tcg_temp_new_i64();

    gen_helper_linx_ssr_swap(old, tcg_env, ssrid, src);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_ebreak(DisasContext *ctx, arg_ebreak *a)
{
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    
    if (a->imm4 == 0) {
        /* Exit request - directly exit the TB without calling helper
         * This ensures QEMU terminates immediately */
        ctx->base.is_jmp = DISAS_NORETURN;
        /* Request shutdown before exiting */
        gen_helper_linx_exit(tcg_env);
        /* This never returns - the helper calls cpu_loop_exit */
        return true;
    }
    
    /* For other EBREAK operations (semihosting), call the helper */
    gen_helper_linx_ebreak(tcg_env, tcg_constant_i32(a->imm4));
    /*
     * Most semihosting operations return to the guest. Only exit/breakpoint
     * paths should terminate the TB.
     */
    if (a->imm4 != 1 && /* PUTCHAR */
        a->imm4 != 2 && /* WRITE */
        a->imm4 != 3 /* READ */) {
        ctx->base.is_jmp = DISAS_NORETURN;
    }
    return true;
}

static void linx_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULinxState *env = cpu_env(cpu);
    const vaddr pc = ctx->base.pc_first;

    ctx->env = env;
    ctx->brtype = (uint8_t)env->brtype;
    ctx->brtarget = 0;
    if (ctx->brtype == LINX_BR_COND ||
        ctx->brtype == LINX_BR_DIRECT ||
        ctx->brtype == LINX_BR_CALL) {
        /* Preserve fixed branch target when resuming from mid-block PCs. */
        ctx->brtarget = env->tgt;
    }
    ctx->cur_insn_len = 0;
    ctx->in_body = env->in_body != 0;
    ctx->decoupled_header = false;
    ctx->tgt_modified = false;
    ctx->ra_set = false;

    /*
     * Branches can legally target non-header instructions (LLVM emits this in
     * libc startup paths). When we enter such a target, stale block metadata
     * from the source block must not be re-applied at the next header boundary.
     */
    if ((ctx->brtype == LINX_BR_COND ||
         ctx->brtype == LINX_BR_DIRECT ||
         ctx->brtype == LINX_BR_CALL) &&
        env->tgt == pc &&
        !linx_is_bstart_at_pc(env, pc)) {
        ctx->brtype = LINX_BR_FALL;
        ctx->brtarget = 0;
    }
}

static void linx_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
}

static void linx_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next);
    tcg_gen_addi_i64(cpu_insn_count, cpu_insn_count, 1);
}

static unsigned linx_insn_len(uint16_t hw)
{
    if ((hw & 0x1) == 0) {
        return ((hw & 0xf) == 0xe) ? 6 : 2;
    }
    return ((hw & 0xf) == 0xf) ? 8 : 4;
}

static void linx_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULinxState *env = cpu_env(cpu);
    vaddr pc = ctx->base.pc_next;
    uint16_t hw;
    unsigned len;
    bool decoded = false;
    uint32_t insn_val = 0;

    /*
     * Arm co-sim exactly at instruction start so trigger snapshot captures
     * pre-execution architectural state for the trigger instruction.
     */
    gen_helper_linx_cosim_before_insn(tcg_env, tcg_constant_i64(pc));

    /* LinxISA is little-endian: bytes in memory like [00 08] should be read as 0x0800 */
    /* Use MO_LE to read instruction in little-endian format */
    hw = translator_lduw_end(env, &ctx->base, pc, MO_LE);
    
    len = linx_insn_len(hw);
    ctx->cur_insn_len = len;
    /* Always update pc_next to ensure tb->size is non-zero even if exception occurs */
    ctx->base.pc_next = pc + len;
    tcg_gen_movi_i64(cpu_insn_pc_next, ctx->base.pc_next);
    gen_helper_linx_dbg_check_pc(tcg_env, tcg_constant_i64(pc));

    switch (len) {
           case 2:
               insn_val = hw;
               linx_trace_begin(pc, (uint64_t)hw, len);
               /* Explicit check for c_bstop (0x0000) - decode tree should handle this but workaround for now */
               if (hw == 0x0000) {
                   union {
                       arg_decode_insn160 f_decode_insn160;
                   } u = { };
                   decoded = trans_c_bstop(ctx, &u.f_decode_insn160);
                   if (decoded) {
                       trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                   }
               } else if ((hw & 0xc7ff) == 0x0000) {
                   /* C.BSTART.STD: mask=0xc7ff, match=0x0000, BrType in bits [13:11] */
                   uint8_t brtype = (hw >> 11) & 0x7;
                   if (brtype != 0) {
                       /* Handle C.BSTART.STD with non-zero BrType */
                       decoded = trans_c_bstart_std(ctx, brtype);
                       if (decoded) {
                           trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                       }
                   } else {
                       /* BrType=0 is c_bstop */
                       union {
                           arg_decode_insn160 f_decode_insn160;
                       } u = { };
                       decoded = trans_c_bstop(ctx, &u.f_decode_insn160);
                       if (decoded) {
                           trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                       }
                   }
               } else if (hw == 0x88c0 || hw == 0xc8c0) {
                   /* C.BSTART.VPAR/VSEQ: fixed fall-through vector block headers. */
                   decoded = trans_c_bstart_std(ctx, LINX_BR_FALL);
                   if (decoded) {
                       trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                   }
               } else {
                   if (!linx_validate_opcode_meta(ctx, pc, (uint64_t)hw, len)) {
                       break;
                   }
                   decoded = decode_insn16(ctx, hw);
                   if (decoded) {
                       trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                   }
               }
               if (!decoded) {
                   qemu_log_mask(LOG_GUEST_ERROR, "Linx: decode failed @ PC=0x%" VADDR_PRIx
                                " hw=0x%04x len=%u\n", pc, hw, len);
                   linx_illegal(ctx);
               }
               break;
    case 4: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, MO_LE);
        insn_val = (uint32_t)hw | ((uint32_t)hw2 << 16);
        linx_trace_begin(pc, (uint64_t)insn_val, len);
        if (!linx_validate_opcode_meta(ctx, pc, (uint64_t)insn_val, len)) {
            break;
        }
        decoded = decode_insn32(ctx, insn_val);
        if (!decoded) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: decode32 failed @ PC=0x%" VADDR_PRIx " insn=0x%08x\n",
                          pc, insn_val);
            linx_illegal(ctx);
        } else {
            trace_linx_insn_exec(pc, insn_val, len, "32-bit");
        }
        break;
    }
    case 6: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, MO_LE);
        uint16_t hw3 = translator_lduw_end(env, &ctx->base, pc + 4, MO_LE);
        uint32_t hi = (uint32_t)hw2 | ((uint32_t)hw3 << 16);
        uint64_t insn48 = (uint64_t)hw | ((uint64_t)hi << 16);
        if (getenv("LINX_TRACE_DECODE48")) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: insn48 pc=0x%" VADDR_PRIx " insn48=0x%012" PRIx64
                          " key=0x%016" PRIx64 "\n",
                          pc, insn48, (uint64_t)(insn48 & 0xffff0000007f000full));
        }
        insn_val = (uint32_t)(insn48 & 0xFFFFFFFF);
        linx_trace_begin(pc, insn48, len);
        if (!linx_validate_opcode_meta(ctx, pc, insn48, len)) {
            break;
        }
        decoded = decode_insn48(ctx, insn48);
        if (!decoded) {
            if (getenv("LINX_TRACE_DECODE_FAIL")) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Linx: decode48 failed pc=0x%" VADDR_PRIx
                              " hw=0x%04x hw2=0x%04x hw3=0x%04x insn48=0x%012" PRIx64
                              " key=0x%016" PRIx64 "\n",
                              pc, hw, hw2, hw3, insn48,
                              (uint64_t)(insn48 & 0xffff0000007f000full));
            }
            linx_illegal(ctx);
        } else {
            trace_linx_insn_exec(pc, insn_val, len, "48-bit");
        }
        break;
    }
    case 8: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, MO_LE);
        uint16_t hw3 = translator_lduw_end(env, &ctx->base, pc + 4, MO_LE);
        uint16_t hw4 = translator_lduw_end(env, &ctx->base, pc + 6, MO_LE);
        uint32_t hi = (uint32_t)hw2 | ((uint32_t)hw3 << 16);
        uint32_t top = (uint32_t)hw4;
        uint64_t insn64 = (uint64_t)hw |
                          ((uint64_t)hi << 16) |
                          ((uint64_t)top << 48);
        insn_val = (uint32_t)(insn64 & 0xFFFFFFFFu);
        linx_trace_begin(pc, insn64, len);
        if (!linx_validate_opcode_meta(ctx, pc, insn64, len)) {
            break;
        }
        decoded = decode_insn64(ctx, insn64);
        if (!decoded) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: decode64 failed @ PC=0x%" VADDR_PRIx
                          " insn=0x%016" PRIx64 "\n",
                          pc, insn64);
            linx_illegal(ctx);
        } else {
            trace_linx_insn_exec(pc, insn_val, len, "64-bit");
        }
        break;
    }
    default:
        linx_illegal(ctx);
        break;
    }

    if (linx_commit_trace_enabled && ctx->base.is_jmp != DISAS_NORETURN) {
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(ctx->base.pc_next));
    }

    /* Always update pc_next to ensure tb->size is non-zero */
    ctx->base.pc_next = pc + len;
}

static void linx_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NEXT:
        tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
        tcg_gen_exit_tb(NULL, 0);
        break;
    case DISAS_TOO_MANY:
        if (linx_is_bstart_at_pc(ctx->env, ctx->base.pc_next)) {
            linx_gen_block_end(ctx, ctx->base.pc_next);
        } else {
            tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
            tcg_gen_exit_tb(NULL, 0);
        }
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps linx_tr_ops = {
    .init_disas_context = linx_tr_init_disas_context,
    .tb_start = linx_tr_tb_start,
    .insn_start = linx_tr_insn_start,
    .translate_insn = linx_tr_translate_insn,
    .tb_stop = linx_tr_tb_stop,
};

void linx_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc;
    translator_loop(cs, tb, max_insns, pc, host_pc, &linx_tr_ops, &dc.base);
}

void linx_translate_init(void)
{
    int i;
    const char *watch = getenv("LINX_WATCH_STORE");
    const char *watch_pc = getenv("LINX_WATCH_STORE_PC");
    const char *watch_load = getenv("LINX_WATCH_LOAD");
    const char *watch_load_pc = getenv("LINX_WATCH_LOAD_PC");
    const char *trace_ra = getenv("LINX_TRACE_RA");
    const char *trace_ra_pc = getenv("LINX_TRACE_RA_PC");
    const char *trace_reg = getenv("LINX_TRACE_REG");
    const char *trace_reg_pc = getenv("LINX_TRACE_REG_PC");
    const char *callframe = getenv("LINX_CALLFRAME_SIZE");
    const char *commit_trace = getenv("LINX_COMMIT_TRACE");
    const char *cosim_enable = getenv("LINX_COSIM_ENABLE");
    const char *opcode_meta_strict = getenv("LINX_OPCODE_META_STRICT");
    static const char *gpr_names[LINX_GPR_COUNT] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23"
    };
    static const char *tq_names[4] = { "t#1", "t#2", "t#3", "t#4" };
    static const char *uq_names[4] = { "u#1", "u#2", "u#3", "u#4" };

    linx_commit_trace_enabled =
        (commit_trace && commit_trace[0] && strcmp(commit_trace, "0") != 0) ||
        (cosim_enable && cosim_enable[0] && strcmp(cosim_enable, "0") != 0);
    linx_opcode_meta_strict = !(opcode_meta_strict && opcode_meta_strict[0] && strcmp(opcode_meta_strict, "0") == 0);
    
    for (i = 0; i < LINX_GPR_COUNT; i++) {
        cpu_gpr[i] = tcg_global_mem_new_i64(tcg_env,
                                            offsetof(CPULinxState, gpr[i]),
                                            gpr_names[i]);
    }
    for (i = 0; i < 4; i++) {
        cpu_tq[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPULinxState, tq[i]),
                                           tq_names[i]);
        cpu_uq[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPULinxState, uq[i]),
                                           uq_names[i]);
    }
    cpu_bpc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, bpc), "bpc");
    cpu_tgt = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, tgt), "tgt");
    cpu_cond = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, cond), "cond");
    cpu_carg = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, carg), "carg");
    cpu_brtype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, brtype), "brtype");
    cpu_blocktype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, blocktype), "blocktype");
    cpu_body_tpc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, body_tpc), "body_tpc");
    cpu_return_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, return_pc), "return_pc");
    cpu_in_body = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, in_body), "in_body");
    cpu_tile_func = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_func), "tile_func");
    cpu_tile_dtype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_dtype), "tile_dtype");
    cpu_tile_iot_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_valid), "tile_iot_valid");
    cpu_tile_iot_flags = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_flags), "tile_iot_flags");
    cpu_tile_iot_dst = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_dst), "tile_iot_dst");
    cpu_tile_iot_grp = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_grp), "tile_iot_grp");
    cpu_tile_iot_src0 = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_src0), "tile_iot_src0");
    cpu_tile_iot_src1 = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_src1), "tile_iot_src1");
    cpu_tile_iot_reg = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_reg), "tile_iot_reg");
    cpu_tile_iot_size = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_size), "tile_iot_size");
    cpu_tile_attr_pad = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_attr_pad), "tile_attr_pad");
    cpu_tile_attr_dtype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_attr_dtype), "tile_attr_dtype");
    cpu_lb[0] = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, lb[0]), "lb0");
    cpu_lb[1] = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, lb[1]), "lb1");
    cpu_lb[2] = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, lb[2]), "lb2");
    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, pc), "pc");
    cpu_insn_pc_next = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, insn_pc_next), "insn_pc_next");
    cpu_insn_count = tcg_global_mem_new_i64(tcg_env,
                                            offsetof(CPULinxState, insn_count),
                                            "insn_count");
    cpu_pending_trap_arg0 =
        tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, pending_trap_arg0),
                               "pending_trap_arg0");
    cpu_pending_trap_cause =
        tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, pending_trap_cause),
                               "pending_trap_cause");

    if (linx_commit_trace_enabled) {
        cpu_trace_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_pc), "trace_pc");
        cpu_trace_insn = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_insn), "trace_insn");
        cpu_trace_len = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_len), "trace_len");
        cpu_trace_wb_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_wb_valid), "trace_wb_valid");
        cpu_trace_wb_rd = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_wb_rd), "trace_wb_rd");
        cpu_trace_wb_data = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_wb_data), "trace_wb_data");
        cpu_trace_mem_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_mem_valid), "trace_mem_valid");
        cpu_trace_mem_is_store = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_mem_is_store), "trace_mem_is_store");
        cpu_trace_mem_addr = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_mem_addr), "trace_mem_addr");
        cpu_trace_mem_wdata = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_mem_wdata), "trace_mem_wdata");
        cpu_trace_mem_rdata = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_mem_rdata), "trace_mem_rdata");
        cpu_trace_mem_size = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_mem_size), "trace_mem_size");
        cpu_trace_trap_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_trap_valid), "trace_trap_valid");
        cpu_trace_trap_cause = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_trap_cause), "trace_trap_cause");
        cpu_trace_traparg0 = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_traparg0), "trace_traparg0");
    }

    if (callframe && callframe[0]) {
        char *endp = NULL;
        uint64_t v;

        errno = 0;
        v = strtoull(callframe, &endp, 0);
        if (errno == 0 && endp && endp != callframe && *endp == '\0') {
            if ((v & 7u) == 0) {
                linx_callframe_size = v;
            }
        }
    }

    if (trace_reg && trace_reg[0] && strcmp(trace_reg, "0") != 0) {
        char *endp = NULL;
        unsigned long v;

        errno = 0;
        v = strtoul(trace_reg, &endp, 0);
        if (errno == 0 && endp && endp != trace_reg && *endp == '\0') {
            if (v < LINX_GPR_COUNT) {
                linx_trace_reg = (uint32_t)v;
                linx_trace_reg_enabled = true;
            }
        }
    }

    if (linx_trace_reg_enabled && trace_reg_pc && trace_reg_pc[0] &&
        strcmp(trace_reg_pc, "0") != 0) {
        char *endp = NULL;
        char *endp2 = NULL;
        uint64_t lo;
        uint64_t hi;

        errno = 0;
        lo = strtoull(trace_reg_pc, &endp, 0);
        if (errno == 0 && endp && endp != trace_reg_pc) {
            if (*endp == '-' || *endp == ':') {
                errno = 0;
                hi = strtoull(endp + 1, &endp2, 0);
                if (errno == 0 && endp2 && endp2 != endp + 1 &&
                    *endp2 == '\0') {
                    linx_trace_reg_pc_lo = MIN(lo, hi);
                    linx_trace_reg_pc_hi = MAX(lo, hi);
                    linx_trace_reg_pc_filter_enabled = true;
                }
            } else if (*endp == '\0') {
                linx_trace_reg_pc_lo = lo;
                linx_trace_reg_pc_hi = lo;
                linx_trace_reg_pc_filter_enabled = true;
            }
        }
    }

    if (trace_ra && trace_ra[0] && strcmp(trace_ra, "0") != 0) {
        linx_trace_ra_enabled = true;
        if (trace_ra_pc && trace_ra_pc[0] && strcmp(trace_ra_pc, "0") != 0) {
            char *endp = NULL;
            uint64_t pc;

            errno = 0;
            pc = strtoull(trace_ra_pc, &endp, 0);
            if (errno == 0 && endp && endp != trace_ra_pc && *endp == '\0') {
                linx_trace_ra_pc = pc;
                linx_trace_ra_pc_enabled = true;
            }
        }
    }

    if (watch_pc && watch_pc[0] && strcmp(watch_pc, "0") != 0) {
        char *endp = NULL;
        char *endp2 = NULL;
        uint64_t lo;
        uint64_t hi;

        errno = 0;
        lo = strtoull(watch_pc, &endp, 0);
        if (errno == 0 && endp && endp != watch_pc) {
            if (*endp == '-' || *endp == ':') {
                errno = 0;
                hi = strtoull(endp + 1, &endp2, 0);
                if (errno == 0 && endp2 && endp2 != endp + 1 && *endp2 == '\0') {
                    linx_watch_store_pc_lo = MIN(lo, hi);
                    linx_watch_store_pc_hi = MAX(lo, hi);
                    linx_watch_store_pc_filter_enabled = true;
                }
            } else if (*endp == '\0') {
                linx_watch_store_pc_lo = lo;
                linx_watch_store_pc_hi = lo;
                linx_watch_store_pc_filter_enabled = true;
            }
        }
    }

    if (watch_load_pc && watch_load_pc[0] && strcmp(watch_load_pc, "0") != 0) {
        char *endp = NULL;
        char *endp2 = NULL;
        uint64_t lo;
        uint64_t hi;

        errno = 0;
        lo = strtoull(watch_load_pc, &endp, 0);
        if (errno == 0 && endp && endp != watch_load_pc) {
            if (*endp == '-' || *endp == ':') {
                errno = 0;
                hi = strtoull(endp + 1, &endp2, 0);
                if (errno == 0 && endp2 && endp2 != endp + 1 &&
                    *endp2 == '\0') {
                    linx_watch_load_pc_lo = MIN(lo, hi);
                    linx_watch_load_pc_hi = MAX(lo, hi);
                    linx_watch_load_pc_filter_enabled = true;
                }
            } else if (*endp == '\0') {
                linx_watch_load_pc_lo = lo;
                linx_watch_load_pc_hi = lo;
                linx_watch_load_pc_filter_enabled = true;
            }
        }
    }

    if (watch && watch[0] && strcmp(watch, "0") != 0) {
        char *endp = NULL;
        uint64_t addr;
        unsigned long len = 1;

        errno = 0;
        addr = strtoull(watch, &endp, 0);
        if (errno == 0 && endp && endp != watch) {
            if (*endp == ':') {
                errno = 0;
                len = strtoul(endp + 1, NULL, 0);
                if (errno != 0 || len == 0) {
                    len = 1;
                }
            } else if (*endp != '\0') {
                len = 0;
            }
        } else {
            len = 0;
        }

        if (len) {
            linx_watch_store_lo = addr;
            linx_watch_store_hi = addr + (uint64_t)len - 1;
            linx_watch_store_enabled = true;
        }
    }

    if (watch_load && watch_load[0] && strcmp(watch_load, "0") != 0) {
        char *endp = NULL;
        uint64_t addr;
        unsigned long len = 1;

        errno = 0;
        addr = strtoull(watch_load, &endp, 0);
        if (errno == 0 && endp && endp != watch_load) {
            if (*endp == ':') {
                errno = 0;
                len = strtoul(endp + 1, NULL, 0);
                if (errno != 0 || len == 0) {
                    len = 1;
                }
            } else if (*endp != '\0') {
                len = 0;
            }
        } else {
            len = 0;
        }

        if (len) {
            linx_watch_load_lo = addr;
            linx_watch_load_hi = addr + (uint64_t)len - 1;
            linx_watch_load_enabled = true;
        }
    }
}
