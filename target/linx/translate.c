/*
 * LinxISA translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "exec/translation-block.h"
#include "exec/log.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPULinxState *env;

    uint8_t brtype;
    vaddr brtarget;
    uint32_t cur_insn_len;
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
static TCGv_i64 cpu_pc;

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

static void linx_block_begin(DisasContext *ctx, uint8_t brtype, vaddr brtarget)
{
    int i;
    for (i = 0; i < 4; i++) {
        tcg_gen_movi_i64(cpu_tq[i], 0);
        tcg_gen_movi_i64(cpu_uq[i], 0);
    }
    tcg_gen_movi_i32(cpu_cond, 0);
    tcg_gen_movi_i64(cpu_tgt, 0);

    ctx->brtype = brtype;
    ctx->brtarget = brtarget;
}

static void linx_gen_goto_tb(DisasContext *ctx, int slot, vaddr dest)
{
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
        linx_gen_goto_tb(ctx, 0, fallthrough);
        break;
    case LINX_BR_DIRECT:
    case LINX_BR_CALL:
        linx_gen_goto_tb(ctx, 0, ctx->brtarget);
        break;
    case LINX_BR_COND: {
        TCGLabel *taken = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
        linx_gen_goto_tb(ctx, 1, fallthrough);
        gen_set_label(taken);
        linx_gen_goto_tb(ctx, 0, ctx->brtarget);
        break;
    }
    case LINX_BR_RET: {
        TCGLabel *taken = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
        linx_gen_goto_tb(ctx, 1, fallthrough);
        gen_set_label(taken);
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    }
    case LINX_BR_IND:
    case LINX_BR_ICALL:
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    default:
        /* Unknown block kind: fall through. */
        linx_gen_goto_tb(ctx, 0, fallthrough);
        break;
    }
}

static inline uint64_t linx_zext32(uint64_t x)
{
    return (uint32_t)x;
}

static inline uint64_t linx_sext32(uint64_t x)
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
    qemu_log_mask(LOG_GUEST_ERROR, "Linx: illegal instruction @ 0x%" VADDR_PRIx "\n",
                  ctx->base.pc_next);
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* Include the auto-generated decoders. */
#include "decode-insn16.c.inc"
#include "decode-insn32.c.inc"
#include "decode-insn48.c.inc"

static bool trans_c_bstart(DisasContext *ctx, arg_c_bstart *a)
{
    if (ctx->base.pc_next != ctx->base.pc_first) {
        linx_gen_block_end(ctx, ctx->base.pc_next);
        return true;
    }
    linx_block_begin(ctx, a->BrType, 0);
    return true;
}

static bool trans_c_bstart_direct(DisasContext *ctx, arg_c_bstart_direct *a)
{
    if (ctx->base.pc_next != ctx->base.pc_first) {
        linx_gen_block_end(ctx, ctx->base.pc_next);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_DIRECT, linx_pcrel_target(ctx->base.pc_next, a->simm12));
    return true;
}

static bool trans_c_bstart_cond(DisasContext *ctx, arg_c_bstart_cond *a)
{
    if (ctx->base.pc_next != ctx->base.pc_first) {
        linx_gen_block_end(ctx, ctx->base.pc_next);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_COND, linx_pcrel_target(ctx->base.pc_next, a->simm12));
    return true;
}

static bool trans_c_bstop(DisasContext *ctx, arg_c_bstop *a)
{
    linx_gen_block_end(ctx, ctx->base.pc_next + ctx->cur_insn_len);
    return true;
}

static bool trans_bstart_call(DisasContext *ctx, arg_bstart_call *a)
{
    if (ctx->base.pc_next != ctx->base.pc_first) {
        linx_gen_block_end(ctx, ctx->base.pc_next);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_CALL, linx_pcrel_target(ctx->base.pc_next, a->simm17));
    return true;
}

static bool trans_bstart_direct(DisasContext *ctx, arg_bstart_direct *a)
{
    if (ctx->base.pc_next != ctx->base.pc_first) {
        linx_gen_block_end(ctx, ctx->base.pc_next);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_DIRECT, linx_pcrel_target(ctx->base.pc_next, a->simm17));
    return true;
}

