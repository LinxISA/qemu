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

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPULinxState *env;

    uint8_t brtype;
    vaddr brtarget;
    uint32_t cur_insn_len;
    bool tgt_modified;
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
static TCGv_i64 cpu_tgt;
static TCGv_i32 cpu_cond;
static TCGv_i32 cpu_carg;  /* Commit argument flag */
static TCGv_i32 cpu_brtype;
static TCGv_i64 cpu_pc;

static unsigned linx_insn_len(uint16_t hw);

static inline MemOp linx_mo_endian(void)
{
    return MO_LE;
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
    }
}

static void linx_block_begin(DisasContext *ctx, uint8_t brtype, vaddr initial_target)
{
    int i;
    for (i = 0; i < 4; i++) {
        tcg_gen_movi_i64(cpu_tq[i], 0);
        tcg_gen_movi_i64(cpu_uq[i], 0);
    }
    tcg_gen_movi_i32(cpu_cond, 0);
    tcg_gen_movi_i32(cpu_carg, 0);
    tcg_gen_movi_i32(cpu_brtype, brtype);
    ctx->tgt_modified = false;
    
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

        /* BSTART.*: low byte 0x01, branch kind in bits [14:12] is non-zero. */
        if ((insn & 0xff) == 0x01 && ((insn >> 12) & 0x7) != 0) {
            return true;
        }

        /* Template blocks: FENTRY/FEXIT/FRET.* share opcode bits[6:0]=0x41. */
        if ((insn & 0x7f) == 0x41 && ((insn >> 12) & 0x7) <= 3) {
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


static void linx_gen_goto_tb(DisasContext *ctx, int slot, vaddr dest)
{
    if (!linx_is_bstart_at_pc(ctx->env, dest)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: jump target 0x%" VADDR_PRIx " is not a block start marker\n",
                      dest);
        tcg_gen_movi_i64(cpu_pc, dest);
        gen_helper_raise_exception(tcg_env,
                                  tcg_constant_i32(LINX_EXCP_BAD_BRANCH_TARGET));
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
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
    switch (ctx->brtype & 0x7) {
    case LINX_BR_FALL:
        /* Always fall through */
        linx_gen_goto_tb(ctx, 0, fallthrough);
        break;
    case LINX_BR_DIRECT:
    case LINX_BR_CALL:
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            /*
             * Fast path: direct/call blocks with a fixed PC-relative target and no
             * SETC.TGT override can be emitted as a direct TB branch.
             */
            linx_gen_goto_tb(ctx, 0, ctx->brtarget);
        } else {
            /* Jump to cpu_tgt (diverted target from BSTART, or set target from SETC.TGT). */
            gen_helper_linx_check_bstart_target(tcg_env, cpu_tgt);
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
        linx_gen_goto_tb(ctx, 1, fallthrough);
        gen_set_label(taken);
        /* Condition set: jump to diverted/set target */
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            /* Fixed target: enable TB chaining for the taken edge. */
            linx_gen_goto_tb(ctx, 0, ctx->brtarget);
        } else {
            gen_helper_linx_check_bstart_target(tcg_env, cpu_tgt);
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
        linx_gen_goto_tb(ctx, 1, fallthrough);
        gen_set_label(taken);
        gen_helper_linx_check_bstart_target(tcg_env, cpu_tgt);
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    }
    case LINX_BR_IND:
    case LINX_BR_ICALL:
        /* Indirect jump/call: jump to cpu_tgt (must be set by SETC.TGT) */
        gen_helper_linx_check_bstart_target(tcg_env, cpu_tgt);
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    default:
        /* Unhandled block kind: fall through. */
        linx_gen_goto_tb(ctx, 0, fallthrough);
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
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_ILLEGAL_INST));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* Include the auto-generated decoders. */
#include "decode-insn16.c.inc"
#include "decode-insn32.c.inc"
#include "decode-insn48.c.inc"

/* C.BSTART.STD handler - called explicitly from linx_tr_translate_insn */
static bool trans_c_bstart_std(DisasContext *ctx, uint8_t brtype)
{
    /* pc_next has already been advanced past the current insn, so we need to
     * check if the CURRENT instruction (pc_next - cur_insn_len) is at pc_first */
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
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

static bool trans_bstart_call(DisasContext *ctx, arg_bstart_call *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
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
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_COND, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_setret(DisasContext *ctx, arg_setret *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr tgt = pc + ((vaddr)a->imm20 << 1);
    tcg_gen_movi_i64(cpu_gpr[LINX_REG_RA], tgt);
    return true;
}

static bool trans_c_setc_tgt(DisasContext *ctx, arg_c_setc_tgt *a)
{
    if (ctx->brtype == 0) {
        return linx_illegal(ctx);
    }
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    ctx->tgt_modified = true;
    return true;
}

static bool trans_setc_tgt(DisasContext *ctx, arg_setc_tgt *a)
{
    if (ctx->brtype == 0) {
        return linx_illegal(ctx);
    }
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    ctx->tgt_modified = true;
    return true;
}

static bool trans_setc_cmp(DisasContext *ctx, TCGCond c, TCGv_i64 l, TCGv_i64 r);

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

static bool trans_setc_cmp(DisasContext *ctx, TCGCond c, TCGv_i64 l, TCGv_i64 r)
{
    if (ctx->brtype == 0) {
        return linx_illegal(ctx);
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

static bool trans_setc_ltu(DisasContext *ctx, arg_setc_ltu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_LTU, l, r);
}

static bool trans_setc_ge(DisasContext *ctx, arg_setc_ge *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_GE, l, r);
}

static bool trans_setc_geu(DisasContext *ctx, arg_setc_geu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_GEU, l, r);
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

static bool trans_mulw(DisasContext *ctx, arg_mulw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();

    tcg_gen_mul_i64(out, l, r);
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
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shl_i64(out, l, sh);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_srl(DisasContext *ctx, arg_srl *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shr_i64(out, l, sh);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_sra(DisasContext *ctx, arg_sra *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sar_i64(out, l, sh);
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
        tcg_gen_movi_i64(cpu_gpr[LINX_REG_RA], tgt);
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

    if ((idx_type & 0x3) == 0) {
        tcg_gen_ext32s_i64(t, i);
    } else {
        tcg_gen_ext32u_i64(t, i);
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
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_qemu_ld_i64(out, addr, 0, mop | linx_mo_endian());
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

/* Helper to decode the split uimm field into actual byte size */
static inline int64_t linx_decode_fentry_uimm(uint32_t uimm_hi, uint32_t uimm_lo)
{
    /* uimm_hi contains bits [14:10], uimm_lo contains bits [9:3] */
    /* Reconstruct: (uimm_hi << 10) | (uimm_lo << 3) */
    return ((int64_t)uimm_hi << 10) | ((int64_t)uimm_lo << 3);
}

/* Helper to get next register in FENTRY range (incrementing with wraparound) */
static inline int linx_next_fentry_reg(int current, int end_adjusted)
{
    current++;
    if (current > 23) {
        current = 2;  /* Wrap from R23 to R2 */
    }
    return current;
}

/* Calculate how many registers to save/restore given [begin, end] range */
static inline int linx_fentry_reg_count(int begin, int end)
{
    if (begin <= end) {
        return end - begin + 1;
    } else {
        /* Wraparound: begin to R23, then R2 to end */
        return (23 - begin + 1) + (end - 2 + 1);
    }
}

static void linx_fentry_save_regs(DisasContext *ctx, int begin, int end,
                                  int64_t stacksize)
{
    /* Save registers into the current frame:
     *   [sp + stacksize - 8]  = reg_begin
     *   [sp + stacksize - 16] = next reg
     *   ...
     *
     * This matches the bring-up toolchain convention and keeps low offsets
     * available for locals.
     */
    if (stacksize <= 0) {
        return;
    }

    const int count = linx_fentry_reg_count(begin, end);
    int reg = begin;
    for (int i = 0; i < count; i++) {
        const int64_t off = stacksize - ((int64_t)(i + 1) * 8);
        if (off < 0) {
            break;
        }

        if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
            TCGv_i64 addr64 = tcg_temp_new_i64();
            tcg_gen_addi_i64(addr64, cpu_gpr[LINX_REG_SP], off);
            linx_store_from_reg(ctx, linx_addr_from_i64(addr64),
                                cpu_gpr[reg], MO_UQ);
        }

        if (reg == end) {
            break;
        }
        reg = linx_next_fentry_reg(reg, end);
    }
}

static void linx_fentry_restore_regs(DisasContext *ctx, int begin, int end,
                                     int64_t stacksize)
{
    if (stacksize <= 0) {
        return;
    }

    const int count = linx_fentry_reg_count(begin, end);
    int reg = begin;
    for (int i = 0; i < count; i++) {
        const int64_t off = stacksize - ((int64_t)(i + 1) * 8);
        if (off < 0) {
            break;
        }

        if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
            TCGv_i64 addr64 = tcg_temp_new_i64();
            TCGv_i64 val = tcg_temp_new_i64();
            tcg_gen_addi_i64(addr64, cpu_gpr[LINX_REG_SP], off);
            tcg_gen_qemu_ld_i64(val, linx_addr_from_i64(addr64), 0,
                                MO_UQ | linx_mo_endian());
            tcg_gen_mov_i64(cpu_gpr[reg], val);
        }

        if (reg == end) {
            break;
        }
        reg = linx_next_fentry_reg(reg, end);
    }
}

/* FENTRY: Function entry - save registers [Begin ~ End], adjust SP */
static bool trans_fentry(DisasContext *ctx, arg_fentry *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    /* Standalone frame macro blocks start execution without an explicit BSTART. */
    linx_block_begin(ctx, LINX_BR_FALL, 0);

    int64_t stacksize = linx_decode_fentry_uimm(a->uimm_hi, a->uimm_lo);
    TCGv_i64 sp = cpu_gpr[LINX_REG_SP];
    
    /* SP = SP - stacksize */
    if (stacksize > 0) {
        tcg_gen_subi_i64(sp, sp, stacksize);
    }
    linx_fentry_save_regs(ctx, a->reg_begin, a->reg_end, stacksize);
    return true;
}

/* FEXIT: Function exit - restore registers, adjust SP (used with IND block for indirect return) */
static bool trans_fexit(DisasContext *ctx, arg_fexit *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);

    int64_t stacksize = linx_decode_fentry_uimm(a->uimm_hi, a->uimm_lo);
    TCGv_i64 sp = cpu_gpr[LINX_REG_SP];

    linx_fentry_restore_regs(ctx, a->reg_begin, a->reg_end, stacksize);
    
    /* SP = SP + stacksize */
    if (stacksize > 0) {
        tcg_gen_addi_i64(sp, sp, stacksize);
    }
    
    /* FEXIT does NOT return directly - it's followed by an IND block with setc.tgt */
    return true;
}

/* FRET.RA: Function return via RA - restore registers, adjust SP, return to RA */
static bool trans_fret_ra(DisasContext *ctx, arg_fret_ra *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);

    int64_t stacksize = linx_decode_fentry_uimm(a->uimm_hi, a->uimm_lo);
    TCGv_i64 sp = cpu_gpr[LINX_REG_SP];
    TCGv_i64 ra = cpu_gpr[LINX_REG_RA];

    linx_fentry_restore_regs(ctx, a->reg_begin, a->reg_end, stacksize);
    
    /* SP = SP + stacksize */
    if (stacksize > 0) {
        tcg_gen_addi_i64(sp, sp, stacksize);
    }
    
    /* Return via RA */
    gen_helper_linx_check_bstart_target(tcg_env, ra);
    tcg_gen_mov_i64(cpu_pc, ra);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* FRET.STK: Function return via stack - restore registers, adjust SP, return */
static bool trans_fret_stk(DisasContext *ctx, arg_fret_stk *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);

    int64_t stacksize = linx_decode_fentry_uimm(a->uimm_hi, a->uimm_lo);
    TCGv_i64 sp = cpu_gpr[LINX_REG_SP];
    TCGv_i64 ra = cpu_gpr[LINX_REG_RA];

    linx_fentry_restore_regs(ctx, a->reg_begin, a->reg_end, stacksize);
    
    /* SP = SP + stacksize */
    if (stacksize > 0) {
        tcg_gen_addi_i64(sp, sp, stacksize);
    }
    
    /* Return via RA (which was restored from stack) */
    gen_helper_linx_check_bstart_target(tcg_env, ra);
    tcg_gen_mov_i64(cpu_pc, ra);
    tcg_gen_lookup_and_goto_ptr();
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
    /* CLZ: Count leading zeros */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_clzi_i64(out, src, 64);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_ctz(DisasContext *ctx, arg_ctz *a)
{
    /* CTZ: Count trailing zeros */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ctzi_i64(out, src, 64);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bcnt(DisasContext *ctx, arg_bcnt *a)
{
    /* BCNT: Population count (count 1 bits) */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ctpop_i64(out, src);
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
    unsigned lsb = a->imml;
    unsigned width = a->imms + 1;
    if (width > 0 && lsb + width <= 64) {
        tcg_gen_sextract_i64(out, src, lsb, width);
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
    unsigned lsb = a->imml;
    unsigned width = a->imms + 1;
    if (width > 0 && lsb + width <= 64) {
        tcg_gen_extract_i64(out, src, lsb, width);
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
    unsigned lsb = a->imml;
    unsigned width = a->imms + 1;
    if (width > 0 && lsb + width <= 64) {
        uint64_t mask = ~(((1ULL << width) - 1) << lsb);
        tcg_gen_andi_i64(out, src, mask);
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
    unsigned lsb = a->imml;
    unsigned width = a->imms + 1;
    if (width > 0 && lsb + width <= 64) {
        uint64_t mask = ((1ULL << width) - 1) << lsb;
        tcg_gen_ori_i64(out, src, mask);
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

static bool trans_b_z(DisasContext *ctx, arg_b_z *a)
{
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

static bool trans_hl_subi(DisasContext *ctx, arg_hl_subi *a)
{
    /* HL.SUBI: Subtract with 24-bit unsigned immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
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

static bool trans_hl_ori(DisasContext *ctx, arg_hl_ori *a)
{
    /* HL.ORI: OR with 24-bit signed immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
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
    /* End TB since we don't know if helper will return for some cases */
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static void linx_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULinxState *env = cpu_env(cpu);

    ctx->env = env;
    ctx->brtype = (uint8_t)env->brtype;
    ctx->brtarget = 0;
    ctx->cur_insn_len = 0;
    ctx->tgt_modified = false;
}

static void linx_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
}

static void linx_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next);
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

    /* LinxISA is little-endian: bytes in memory like [00 08] should be read as 0x0800 */
    /* Use MO_LE to read instruction in little-endian format */
    hw = translator_lduw_end(env, &ctx->base, pc, MO_LE);
    
    len = linx_insn_len(hw);
    ctx->cur_insn_len = len;
    /* Always update pc_next to ensure tb->size is non-zero even if exception occurs */
    ctx->base.pc_next = pc + len;

    switch (len) {
           case 2:
               insn_val = hw;
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
               } else {
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
        decoded = decode_insn32(ctx, insn_val);
        if (!decoded) {
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
        insn_val = (uint32_t)(insn48 & 0xFFFFFFFF);
        decoded = decode_insn48(ctx, insn48);
        if (!decoded) {
            linx_illegal(ctx);
        } else {
            trace_linx_insn_exec(pc, insn_val, len, "48-bit");
        }
        break;
    }
    default:
        linx_illegal(ctx);
        break;
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
    static const char *gpr_names[LINX_GPR_COUNT] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23"
    };
    static const char *tq_names[4] = { "t#1", "t#2", "t#3", "t#4" };
    static const char *uq_names[4] = { "u#1", "u#2", "u#3", "u#4" };
    
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
    cpu_tgt = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, tgt), "tgt");
    cpu_cond = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, cond), "cond");
    cpu_carg = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, carg), "carg");
    cpu_brtype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, brtype), "brtype");
    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, pc), "pc");
}