static bool trans_bstart_cond(DisasContext *ctx, arg_bstart_cond *a)
{
    if (ctx->base.pc_next != ctx->base.pc_first) {
        linx_gen_block_end(ctx, ctx->base.pc_next);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_COND, linx_pcrel_target(ctx->base.pc_next, a->simm17));
    return true;
}

static bool trans_c_setret(DisasContext *ctx, arg_c_setret *a)
{
    vaddr pc = ctx->base.pc_next;
    vaddr tgt = pc + ((vaddr)a->uimm5 << 1);
    tcg_gen_movi_i64(cpu_gpr[LINX_REG_RA], tgt);
    return true;
}

static bool trans_setret(DisasContext *ctx, arg_setret *a)
{
    vaddr pc = ctx->base.pc_next;
    vaddr tgt = pc + ((vaddr)a->imm20 << 1);
    tcg_gen_movi_i64(cpu_gpr[LINX_REG_RA], tgt);
    return true;
}

static bool trans_c_setc_tgt(DisasContext *ctx, arg_c_setc_tgt *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    return true;
}

static bool trans_setc_tgt(DisasContext *ctx, arg_setc_tgt *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    return true;
}

static bool trans_c_setc_eq(DisasContext *ctx, arg_c_setc_eq *a)
{
    TCGLabel *t = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);

    tcg_gen_movi_i32(cpu_cond, 0);
    tcg_gen_brcond_i64(TCG_COND_EQ, l, r, t);
    tcg_gen_br(done);
    gen_set_label(t);
    tcg_gen_movi_i32(cpu_cond, 1);
    gen_set_label(done);
    return true;
}

static bool trans_c_setc_ne(DisasContext *ctx, arg_c_setc_ne *a)
{
    TCGLabel *t = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);

    tcg_gen_movi_i32(cpu_cond, 0);
    tcg_gen_brcond_i64(TCG_COND_NE, l, r, t);
    tcg_gen_br(done);
    gen_set_label(t);
    tcg_gen_movi_i32(cpu_cond, 1);
    gen_set_label(done);
    return true;
}

static bool trans_setc_cmp(DisasContext *ctx, TCGCond c, TCGv_i64 l, TCGv_i64 r)
{
    TCGLabel *t = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_movi_i32(cpu_cond, 0);
    tcg_gen_brcond_i64(c, l, r, t);
    tcg_gen_br(done);
    gen_set_label(t);
    tcg_gen_movi_i32(cpu_cond, 1);
    gen_set_label(done);
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
    TCGv_i64 pred = linx_get_reg(a->SrcP);
    TCGv_i64 tval = linx_get_reg(a->SrcL);
    TCGv_i64 fval = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    TCGLabel *l_true = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_movi_i64(out, 0);
    tcg_gen_brcondi_i64(TCG_COND_NE, pred, 0, l_true);
    tcg_gen_mov_i64(out, fval);
    tcg_gen_br(done);
    gen_set_label(l_true);
    tcg_gen_mov_i64(out, tval);
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
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shl_i64(out, l, sh);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_srlw(DisasContext *ctx, arg_srlw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shr_i64(out, l, sh);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_sraw(DisasContext *ctx, arg_sraw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sar_i64(out, l, sh);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_slliw(DisasContext *ctx, arg_slliw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shli_i64(out, l, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_srliw(DisasContext *ctx, arg_srliw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shri_i64(out, l, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_sraiw(DisasContext *ctx, arg_sraiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sari_i64(out, l, a->shamt & 0x1f);
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
    TCGv_i64 v = tcg_temp_new_i64();
    tcg_gen_movi_i64(v, (int64_t)a->simm5);
    linx_set_dest(a->RegDst, v);
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
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SH);
}

static bool trans_lhui(DisasContext *ctx, arg_lhui *a)
{
    int64_t off = (int64_t)a->simm12 * 2;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UH);
}

static bool trans_lwi(DisasContext *ctx, arg_lwi *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lwui(DisasContext *ctx, arg_lwui *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_ldi(DisasContext *ctx, arg_ldi *a)
{
    int64_t off = (int64_t)a->simm12 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_Q);
}

static bool trans_lb(DisasContext *ctx, arg_lb *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_lw(DisasContext *ctx, arg_lw *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_ld(DisasContext *ctx, arg_ld *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_Q);
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
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UH);
}

static bool trans_swi(DisasContext *ctx, arg_swi *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_sdi(DisasContext *ctx, arg_sdi *a)
{
    int64_t off = (int64_t)a->simm12 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_Q);
}

static bool trans_sb(DisasContext *ctx, arg_sb *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UB);
}

static bool trans_sw(DisasContext *ctx, arg_sw *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 2);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UW);
}

static bool trans_sd(DisasContext *ctx, arg_sd *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 3);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_Q);
}

static bool trans_c_lwi(DisasContext *ctx, arg_c_lwi *a)
{
    int64_t off = (int64_t)a->simm5 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, 31, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_c_ldi(DisasContext *ctx, arg_c_ldi *a)
{
    int64_t off = (int64_t)a->simm5 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, 31, linx_addr_from_i64(addr64), MO_Q);
}

static bool trans_c_swi(DisasContext *ctx, arg_c_swi *a)
{
    int64_t off = (int64_t)a->simm5 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), cpu_tq[0], MO_UW);
}

static bool trans_c_sdi(DisasContext *ctx, arg_c_sdi *a)
{
    int64_t off = (int64_t)a->simm5 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), cpu_tq[0], MO_Q);
}

static bool trans_ebreak(DisasContext *ctx, arg_ebreak *a)
{
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    gen_helper_linx_ebreak(tcg_env);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static void linx_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULinxState *env = cpu_env(cpu);

    ctx->env = env;
    ctx->brtype = LINX_BR_FALL;
    ctx->brtarget = 0;
    ctx->cur_insn_len = 0;
}

static void linx_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
}

static void linx_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next, 0, 0);
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
    uint16_t hw = translator_lduw_end(env, &ctx->base, pc, linx_mo_endian());
    unsigned len = linx_insn_len(hw);

    ctx->cur_insn_len = len;

    switch (len) {
    case 2:
        if (!decode_insn16(ctx, hw)) {
            linx_illegal(ctx);
        }
        break;
    case 4: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, linx_mo_endian());
        uint32_t insn = (uint32_t)hw | ((uint32_t)hw2 << 16);
        if (!decode_insn32(ctx, insn)) {
            linx_illegal(ctx);
        }
        break;
    }
    case 6: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, linx_mo_endian());
        uint16_t hw3 = translator_lduw_end(env, &ctx->base, pc + 4, linx_mo_endian());
        uint32_t hi = (uint32_t)hw2 | ((uint32_t)hw3 << 16);
        uint64_t insn48 = (uint64_t)hw | ((uint64_t)hi << 16);
        if (!decode_insn48(ctx, insn48)) {
            linx_illegal(ctx);
        }
        break;
    }
    default:
        linx_illegal(ctx);
        break;
    }

    if (ctx->base.is_jmp == DISAS_NEXT) {
        ctx->base.pc_next += len;
    }
}

static void linx_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NEXT:
    case DISAS_TOO_MANY:
        tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
        tcg_gen_exit_tb(NULL, 0);
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
    for (i = 0; i < LINX_GPR_COUNT; i++) {
        cpu_gpr[i] = tcg_global_mem_new_i64(tcg_env,
                                            offsetof(CPULinxState, gpr[i]),
                                            g_strdup_printf("r%d", i));
    }
    for (i = 0; i < 4; i++) {
        cpu_tq[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPULinxState, tq[i]),
                                           g_strdup_printf("t#%d", i + 1));
        cpu_uq[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPULinxState, uq[i]),
                                           g_strdup_printf("u#%d", i + 1));
    }
    cpu_tgt = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, tgt), "tgt");
    cpu_cond = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, cond), "cond");
    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, pc), "pc");
}
