/*
 * Linx Block ISA translation.
 *
 * Copyright (c) 2021-2024 Hisilicon Technologies.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "fpu/softfloat.h"

#define SIMT_WRAP_BEGIN                                                     \
    CPULINXState *env = ctx->cs->env_ptr;                                   \
    for (int lane_id = 0; lane_id < env->csr_lanenum; ++lane_id) {          \
        TCGLabel *disable_lane = gen_new_label();                           \
        TCGv lane_exec_flag = tcg_temp_local_new();                         \
        tcg_gen_andi_tl(lane_exec_flag, predm, 1ull << lane_id);            \
        tcg_gen_brcondi_tl(TCG_COND_EQ, lane_exec_flag, 0, disable_lane);

#define SIMT_WRAP_END                                                       \
        gen_set_label(disable_lane);                                        \
        temp_free(ctx);                                                     \
    }

#define SIMT_WRAP_END_DEST_EXT(D_REG)                                       \
        gen_set_label(disable_lane);                                        \
        tcg_gen_movcond_tl(TCG_COND_EQ, dest, lane_exec_flag,               \
            ctx->zero, ctx->zero, dest);                                    \
        set_dst_regx_fvec_with_width(ctx, dest, D_REG, lane_id);            \
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM)) {                          \
            gen_helper_vec_print(dest);                                     \
        }                                                                   \
        tcg_temp_free(dest);                                                \
        tcg_temp_free(lane_exec_flag);                                      \
        temp_free(ctx);                                                     \
    }

#define SIMT_WRAP_END_DEST(D_REG, TYP)                                      \
        gen_set_label(disable_lane);                                        \
        tcg_gen_movcond_tl(TCG_COND_EQ, dest, lane_exec_flag,               \
            tcg_constant_tl(0), tcg_constant_tl(0), dest);                  \
        set_dst_regx_fvec(ctx, dest, D_REG, lane_id, TYP);                  \
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM)) {                          \
            gen_helper_vec_print(dest);                                     \
        }                                                                   \
        tcg_temp_free(dest);                                                \
        tcg_temp_free(lane_exec_flag);                                      \
        temp_free(ctx);                                                     \
    }
#define GET_WIDTH(xlen) \
({                      \
    int width = 64;     \
    switch (xlen) {     \
    case WIDTH_DOUBLE:  \
        width = 64;     \
        break;          \
    case WIDTH_WORD:    \
        width = 32;     \
        break;          \
    case WIDTH_HALF:    \
        width = 16;     \
        break;          \
    case WIDTH_BYTE:    \
        width = 8;      \
        break;          \
    default:            \
        width = 64;     \
        break;          \
    }                   \
    width;              \
})

static bool is_vec_reg(uint32_t dst_code)
{
    int dst = extract16(dst_code, 0, 7);
    switch (dst) {
    case DST_FVEC_VT:
    case DST_FVEC_VU:
    case DST_FVEC_VM:
    case DST_FVEC_VN:
    case DST_FVEC_ZERO:
        return true;
    case DST_FVEC_U:
    case DST_FVEC_T:
    case DST_FVEC_PRED:
    default: return false;
    }
    printf("writing to a strange register is not permitted."
           " dst: %d\n", dst_code);
    assert(0);
}

static FloatRoundMode get_real_round_mode(uint64_t val)
{
    enum {NONE = 0, RNE = 1, RTZ, RDN, RUP, RNA, RTO, RHB, InvalidR};
    switch (val) {
    case NONE:
    case RNE: return float_round_nearest_even;
    case RTZ: return float_round_to_zero;
    case RDN: return float_round_down;
    case RUP: return float_round_up;
    case RNA: return float_round_ties_away;
    case RTO: return float_round_to_odd;
    case RHB: return float_round_nearest_even;
    }
    assert(0);
    return 0;
}

static bool simt_scalar_set_dst(DisasContext *ctx, uint32_t RegDst, TCGv dest)
{
    switch (extract16(RegDst, 0, 7)) {
    case DST_FVEC_PRED:
        tcg_gen_mov_tl(predm, dest);
        break;
    case DST_FVEC_T:
        set_dst_regx(ctx, dest, TARGET_REG_T);
        break;
    case DST_FVEC_U:
        set_dst_regx(ctx, dest, TARGET_REG_U);
        break;
    default:
        assert(0);
    }
    return true;
}

static inline bool gen_scalar_arith(DisasContext *ctx, arg_simt_arith *a,
                                  int type, void (*func)(TCGv, TCGv, TCGv))
{
    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, 0);
    TCGv dest = temp_new(ctx);

    tcg_gen_shli_tl(src_r, src_r, a->shamt);
    func(dest, src_l, src_r);

    simt_scalar_set_dst(ctx, a->RegDst, dest);

    if (DST_FVEC_PRED == extract16(a->RegDst, 0, 7)) {
        /* disconnect tb when writing to the p register. */
        gen_goto_tb(ctx, 1, ctx->base.pc_next + 8);
    }
    return true;
}

static inline bool gen_simt_arith(DisasContext *ctx, arg_simt_arith *a,
                                  int type, void (*func)(TCGv, TCGv, TCGv))
{
    if (!is_vec_reg(a->RegDst)) {
        return gen_scalar_arith(ctx, a, type, func);
    }

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    src_r = conver_src_value(ctx, src_r, type, a->SrcRType);

    tcg_gen_shli_tl(src_r, src_r, a->shamt);

    func(dest, src_l, src_r);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    if (DST_FVEC_PRED == extract16(a->RegDst, 0, 7)) {
        /* disconnect tb when writing to the p register. */
        gen_goto_tb(ctx, 1, ctx->base.pc_next + 8);
    }
    return true;
}

static bool trans_simt_add_32(DisasContext *ctx, arg_simt_add_32 *a)
{
    return gen_simt_arith(ctx, a, INSTR_TYPE_AU, tcg_gen_add_tl);
}

static bool trans_simt_sub_32(DisasContext *ctx, arg_simt_sub_32 *a)
{
    return gen_simt_arith(ctx, a, INSTR_TYPE_AU, tcg_gen_sub_tl);
}

static bool trans_simt_and_32(DisasContext *ctx, arg_simt_and_32 *a)
{
    return gen_simt_arith(ctx, a, INSTR_TYPE_LU, tcg_gen_and_tl);
}

static bool trans_simt_or_32(DisasContext *ctx, arg_simt_or_32 *a)
{
    return gen_simt_arith(ctx, a, INSTR_TYPE_LU, tcg_gen_or_tl);
}

static bool trans_simt_xor_32(DisasContext *ctx, arg_simt_xor_32 *a)
{
    return gen_simt_arith(ctx, a, INSTR_TYPE_LU, tcg_gen_xor_tl);
}

static bool gen_scalar_shift(DisasContext *ctx, arg_simt_shift *a,
                           void (*func)(TCGv, TCGv, TCGv))
{
    TCGv dest, src_l, src_r, lo;
    src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, 0);
    dest = temp_new(ctx);
    lo = tcg_temp_new();

    tcg_gen_andi_tl(lo, src_r, 0x3f);
    func(dest, src_l, lo);
    tcg_temp_free(lo);

    simt_scalar_set_dst(ctx, a->RegDst, dest);

    return true;
}

static bool gen_simt_shift(DisasContext *ctx, arg_simt_shift *a,
                           void (*func)(TCGv, TCGv, TCGv))
{
    if (!is_vec_reg(a->RegDst)) {
        return gen_scalar_shift(ctx, a, func);
    }
    /* SIMT Zero Extension */
    a->SrcL &= 0x1FF;

    SIMT_WRAP_BEGIN

    TCGv dest, src_l, src_r, lo;
    src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    lo = tcg_temp_new();

    tcg_gen_andi_tl(lo, src_r, 0x3f);
    func(dest, src_l, lo);
    tcg_temp_free(lo);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_srl_32(DisasContext *ctx, arg_simt_srl_32 *a)
{
    return gen_simt_shift(ctx, a, tcg_gen_shr_tl);
}

static bool trans_simt_sra_32(DisasContext *ctx, arg_simt_sra_32 *a)
{
    return gen_simt_shift(ctx, a, tcg_gen_sar_tl);
}

static bool trans_simt_sll_32(DisasContext *ctx, arg_simt_sll_32 *a)
{
    return gen_simt_shift(ctx, a, tcg_gen_shl_tl);
}

static inline bool gen_scalar_arith_imm(DisasContext *ctx, arg_simt_arith_i *a,
                                      void (*func)(TCGv, TCGv, target_long))
{
    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    TCGv dest = temp_new(ctx);

    func(dest, src_l, a->imm);

    simt_scalar_set_dst(ctx, a->RegDst, dest);
    return true;
}

static inline bool gen_simt_arith_imm(DisasContext *ctx, arg_simt_arith_i *a,
                                      void (*func)(TCGv, TCGv, target_long))
{
    if (is_vec_reg(a->RegDst)) {
        SIMT_WRAP_BEGIN

        TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
        TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

        func(dest, src_l, a->imm);

        SIMT_WRAP_END_DEST_EXT(a->RegDst)
    } else {
        return gen_scalar_arith_imm(ctx, a, func);
    }
    return true;
}

static bool trans_simt_addli_32(DisasContext *ctx, arg_simt_addi_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_addi_tl(dest, src_l, (uint64_t)(a->imm & 0xffffffff));

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_simt_addi_32(DisasContext *ctx, arg_simt_addi_32 *a)
{
    return gen_simt_arith_imm(ctx, a, tcg_gen_addi_tl);
}

static bool trans_simt_subi_32(DisasContext *ctx, arg_simt_subi_32 *a)
{
    return gen_simt_arith_imm(ctx, a, tcg_gen_subi_tl);
}

static bool trans_simt_andi_32(DisasContext *ctx, arg_simt_andi_32 *a)
{
    return gen_simt_arith_imm(ctx, a, tcg_gen_andi_tl);
}

static bool trans_simt_ori_32(DisasContext *ctx, arg_simt_ori_32 *a)
{
    return gen_simt_arith_imm(ctx, a, tcg_gen_ori_tl);
}

static bool trans_simt_xori_32(DisasContext *ctx, arg_simt_xori_32 *a)
{
    return gen_simt_arith_imm(ctx, a, tcg_gen_xori_tl);
}

static inline bool gen_simt_shift_imm(DisasContext *ctx, arg_simt_shift_i *a,
                                      void (*func)(TCGv, TCGv, target_long))
{
    if (is_vec_reg(a->RegDst)) {
        /* SIMT Zero Extension */
        a->SrcL &= 0x1FF;

        SIMT_WRAP_BEGIN

        TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
        TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

        func(dest, src_l, a->shamt);

        SIMT_WRAP_END_DEST_EXT(a->RegDst)
    } else {
        TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
        TCGv dest = temp_new(ctx);

        func(dest, src_l, a->shamt);

        switch (extract16(a->RegDst, 0, 7)) {
        case DST_FVEC_T:
            set_dst_regx(ctx, dest, TARGET_REG_T);
            break;
        case DST_FVEC_U:
            set_dst_regx(ctx, dest, TARGET_REG_U);
            break;
        default:
            assert(0);
        }
    }
    return true;
}

static bool trans_simt_srli_32(DisasContext *ctx, arg_simt_srli_32 *a)
{
    return gen_simt_shift_imm(ctx, a, tcg_gen_shri_tl);
}

static bool trans_simt_srai_32(DisasContext *ctx, arg_simt_srai_32 *a)
{
    return gen_simt_shift_imm(ctx, a, tcg_gen_sari_tl);
}

static bool trans_simt_slli_32(DisasContext *ctx, arg_simt_slli_32 *a)
{
    return gen_simt_shift_imm(ctx, a, tcg_gen_shli_tl);
}

static void gen_scalar_cmp(DisasContext *ctx, arg_simt_shift *a,
                         TCGCond cond)
{
    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, 0);
    TCGv dest = temp_new(ctx);
    tcg_gen_setcond_tl(cond, dest, src1, src2);
    simt_scalar_set_dst(ctx, a->RegDst, dest);
}

static bool gen_simt_cmp(DisasContext *ctx, arg_simt_shift *a,
                         TCGCond cond)
{
    ctx->instr_attr |= CTX_INSTR_ATTR_CMP_MASK;
    if (is_vec_reg(a->RegDst) ||
        extract16(a->RegDst, 0, 7) == DST_FVEC_PRED) {
        SIMT_WRAP_BEGIN
        TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
        TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
        TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
        tcg_gen_setcond_tl(cond, dest, src1, src2);
        SIMT_WRAP_END_DEST_EXT(a->RegDst)
    } else {
        gen_scalar_cmp(ctx, a, cond);
    }
    return true;
}

static bool trans_simt_cmp_eq_32(DisasContext *ctx, arg_simt_cmp_eq_32 *a)
{
    return gen_simt_cmp(ctx, a, TCG_COND_EQ);
}

static bool trans_simt_cmp_ne_32(DisasContext *ctx, arg_simt_cmp_ne_32 *a)
{
    return gen_simt_cmp(ctx, a, TCG_COND_NE);
}

static bool trans_simt_cmp_lt_32(DisasContext *ctx, arg_simt_cmp_lt_32 *a)
{
    return gen_simt_cmp(ctx, a, TCG_COND_LT);
}

static bool trans_simt_cmp_ge_32(DisasContext *ctx, arg_simt_cmp_ge_32 *a)
{
    return gen_simt_cmp(ctx, a, TCG_COND_GE);
}

static bool trans_simt_cmp_ltu_32(DisasContext *ctx, arg_simt_cmp_ltu_32 *a)
{
    return gen_simt_cmp(ctx, a, TCG_COND_LTU);
}

static bool trans_simt_cmp_geu_32(DisasContext *ctx, arg_simt_cmp_geu_32 *a)
{
    return gen_simt_cmp(ctx, a, TCG_COND_GEU);
}

static void gen_scalar_cmp_arith(DisasContext *ctx, arg_simt_shift *a,
                               void (*func)(TCGv, TCGv, TCGv))
{
    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, 0);
    TCGv dest = temp_new(ctx);

    func(dest, src_l, src_r);

    tcg_gen_setcondi_tl(TCG_COND_NE, dest, dest, 0);

    simt_scalar_set_dst(ctx, a->RegDst, dest);
}

static bool gen_simt_cmp_arith(DisasContext *ctx, arg_simt_shift *a,
                               void (*func)(TCGv, TCGv, TCGv))
{
    ctx->instr_attr |= CTX_INSTR_ATTR_CMP_MASK;
    if (is_vec_reg(a->RegDst) ||
        extract16(a->RegDst, 0, 7) == DST_FVEC_PRED) {
        SIMT_WRAP_BEGIN

        TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
        TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
        TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

        func(dest, src_l, src_r);

        tcg_gen_setcondi_tl(TCG_COND_NE, dest, dest, 0);

        SIMT_WRAP_END_DEST_EXT(a->RegDst)

    } else {
        gen_scalar_cmp_arith(ctx, a, func);
    }
    return true;
}

static bool trans_simt_cmp_and_32(DisasContext *ctx, arg_simt_cmp_and_32 *a)
{
    return gen_simt_cmp_arith(ctx, a, tcg_gen_and_tl);
}

static bool trans_simt_cmp_or_32(DisasContext *ctx, arg_simt_cmp_or_32 *a)
{
    return gen_simt_cmp_arith(ctx, a, tcg_gen_or_tl);
}

static bool gen_scalar_cmpi(DisasContext *ctx,
    arg_simt_arith_i *a, TCGCond cond)
{
    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    TCGv src2 = tcg_constant_tl(a->imm);
    TCGv dest = temp_new(ctx);

    tcg_gen_setcond_tl(cond, dest, src1, src2);
    simt_scalar_set_dst(ctx, a->RegDst, dest);
    return true;
}

static bool gen_simt_cmpi(DisasContext *ctx, arg_simt_arith_i *a, TCGCond cond)
{
    ctx->instr_attr |= CTX_INSTR_ATTR_CMP_MASK;
    if (is_vec_reg(a->RegDst) ||
        extract16(a->RegDst, 0, 7) == DST_FVEC_PRED) {
        SIMT_WRAP_BEGIN

        TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
        TCGv src2 = tcg_constant_tl(a->imm);
        TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

        tcg_gen_setcond_tl(cond, dest, src1, src2);

        SIMT_WRAP_END_DEST_EXT(a->RegDst)
    } else {
        gen_scalar_cmpi(ctx, a, cond);
    }
    return true;
}

static bool trans_simt_cmp_eqi_32(DisasContext *ctx, arg_simt_cmp_eqi_32 *a)
{
    return gen_simt_cmpi(ctx, a, TCG_COND_EQ);
}

static bool trans_simt_cmp_nei_32(DisasContext *ctx, arg_simt_cmp_nei_32 *a)
{
    return gen_simt_cmpi(ctx, a, TCG_COND_NE);
}

static bool trans_simt_cmp_lti_32(DisasContext *ctx, arg_simt_cmp_lti_32 *a)
{
    return gen_simt_cmpi(ctx, a, TCG_COND_LT);
}

static bool trans_simt_cmp_gei_32(DisasContext *ctx, arg_simt_cmp_gei_32 *a)
{
    return gen_simt_cmpi(ctx, a, TCG_COND_GE);
}

static bool trans_simt_cmp_ltui_32(DisasContext *ctx, arg_simt_cmp_ltui_32 *a)
{
    return gen_simt_cmpi(ctx, a, TCG_COND_LTU);
}

static bool trans_simt_cmp_geui_32(DisasContext *ctx, arg_simt_cmp_geui_32 *a)
{
    return gen_simt_cmpi(ctx, a, TCG_COND_GEU);
}

static void gen_scalar_cmp_arith_i(DisasContext *ctx, arg_simt_arith_i *a,
                                 void (*func)(TCGv, TCGv, target_long))
{

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    TCGv dest = temp_new(ctx);

    func(dest, src1, a->imm);

    tcg_gen_setcondi_tl(TCG_COND_NE, dest, dest, 0);
    simt_scalar_set_dst(ctx, a->RegDst, dest);
}

static bool gen_simt_cmp_arith_i(DisasContext *ctx, arg_simt_arith_i *a,
                                 void (*func)(TCGv, TCGv, target_long))
{
    ctx->instr_attr |= CTX_INSTR_ATTR_CMP_MASK;
    if (is_vec_reg(a->RegDst) ||
        extract16(a->RegDst, 0, 7) == DST_FVEC_PRED) {
        SIMT_WRAP_BEGIN

        TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
        TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

        func(dest, src1, a->imm);

        tcg_gen_setcondi_tl(TCG_COND_NE, dest, dest, 0);

        SIMT_WRAP_END_DEST_EXT(a->RegDst)
    } else {
        gen_scalar_cmp_arith_i(ctx, a, func);
    }
    return true;
}

static bool trans_simt_cmp_andi_32(DisasContext *ctx, arg_simt_cmp_andi_32 *a)
{
    return gen_simt_cmp_arith_i(ctx, a, tcg_gen_andi_tl);
}

static bool trans_simt_cmp_ori_32(DisasContext *ctx, arg_simt_cmp_ori_32 *a)
{
    return gen_simt_cmp_arith_i(ctx, a, tcg_gen_ori_tl);
}

static bool trans_simt_bxs_32(DisasContext *ctx, arg_simt_bxs_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv tmp = tcg_temp_new();

    int xlen = extract16(a->SrcL, 7, 2);
    int width = GET_WIDTH(xlen);
    tcg_gen_rotri(tmp, src1, a->M, width);
    tcg_gen_sextract_tl(dest, tmp, 0, a->N + 1);
    tcg_temp_free(tmp);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_bxu_32(DisasContext *ctx, arg_simt_bxu_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv tmp = tcg_temp_new();

    int xlen = extract16(a->SrcL, 7, 2);
    int width = GET_WIDTH(xlen);

    tcg_gen_rotri(tmp, src1, a->M, width);
    tcg_gen_extract_tl(dest, tmp, 0, a->N + 1);
    tcg_temp_free(tmp);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_bic_32(DisasContext *ctx, arg_simt_bic_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv tmp = tcg_temp_new();
    int xlen = extract16(a->SrcL, 7, 2);
    int width = GET_WIDTH(xlen);
    width--;

    /* 生成第M位后N位连续为0，其他位全为1的掩码 */
    if (a->N < width) {
        TCGv mask0 = tcg_const_tl(~(((1ULL << (a->N + 1)) - 1) << a->M));
        tcg_gen_and_tl(tmp, src1, mask0);
        tcg_temp_free(mask0);
        if (a->N + a->M - width > 0) {
            TCGv mask1 =
                tcg_const_tl(~(((1ULL << (a->N + (a->M - width))) - 1)));
            tcg_gen_and_tl(tmp, tmp, mask1);
            tcg_temp_free(mask1);
        }
        tcg_gen_mov_tl(dest, tmp);
    } else {
        TCGv mask2 = tcg_const_tl(0ULL);
        tcg_gen_and_tl(tmp, src1, mask2);
        tcg_gen_mov_tl(dest, tmp);
        tcg_temp_free(mask2);
    }
    tcg_temp_free(tmp);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_bis_32(DisasContext *ctx, arg_simt_bis_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv tmp = tcg_temp_new();
    int xlen = extract16(a->SrcL, 7, 2);
    int width = GET_WIDTH(xlen);
    width--;

    /* 生成第M位后N位连续为1，其他位全为0的掩码 */
    if (a->N < width) {
        TCGv mask0 = tcg_const_tl(((1ULL << (a->N + 1)) - 1) << a->M);
        tcg_gen_or_tl(tmp, src1, mask0);
        tcg_temp_free(mask0);
        if (a->N + a->M - width > 0) {
            TCGv mask1 =
                tcg_const_tl((((1ULL << (a->N + (a->M - width))) - 1)));
            tcg_gen_or_tl(tmp, tmp, mask1);
            tcg_temp_free(mask1);
        }
        tcg_gen_mov_tl(dest, tmp);
    } else {
        TCGv mask2 = tcg_const_tl(0xffffffffffffffff);
        tcg_gen_or_tl(tmp, src1, mask2);
        tcg_gen_mov_tl(dest, tmp);
        tcg_temp_free(mask2);
    }
    tcg_temp_free(tmp);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_ctz_32(DisasContext *ctx, arg_simt_ctz_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    int xlen = extract16(a->SrcL, 7, 2);
    uint8_t M = a->M, N = a->N + 1;

    int width = GET_WIDTH(xlen);
    uint64_t mask = (N >= width) ? 0ULL : ~((1ULL << N) - 1);
    TCGv tmp = tcg_temp_new();
    tcg_gen_mov_tl(tmp, src1);

    if (M + N > width) {
        tcg_gen_rotri(tmp, tmp, M, width);
    } else {
        tcg_gen_shri_tl(tmp, tmp, M);
    }
    tcg_gen_ori_tl(tmp, tmp, mask);
    tcg_gen_ctzi_tl(dest, tmp, width);

    tcg_temp_free(tmp);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool trans_simt_clz_32(DisasContext *ctx, arg_simt_clz_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    uint8_t M = a->M, N = a->N + 1;
    int xlen = extract16(a->SrcL, 7, 2);

    uint8_t shift = TARGET_LONG_BITS - N;
    int width = GET_WIDTH(xlen);
    uint64_t mask = (N >= width) ? 0ULL : ((1ULL << shift) - 1);
    TCGv tmp = tcg_temp_new();
    tcg_gen_mov_tl(tmp, src1);

    if (M + N > width) {
        tcg_gen_rotri(tmp, tmp, M, width);
    } else {
        tcg_gen_shri_tl(tmp, tmp, M);
    }
    tcg_gen_shli_tl(tmp, tmp, shift);
    tcg_gen_ori_tl(tmp, tmp, mask);
    tcg_gen_clzi_tl(dest, tmp, width);

    tcg_temp_free(tmp);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool trans_simt_bcnt_32(DisasContext *ctx, arg_simt_bcnt_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    uint8_t M = a->M, N = a->N + 1;
    int xlen = extract16(a->SrcL, 7, 2);

    int width = GET_WIDTH(xlen);
    uint64_t mask = (N >= width) ? ~0ULL : ((1ULL << N) - 1);

    TCGv tmp = tcg_temp_new();
    tcg_gen_mov_tl(tmp, src1);

    if (M + N > width) {
        tcg_gen_rotri(tmp, tmp, M, width);
    } else {
        tcg_gen_shri_tl(tmp, tmp, M);
    }

    tcg_gen_andi_tl(tmp, tmp, mask);
    tcg_gen_ctpop_tl(dest, tmp);

    tcg_temp_free(tmp);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool trans_simt_rev_32(DisasContext *ctx, arg_simt_rev_32 *a)
{
    uint32_t M = a->M + 1, N = a->N;

    if (N > M) {
        gen_exception_illegal(ctx);
        return false;
    }

    SIMT_WRAP_BEGIN

    /* expect: abcdefgh => badcfehg */
    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    gen_helper_reverse_bits(dest, src1, tcg_constant_i32(M),
                            tcg_constant_i32(N));

    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool gen_simt_mul(DisasContext *ctx, arg_simt_shift *a)
{
    if (!is_vec_reg(a->RegDst)) {
        TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
        TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, 0);
        TCGv dest = temp_new(ctx);
        TCGv h64 = temp_new(ctx);

        if (extract16(a->SrcL, 9, 1)) {
            tcg_gen_muls2_tl(dest, h64, src1, src2);
        } else {
            tcg_gen_mulu2_tl(dest, h64, src1, src2);
        }
        simt_scalar_set_dst(ctx, a->RegDst, dest);
        return true;
    }

    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv h64 = temp_new(ctx);

    if (extract16(a->SrcL, 9, 1)) {
        tcg_gen_muls2_tl(dest, h64, src1, src2);
    } else {
        tcg_gen_mulu2_tl(dest, h64, src1, src2);
    }
    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool trans_simt_mul_32(DisasContext *ctx, arg_simt_mul_32 *a)
{
    return gen_simt_mul(ctx, a);
}

static bool trans_simt_madd_32(DisasContext *ctx, arg_simt_madd_32 *a)
{
    if (!is_vec_reg(a->RegDst)) {
        TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
        TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, 0);
        TCGv src3 = get_src_regx_fvec_with_width(ctx, a->SrcD, 0);
        TCGv dest = temp_new(ctx);

        TCGv res = tcg_temp_new();
        tcg_gen_mul_tl(res, src1, src2);
        tcg_gen_add_tl(dest, src3, res);
        tcg_temp_free(res);
        simt_scalar_set_dst(ctx, a->RegDst, dest);
        return true;
    }
    SIMT_WRAP_BEGIN
    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv src3 = get_src_regx_fvec_with_width(ctx, a->SrcD, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    TCGv res = tcg_temp_new();
    tcg_gen_mul_tl(res, src1, src2);
    tcg_gen_add_tl(dest, src3, res);
    tcg_temp_free(res);
    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

#ifndef PRERELEASE
static void handle_simt_branch(DisasContext *ctx, TCGv lane_direction_mask,
                               int br_offset, int rc_offset)
{
    target_ulong rpc = ctx->base.pc_next + rc_offset;
    target_ulong taken_pc = ctx->base.pc_next + br_offset;
    target_ulong not_taken_pc = ctx->pc_succ_insn;

    TCGLabel *branch_taken = gen_new_label();
    TCGLabel *not_taken = gen_new_label();
    TCGLabel *diverge_label = gen_new_label();

    /* If lane_direction_mask is equal to predm or 0, there is no diverge */
    TCGv tmp1 = tcg_temp_new(), tmp2 = tcg_temp_new();
    tcg_gen_setcond_tl(TCG_COND_EQ, tmp1, lane_direction_mask, predm);
    tcg_gen_setcondi_tl(TCG_COND_EQ, tmp2, lane_direction_mask, 0);
    tcg_gen_or_tl(tmp1, tmp1, tmp2);
    tcg_gen_brcondi_tl(TCG_COND_EQ, tmp1, 0, diverge_label);
    /* un-diverge */
    /* If lane_direction_mask is 0, all lane are not taken */
    tcg_gen_brcondi_tl(TCG_COND_EQ, lane_direction_mask, 0, not_taken);
    tcg_gen_br(branch_taken);

    gen_set_label(diverge_label);
    /* diverge */
    /* If branch diverge exists, the not-taken branch is used first. */
    gen_helper_simt_push_cstk(cpu_env, tcg_constant_tl(rpc),
                              tcg_constant_tl(rpc), predm);
    tcg_gen_andc_tl(predm, predm, lane_direction_mask);
    gen_helper_simt_push_cstk(cpu_env, tcg_constant_tl(rpc),
                              tcg_constant_tl(taken_pc), lane_direction_mask);

    /*
     * TODO: Maybe we can invalidate the TB at the destination address before
     * jumping in the SIMT block. So we don't need to rotate the T/U/M/N
     * registers.
     */

    gen_set_label(not_taken);
    tcg_gen_movi_tl(cpu_pc, not_taken_pc);
    tcg_gen_lookup_and_goto_ptr();

    gen_set_label(branch_taken);
    tcg_gen_movi_tl(cpu_pc, taken_pc);
    tcg_gen_lookup_and_goto_ptr();
    tcg_temp_free(tmp1);
    tcg_temp_free(tmp2);
    tcg_temp_free(lane_direction_mask);

    ctx->base.is_jmp = DISAS_NORETURN;
}

static TCGv get_compare_lane_direction_mask(DisasContext *ctx, int SrcL,
                                            int SrcR, TCGCond cond)
{
    CPULINXState *env = ctx->cs->env_ptr;
    TCGv lane_direction_mask = tcg_temp_local_new();
    tcg_gen_movi_tl(lane_direction_mask, 0);
    /* Obtains the direction of each branch of a activate lane. */
    for (int lane_id = 0; lane_id < env->csr_lanenum; ++lane_id) {
        TCGLabel *disable_lane = gen_new_label();
        TCGv lane_exec_flag = tcg_temp_new();
        tcg_gen_andi_tl(lane_exec_flag, predm, 1 << lane_id);
        tcg_gen_brcondi_tl(TCG_COND_EQ, lane_exec_flag, 0, disable_lane);

        TCGv src1 = get_src_regx_fvec_with_width(ctx, SrcL, lane_id);
        TCGv src2 = get_src_regx_fvec_with_width(ctx, SrcR, lane_id);
        TCGv is_jump = tcg_temp_new();

        tcg_gen_setcond_tl(cond, is_jump, src1, src2);
        tcg_gen_shli_tl(is_jump, is_jump, lane_id);
        tcg_gen_or_tl(lane_direction_mask, lane_direction_mask, is_jump);

        gen_set_label(disable_lane);
        temp_free(ctx);
        tcg_temp_free(is_jump);
        tcg_temp_free(lane_exec_flag);
    }

    return lane_direction_mask;
}

static bool gen_simt_bcmp(DisasContext *ctx, int SrcL, int SrcR,
                          int br_offset, int rc_offset, TCGCond cond)
{
    TCGv lane_direction_mask = get_compare_lane_direction_mask(ctx, SrcL, SrcR,
                                                               cond);
    handle_simt_branch(ctx, lane_direction_mask, br_offset, rc_offset);
    return true;
}

static bool trans_simt_b_eq_32(DisasContext *ctx, arg_simt_b_eq_32 *a)
{
    return gen_simt_bcmp(ctx, a->SrcL, a->SrcR, a->br_offset, a->rc_offset,
                         TCG_COND_EQ);
}

static bool trans_simt_b_ne_32(DisasContext *ctx, arg_simt_b_ne_32 *a)
{
    return gen_simt_bcmp(ctx, a->SrcL, a->SrcR, a->br_offset, a->rc_offset,
                         TCG_COND_NE);
}

static bool trans_simt_b_lt_32(DisasContext *ctx, arg_simt_b_lt_32 *a)
{
    return gen_simt_bcmp(ctx, a->SrcL, a->SrcR, a->br_offset, a->rc_offset,
                         TCG_COND_LT);
}

static bool trans_simt_b_ge_32(DisasContext *ctx, arg_simt_b_ge_32 *a)
{
    return gen_simt_bcmp(ctx, a->SrcL, a->SrcR, a->br_offset, a->rc_offset,
                         TCG_COND_GE);
}

static bool trans_simt_b_ltu_32(DisasContext *ctx, arg_simt_b_ltu_32 *a)
{
    return gen_simt_bcmp(ctx, a->SrcL, a->SrcR, a->br_offset, a->rc_offset,
                         TCG_COND_LTU);
}

static bool trans_simt_b_geu_32(DisasContext *ctx, arg_simt_b_geu_32 *a)
{
    return gen_simt_bcmp(ctx, a->SrcL, a->SrcR, a->br_offset, a->rc_offset,
                         TCG_COND_GEU);
}

static bool trans_simt_jr_32(DisasContext *ctx, arg_simt_jr_32 *a)
{
    /*
     * FIXME: Assume that all lanes jump in the same direction.
     * Therefore, we only need to calculate the target address of the first
     * activated lane.
     */
    CPULINXState *env = ctx->cs->env_ptr;
    for (int lane_id = 0; lane_id < env->csr_lanenum; ++lane_id) {
        TCGLabel *inactive_lane = gen_new_label();
        TCGv lane_exec_flag = tcg_temp_new();
        tcg_gen_andi_tl(lane_exec_flag, predm, 1 << lane_id);
        tcg_gen_brcondi_tl(TCG_COND_EQ, lane_exec_flag, 0, inactive_lane);

        TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
        TCGv tmp = tcg_temp_local_new();
        TCGLabel *out_of_block = gen_new_label();

        tcg_gen_addi_tl(tmp, src1, a->imm);
        tcg_gen_brcond_tl(TCG_COND_LT, tmp, tpc1, out_of_block);

        tcg_gen_mov_tl(cpu_pc, tmp);
        tcg_temp_free(tmp);

        tcg_gen_lookup_and_goto_ptr();

        gen_set_label(out_of_block);
        gen_exception_illegal(ctx);

        tcg_temp_free(lane_exec_flag);
        /* get_src_regx_fvec_with_width maybe using temp_new(ctx) */
        temp_free(ctx);
        gen_set_label(inactive_lane);
    }

    /*
     * TODO: Maybe we can invalidate the TB at the destination address before
     * jumping in the SIMT block. So we don't need to rotate the T/U/M/N
     * registers.
     */

    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_simt_j_32(DisasContext *ctx, arg_simt_j_32 *a)
{
    CPULINXState *env = ctx->cs->env_ptr;
    target_ulong next_pc = ctx->base.pc_next + a->imm;

    if (next_pc < env->tpc1) {
        qemu_log("in-block jump is out of block. bpc: 0x%lx, tpc: 0x%lx,"
                 " next_pc: 0x%lx, body_start:0x%lx\n",
                 ctx->bpc, ctx->base.pc_next, next_pc, env->tpc1);
        gen_exception_illegal(ctx);
    }

    /*
     * TODO: Maybe we can invalidate the TB at the destination address before
     * jumping in the SIMT block. So we don't need to rotate the T/U/M/N
     * registers.
     */

    tcg_gen_movi_tl(cpu_pc, next_pc);
    tcg_gen_lookup_and_goto_ptr();

    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}
#endif

static TCGv gen_simt_load_32_op(DisasContext *ctx, MemOp memop, bool is_addr,
                                TCGv addr, int dest_reg_enc, int lane_id,
                                int SrcL)
{
    int src = extract16(SrcL, 0, 7);
    TCGv dest_Treg = tcg_temp_local_new();
    if (tb_cflags(ctx->base.tb) & CF_ATOMIC) {
        TCGv_i32 memop_t = tcg_constant_i32(memop);
        gen_helper_load_data(dest_Treg, cpu_env, addr, memop_t);
    } else {
        if (src == SRC_FVEC_TA || src == SRC_FVEC_TB ||
            src == SRC_FVEC_TC || src == SRC_FVEC_TD ||
            src == SRC_FVEC_TE || src == SRC_FVEC_TF ||
            src == SRC_FVEC_TG || src == SRC_FVEC_TH ||
            src == SRC_FVEC_TO || src == SRC_FVEC_TO1 ||
            src == SRC_FVEC_TO2 || src == SRC_FVEC_TO3) {
            TCGv_i32 memop_t = tcg_constant_i32(memop);
            gen_helper_tile_load(dest_Treg, addr, memop_t);
        } else {
            tcg_gen_qemu_ld_tl(dest_Treg, addr, ctx->mem_idx, memop);
        }
    }
    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        TCGv tmp = tcg_temp_new_i64();
        tcg_gen_mov_tl(tmp, addr);
        switch (src) {
        case SRC_FVEC_TA:
            tcg_gen_sub_tl(tmp, tmp, ta);
            break;
        case SRC_FVEC_TB:
            tcg_gen_sub_tl(tmp, tmp, tb);
            break;
        case SRC_FVEC_TC:
            tcg_gen_sub_tl(tmp, tmp, tc);
            break;
        case SRC_FVEC_TD:
            tcg_gen_sub_tl(tmp, tmp, td);
            break;
        case SRC_FVEC_TE:
            tcg_gen_sub_tl(tmp, tmp, te);
            break;
        case SRC_FVEC_TF:
            tcg_gen_sub_tl(tmp, tmp, tf);
            break;
        case SRC_FVEC_TG:
            tcg_gen_sub_tl(tmp, tmp, tg);
            break;
        case SRC_FVEC_TH:
            tcg_gen_sub_tl(tmp, tmp, th);
            break;
        case SRC_FVEC_TO:
            tcg_gen_sub_tl(tmp, tmp, to);
            break;
        case SRC_FVEC_TO1:
            tcg_gen_sub_tl(tmp, tmp, to1);
            break;
        case SRC_FVEC_TO2:
            tcg_gen_sub_tl(tmp, tmp, to2);
            break;
        case SRC_FVEC_TO3:
            tcg_gen_sub_tl(tmp, tmp, to3);
            break;
        default:
            break;
        }
        gen_helper_memprnt(tmp, dest_Treg, tcg_constant_tl(READ));
        tcg_temp_free_i64(tmp);
    }

    return is_addr ? addr : dest_Treg;

    if (is_addr) {
        set_dst_regx_fvec_with_width(ctx, addr, dest_reg_enc,
                                     lane_id);
    } else {
        set_dst_regx_fvec_with_width(ctx, dest_Treg, dest_reg_enc,
                                     lane_id);
    }
}

static void gen_scalar_load_32(DisasContext *ctx, MemOp memop, bool is_addr,
                             int shamt, int SrcR, int SrcL, int l,
                             int reg_dst, int scaled_lc0, bool has_lc0)
{
    TCGv addr_base = get_src_regx_fvec(ctx, SrcL, 0);
    TCGv SrcRreg = get_src_regx_fvec_with_width(ctx, SrcR, 0);

    TCGv shift = tcg_temp_new();
    tcg_gen_mov_tl(shift, SrcRreg);
    tcg_gen_shli_tl(shift, shift, shamt);

    /* shift becomes the "addr" */
    tcg_gen_add_tl(shift, addr_base, shift);
    shift = add_offset_lc0(shift, 0, scaled_lc0, has_lc0);

    TCGv dest = gen_simt_load_32_op(ctx, memop, is_addr, shift, reg_dst,
                        0, SrcL);
    tcg_temp_free(shift);
    if (l) {
        gen_helper_mem_check(cpu_env, addr_base);
    }
    simt_scalar_set_dst(ctx, reg_dst, dest);
}

static void gen_simt_load_32(DisasContext *ctx, MemOp memop, bool is_addr,
                             int shamt, int SrcR, int SrcL, int l,
                             int reg_dst, int scaled_lc0, bool has_lc0)
{
    if (!is_vec_reg(reg_dst)) {
        return gen_scalar_load_32(ctx, memop, is_addr, shamt,
            SrcR, SrcL, l, reg_dst, scaled_lc0, has_lc0);
    }

    SIMT_WRAP_BEGIN

    TCGv addr_base = get_src_regx_fvec(ctx, SrcL, lane_id);
    TCGv SrcRreg = get_src_regx_fvec_with_width(ctx, SrcR, lane_id);

    TCGv shift = tcg_temp_new();
    tcg_gen_mov_tl(shift, SrcRreg);
    tcg_gen_shli_tl(shift, shift, shamt);

    /* shift becomes the "addr" */
    tcg_gen_add_tl(shift, addr_base, shift);
    shift = add_offset_lc0(shift, lane_id, scaled_lc0, has_lc0);

    TCGv dest = gen_simt_load_32_op(ctx, memop, is_addr, shift, reg_dst,
                        lane_id, SrcL);
    tcg_temp_free(shift);
    if (l) {
        gen_helper_mem_check(cpu_env, addr_base);
    }
    SIMT_WRAP_END_DEST_EXT(reg_dst)
}

static void gen_scalar_load_32_imm(DisasContext *ctx, MemOp memop, bool is_addr,
                                int imm, int SrcL, int l, int reg_dst,
                                int scaled_shift, int scaled_lc0, bool has_lc0)
{
    /* get SrcLreg  addr_base */
    TCGv addr_base = get_src_regx_fvec(ctx, SrcL, 0);

    TCGv addr = tcg_temp_new();
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);
    addr = add_offset_lc0(addr, 0, scaled_lc0, has_lc0);

    TCGv dest = gen_simt_load_32_op(ctx, memop, is_addr, addr, reg_dst,
                        0, SrcL);
    if (l) {
        gen_helper_mem_check(cpu_env, addr);
    }
    tcg_temp_free(addr);

    simt_scalar_set_dst(ctx, reg_dst, dest);
}

static void gen_simt_load_32_imm(DisasContext *ctx, MemOp memop, bool is_addr,
                                int imm, int SrcL, int l, int reg_dst,
                                int scaled_shift, int scaled_lc0, bool has_lc0)
{

    if (!is_vec_reg(reg_dst)) {
        return gen_scalar_load_32_imm(ctx, memop, is_addr, imm,
            SrcL, l, reg_dst, scaled_shift, scaled_lc0, has_lc0);
    }
    SIMT_WRAP_BEGIN

    /* get SrcLreg  addr_base */
    TCGv addr_base = get_src_regx_fvec(ctx, SrcL, lane_id);

    TCGv addr = tcg_temp_new();
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);
    addr = add_offset_lc0(addr, lane_id, scaled_lc0, has_lc0);

    TCGv dest = gen_simt_load_32_op(ctx, memop, is_addr, addr, reg_dst,
                        lane_id, SrcL);
    if (l) {
        gen_helper_mem_check(cpu_env, addr);
    }
    tcg_temp_free(addr);

    SIMT_WRAP_END_DEST_EXT(reg_dst)
}

/* Load Register Offset */
static bool trans_simt_lb_32(DisasContext *ctx, arg_simt_lb_32 *a)
{
    gen_simt_load_32(ctx, MO_SB, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool trans_simt_lh_32(DisasContext *ctx, arg_simt_lh_32 *a)
{
    gen_simt_load_32(ctx, MO_TESW, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_lw_32(DisasContext *ctx, arg_simt_lw_32 *a)
{
    gen_simt_load_32(ctx, MO_TESL, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool trans_simt_ld_32(DisasContext *ctx, arg_simt_ld_32 *a)
{
    gen_simt_load_32(ctx, MO_TEQ, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool trans_simt_lbu_32(DisasContext *ctx, arg_simt_lbu_32 *a)
{
    gen_simt_load_32(ctx, MO_UB, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool trans_simt_lhu_32(DisasContext *ctx, arg_simt_lhu_32 *a)
{
    gen_simt_load_32(ctx, MO_TEUW, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_lwu_32(DisasContext *ctx, arg_simt_lwu_32 *a)
{
    gen_simt_load_32(ctx, MO_TEUL, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LW_SW, NO_LC0);
    return true;
}

/* Load Imm Offset */
static bool trans_simt_lb_i_32(DisasContext *ctx, arg_simt_lb_i_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LB_SB, OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool trans_simt_lh_i_32(DisasContext *ctx, arg_simt_lh_i_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LH_SH, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_lw_i_32(DisasContext *ctx, arg_simt_lw_i_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LW_SW, OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool trans_simt_ld_i_32(DisasContext *ctx, arg_simt_ld_i_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LD_SD, OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool trans_simt_lbu_i_32(DisasContext *ctx, arg_simt_lbu_i_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LB_SB, OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool trans_simt_lhu_i_32(DisasContext *ctx, arg_simt_lhu_i_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LH_SH, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_lwu_i_32(DisasContext *ctx, arg_simt_lwu_i_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LW_SW, OFFSET_LW_SW, NO_LC0);
    return true;
}

/* Load Imm UnScaled */
static bool trans_simt_lh_ui_32(DisasContext *ctx, arg_simt_lh_ui_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_lw_ui_32(DisasContext *ctx, arg_simt_lw_ui_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL, a->l,
                         a->RegDst, UNSCALED, OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool trans_simt_ld_ui_32(DisasContext *ctx, arg_simt_ld_ui_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool trans_simt_lhu_ui_32(DisasContext *ctx, arg_simt_lhu_ui_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_lwu_ui_32(DisasContext *ctx, arg_simt_lwu_ui_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LW_SW, NO_LC0);
    return true;
}

/* Load Register Offset with LC0 */
static bool trans_simt_lb_lc0_32(DisasContext *ctx, arg_simt_lb_lc0_32 *a)
{
    gen_simt_load_32(ctx, MO_SB, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LB_SB, HAS_LC0);
    return true;
}
static bool trans_simt_lh_lc0_32(DisasContext *ctx, arg_simt_lh_lc0_32 *a)
{
    gen_simt_load_32(ctx, MO_TESW, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_lw_lc0_32(DisasContext *ctx, arg_simt_lw_lc0_32 *a)
{
    gen_simt_load_32(ctx, MO_TESL, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LW_SW, HAS_LC0);
    return true;
}
static bool trans_simt_ld_lc0_32(DisasContext *ctx, arg_simt_ld_lc0_32 *a)
{
    gen_simt_load_32(ctx, MO_TEQ, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LD_SD, HAS_LC0);
    return true;
}
static bool trans_simt_lbu_lc0_32(DisasContext *ctx, arg_simt_lbu_lc0_32 *a)
{
    gen_simt_load_32(ctx, MO_UB, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LB_SB, HAS_LC0);
    return true;
}
static bool trans_simt_lhu_lc0_32(DisasContext *ctx, arg_simt_lhu_lc0_32 *a)
{
    gen_simt_load_32(ctx, MO_TEUW, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_lwu_lc0_32(DisasContext *ctx, arg_simt_lwu_lc0_32 *a)
{
    gen_simt_load_32(ctx, MO_TEUL, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LW_SW, HAS_LC0);
    return true;
}

/* Load Imm Offset with LC0 */
static bool trans_simt_lb_i_lc0_32(DisasContext *ctx, arg_simt_lb_i_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LB_SB, OFFSET_LB_SB, HAS_LC0);
    return true;
}
static bool trans_simt_lh_i_lc0_32(DisasContext *ctx, arg_simt_lh_i_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LH_SH, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_lw_i_lc0_32(DisasContext *ctx, arg_simt_lw_i_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LW_SW, OFFSET_LW_SW, HAS_LC0);
    return true;
}
static bool trans_simt_ld_i_lc0_32(DisasContext *ctx, arg_simt_ld_i_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LD_SD, OFFSET_LD_SD, HAS_LC0);
    return true;
}

static bool trans_simt_lbu_i_lc0_32(DisasContext *ctx, arg_simt_lbu_i_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LB_SB, OFFSET_LB_SB, HAS_LC0);
    return true;
}
static bool trans_simt_lhu_i_lc0_32(DisasContext *ctx, arg_simt_lhu_i_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LH_SH, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_lwu_i_lc0_32(DisasContext *ctx, arg_simt_lwu_i_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LW_SW, OFFSET_LW_SW, HAS_LC0);
    return true;
}

/* Load Imm UnScaled with LC0 */
static bool trans_simt_lh_ui_lc0_32(DisasContext *ctx, arg_simt_lh_ui_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_lw_ui_lc0_32(DisasContext *ctx, arg_simt_lw_ui_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL, a->l,
                         a->RegDst, UNSCALED, OFFSET_LW_SW, HAS_LC0);
    return true;
}
static bool trans_simt_ld_ui_lc0_32(DisasContext *ctx, arg_simt_ld_ui_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LD_SD, HAS_LC0);
    return true;
}

static bool trans_simt_lhu_ui_lc0_32(DisasContext *ctx,
                                     arg_simt_lhu_ui_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_lwu_ui_lc0_32(DisasContext *ctx,
                                     arg_simt_lwu_ui_lc0_32 *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LW_SW, HAS_LC0);
    return true;
}

static void gen_simt_store_32_op(DisasContext *ctx, MemOp memop,
                                 TCGv dest, TCGv data, int width, int label)
{
    int dst = extract16(label, 0, 7);
    int local = extract16(label, 15, 1);
    if (tb_cflags(ctx->base.tb) & CF_ATOMIC) {
        TCGv_i32 size = tcg_constant_i32(width);
        gen_helper_store_data(cpu_env, dest, data, size);
    } else {
        if (local) {
            TCGv_i32 size = tcg_constant_i32(width);
            gen_helper_tile_store(dest, data, size);
        } else {
            tcg_gen_qemu_st_tl(data, dest, ctx->mem_idx, memop);
        }
    }

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        TCGv tmp = tcg_temp_new_i64();
        tcg_gen_mov_tl(tmp, dest);
        switch (dst) {
        case SRC_FVEC_TO:
            tcg_gen_sub_tl(tmp, tmp, to);
            break;
        case SRC_FVEC_TO1:
            tcg_gen_sub_tl(tmp, tmp, to1);
            break;
        case SRC_FVEC_TO2:
            tcg_gen_sub_tl(tmp, tmp, to2);
            break;
        case SRC_FVEC_TO3:
            tcg_gen_sub_tl(tmp, tmp, to3);
            break;
        default:
            break;
        }
        gen_helper_memprnt(tmp, data, tcg_constant_tl(WRITE));
        tcg_temp_free_i64(tmp);
    }
}

static void gen_simt_store_32(DisasContext *ctx, MemOp memop,
                             int SrcD, int SrcR, int SrcL, int l,
                             int width, int scaled_shift,
                             int scaled_lc0, bool has_lc0)
{
    SIMT_WRAP_BEGIN

    TCGv addr_base = get_src_regx_fvec(ctx, SrcL, lane_id);
    TCGv data = get_src_regx_fvec_with_width(ctx, SrcD, lane_id);
    TCGv shift_base = get_src_regx_fvec_with_width(ctx, SrcR, lane_id);

    TCGv shift = tcg_temp_new();
    tcg_gen_mov_tl(shift, shift_base);
    tcg_gen_shli_tl(shift, shift, scaled_shift);

    /* shift become dest */
    tcg_gen_add_tl(shift, addr_base, shift);
    shift = add_offset_lc0(shift, lane_id, scaled_lc0, has_lc0);

    gen_simt_store_32_op(ctx, memop, shift, data, width,
                            ((SrcL & 0x3F) | (l << 15)));
    if (l) {
        gen_helper_mem_check(cpu_env, shift);
    }
    tcg_temp_free(shift);

    SIMT_WRAP_END
}

static void gen_simt_store_32_imm(DisasContext *ctx, MemOp memop,
                                 int shift, int SrcR, int SrcL, int l,
                                 int width, int scaled_shift,
                                 int scaled_lc0, bool has_lc0)
{
    SIMT_WRAP_BEGIN

    TCGv addr_base = get_src_regx_fvec(ctx, SrcR, lane_id);
    TCGv data = get_src_regx_fvec_with_width(ctx, SrcL, lane_id);
    TCGv dest = tcg_temp_new();

    tcg_gen_addi_tl(dest, addr_base, shift << scaled_shift);
    dest = add_offset_lc0(dest, lane_id, scaled_lc0, has_lc0);

    gen_simt_store_32_op(ctx, memop, dest, data, width,
                            ((SrcL & 0x3F) | (l << 15)));
    if (l) {
        gen_helper_mem_check(cpu_env, dest);
    }
    tcg_temp_free(dest);
    SIMT_WRAP_END
}

/* Store Regisger Offset */
static bool trans_simt_sb_32(DisasContext *ctx, arg_simt_sb_32 *a)
{
    gen_simt_store_32(ctx, MO_SB, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LB_SB, OFFSET_LB_SB + a->shamt,
                     OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool trans_simt_sh_32(DisasContext *ctx, arg_simt_sh_32 *a)
{
    gen_simt_store_32(ctx, MO_TESW, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LH_SH, OFFSET_LH_SH + a->shamt,
                     OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_sw_32(DisasContext *ctx, arg_simt_sw_32 *a)
{
    gen_simt_store_32(ctx, MO_TESL, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LW_SW, OFFSET_LW_SW + a->shamt,
                     OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool trans_simt_sd_32(DisasContext *ctx, arg_simt_sd_32 *a)
{
    gen_simt_store_32(ctx, MO_TEQ, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LD_SD, OFFSET_LD_SD + a->shamt,
                     OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool trans_simt_sh_u_32(DisasContext *ctx, arg_simt_sh_u_32 *a)
{
    gen_simt_store_32(ctx, MO_TESW, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LH_SH, a->shamt,
                     OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_sw_u_32(DisasContext *ctx, arg_simt_sw_u_32 *a)
{
    gen_simt_store_32(ctx, MO_TESL, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LW_SW, a->shamt,
                     OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool trans_simt_sd_u_32(DisasContext *ctx, arg_simt_sd_u_32 *a)
{
    gen_simt_store_32(ctx, MO_TEQ, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LD_SD, a->shamt,
                     OFFSET_LD_SD, NO_LC0);
    return true;
}

/* Store Imm Offset */
static bool trans_simt_sb_i_32(DisasContext *ctx, arg_simt_sb_i_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_SB, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LB_SB, OFFSET_LB_SB, OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool trans_simt_sh_i_32(DisasContext *ctx, arg_simt_sh_i_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LH_SH, OFFSET_LH_SH, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_sw_i_32(DisasContext *ctx, arg_simt_sw_i_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LW_SW, OFFSET_LW_SW, OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool trans_simt_sd_i_32(DisasContext *ctx, arg_simt_sd_i_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LD_SD, OFFSET_LD_SD, OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool trans_simt_sh_ui_32(DisasContext *ctx, arg_simt_sh_ui_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LH_SH, UNSCALED, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool trans_simt_sw_ui_32(DisasContext *ctx, arg_simt_sw_ui_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LW_SW, UNSCALED, OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool trans_simt_sd_ui_32(DisasContext *ctx, arg_simt_sd_ui_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LD_SD, UNSCALED, OFFSET_LD_SD, NO_LC0);
    return true;
}

/* Store Regisger Offset with LC0 */
static bool trans_simt_sb_lc0_32(DisasContext *ctx, arg_simt_sb_lc0_32 *a)
{
    gen_simt_store_32(ctx, MO_SB, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LB_SB, OFFSET_LB_SB + a->shamt,
                     OFFSET_LB_SB, HAS_LC0);
    return true;
}
static bool trans_simt_sh_lc0_32(DisasContext *ctx, arg_simt_sh_lc0_32 *a)
{
    gen_simt_store_32(ctx, MO_TESW, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LH_SH, OFFSET_LH_SH + a->shamt,
                     OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_sw_lc0_32(DisasContext *ctx, arg_simt_sw_lc0_32 *a)
{
    gen_simt_store_32(ctx, MO_TESL, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LW_SW, OFFSET_LW_SW + a->shamt,
                     OFFSET_LW_SW, HAS_LC0);
    return true;
}
static bool trans_simt_sd_lc0_32(DisasContext *ctx, arg_simt_sd_lc0_32 *a)
{
    gen_simt_store_32(ctx, MO_TEQ, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LD_SD, OFFSET_LD_SD + a->shamt,
                     OFFSET_LD_SD, HAS_LC0);
    return true;
}

static bool trans_simt_sh_u_lc0_32(DisasContext *ctx, arg_simt_sh_u_lc0_32 *a)
{
    gen_simt_store_32(ctx, MO_TESW, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LH_SH, a->shamt,
                     OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_sw_u_lc0_32(DisasContext *ctx, arg_simt_sw_u_lc0_32 *a)
{
    gen_simt_store_32(ctx, MO_TESL, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LW_SW, a->shamt,
                     OFFSET_LW_SW, HAS_LC0);
    return true;
}
static bool trans_simt_sd_u_lc0_32(DisasContext *ctx, arg_simt_sd_u_lc0_32 *a)
{
    gen_simt_store_32(ctx, MO_TEQ, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LD_SD, a->shamt,
                     OFFSET_LD_SD, HAS_LC0);
    return true;
}

/* Store Imm Offset with LC0 */
static bool trans_simt_sb_i_lc0_32(DisasContext *ctx, arg_simt_sb_i_lc0_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_SB, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LB_SB, OFFSET_LB_SB, OFFSET_LB_SB, HAS_LC0);
    return true;
}
static bool trans_simt_sh_i_lc0_32(DisasContext *ctx, arg_simt_sh_i_lc0_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LH_SH, OFFSET_LH_SH, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_sw_i_lc0_32(DisasContext *ctx, arg_simt_sw_i_lc0_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LW_SW, OFFSET_LW_SW, OFFSET_LW_SW, HAS_LC0);
    return true;
}
static bool trans_simt_sd_i_lc0_32(DisasContext *ctx, arg_simt_sd_i_lc0_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LD_SD, OFFSET_LD_SD, OFFSET_LD_SD, HAS_LC0);
    return true;
}

static bool trans_simt_sh_ui_lc0_32(DisasContext *ctx, arg_simt_sh_ui_lc0_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LH_SH, UNSCALED, OFFSET_LH_SH, HAS_LC0);
    return true;
}
static bool trans_simt_sw_ui_lc0_32(DisasContext *ctx, arg_simt_sw_ui_lc0_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LW_SW, UNSCALED, OFFSET_LW_SW, HAS_LC0);
    return true;
}
static bool trans_simt_sd_ui_lc0_32(DisasContext *ctx, arg_simt_sd_ui_lc0_32 *a)
{
    gen_simt_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LD_SD, UNSCALED, OFFSET_LD_SD, HAS_LC0);
    return true;
}

static bool trans_simt_div_32(DisasContext *ctx, arg_simt_div_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    if (extract16(a->SrcL, 9, 1)) {
        gen_blk_div(dest, src1, src2);
    } else {
        gen_blk_divu(dest, src1, src2);
    }

    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool trans_simt_rem_32(DisasContext *ctx, arg_simt_rem_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    if (extract16(a->SrcL, 9, 1)) {
        gen_blk_rem(dest, src1, src2);
    } else {
        gen_blk_remu(dest, src1, src2);
    }
    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool trans_scala_fadd_32(DisasContext *ctx, arg_simt_fadd_32 *a)
{
    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, 0);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, 0);
    TCGv dest = temp_new(ctx);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fadd_b(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fadd_h(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fadd_s(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fadd_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }
    simt_scalar_set_dst(ctx, a->RegDst, dest);
    return true;
}


static bool trans_simt_fadd_32(DisasContext *ctx, arg_simt_fadd_32 *a)
{
    if (!is_vec_reg(a->RegDst)) {
        return trans_scala_fadd_32(ctx, a);
    }
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv vlen = tcg_constant_i64(a->vlen);
    TCGv rm = tcg_constant_i64(get_real_round_mode(a->rm));
    TCGv sat = tcg_constant_i64(a->sat);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fadd_b(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fadd_h(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fadd_s(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fadd_d(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fadd_xn(dest, cpu_env, src_l, src_r, vlen, sat, rm);
        break;
    default:
        g_assert_not_reached();
    }
    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fsub_32(DisasContext *ctx, arg_simt_fsub_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv vlen = tcg_constant_i64(a->vlen);
    TCGv rm = tcg_constant_i64(get_real_round_mode(a->rm));
    TCGv sat = tcg_constant_i64(a->sat);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fsub_b(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fsub_h(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fsub_s(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fsub_d(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fsub_xn(dest, cpu_env, src_l, src_r, vlen, sat, rm);
        break;
    default:
        g_assert_not_reached();
    }
    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fmul_32(DisasContext *ctx, arg_simt_fmul_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv vlen = tcg_constant_i64(a->vlen);
    TCGv rm = tcg_constant_i64(get_real_round_mode(a->rm));
    TCGv sat = tcg_constant_i64(a->sat);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fmul_b(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fmul_h(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fmul_s(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fmul_d(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fmul_xn(dest, cpu_env, src_l, src_r, vlen, sat, rm);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fdiv_32(DisasContext *ctx, arg_simt_fdiv_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv vlen = tcg_constant_i64(a->vlen);
    TCGv rm = tcg_constant_i64(get_real_round_mode(a->rm));
    TCGv sat = tcg_constant_i64(a->sat);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fdiv_b(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fdiv_h(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fdiv_s(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fdiv_d(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fdiv_xn(dest, cpu_env, src_l, src_r, vlen, sat, rm);
        break;
    default:
        g_assert_not_reached();
    }
    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fmadd_32(DisasContext *ctx, arg_simt_fmadd_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv src_t = get_src_regx_fvec(ctx, a->SrcA, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fmadd_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fmadd_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fmadd_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fmadd_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fmadd_bf(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fmsub_32(DisasContext *ctx, arg_simt_fmsub_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv src_t = get_src_regx_fvec(ctx, a->SrcA, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fmsub_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fmsub_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fmsub_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fmsub_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fmsub_bf(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }
    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fnmadd_32(DisasContext *ctx, arg_simt_fnmadd_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv src_t = get_src_regx_fvec(ctx, a->SrcA, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fnmadd_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fnmadd_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fnmadd_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fnmadd_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fnmadd_bf(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }
    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fnmsub_32(DisasContext *ctx, arg_simt_fnmsub_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv src_t = get_src_regx_fvec(ctx, a->SrcA, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fnmsub_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fnmsub_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fnmsub_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fnmsub_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fnmsub_bf(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }
    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static TCGv gen_simt_fp_eq(DisasContext *ctx, int srcl_code, int srcr_code,
                           bool is_eq, int lane_id)
{
    TCGv src_l = get_src_regx_fvec(ctx, srcl_code, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, srcr_code, lane_id);
    TCGv tmp = tcg_temp_local_new();
    switch (extract16(srcl_code, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_feq_b(tmp, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_feq_h(tmp, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_feq_s(tmp, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_feq_d(tmp, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    if (!is_eq) {
        tcg_gen_not_tl(tmp, tmp);
        tcg_gen_andi_tl(tmp, tmp, 1);
    }

    return tmp;
}

static bool trans_simt_feq_32(DisasContext *ctx, arg_simt_feq_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv dest = gen_simt_fp_eq(ctx, a->SrcL, a->SrcR, true, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fne_32(DisasContext *ctx, arg_simt_fne_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv dest = gen_simt_fp_eq(ctx, a->SrcL, a->SrcR, false, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static TCGv gen_simt_fp_lt(DisasContext *ctx, int srcl_code, int srcr_code,
                             bool is_lt, int lane_id)
{
    TCGv src_l = get_src_regx_fvec(ctx, srcl_code, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, srcr_code, lane_id);
    TCGv tmp = tcg_temp_local_new();
    switch (extract16(srcl_code, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_flt_b(tmp, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_flt_h(tmp, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_flt_s(tmp, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_flt_d(tmp, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    if (!is_lt) {
        tcg_gen_not_tl(tmp, tmp);
        tcg_gen_andi_tl(tmp, tmp, 1);
    }

    return tmp;
}

static bool trans_simt_flt_32(DisasContext *ctx, arg_simt_flt_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv dest = gen_simt_fp_lt(ctx, a->SrcL, a->SrcR, true, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fge_32(DisasContext *ctx, arg_simt_fge_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv dest = gen_simt_fp_lt(ctx, a->SrcL, a->SrcR, false, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_feqs_32(DisasContext *ctx, arg_simt_feqs_32 *a)
{
    SIMT_WRAP_BEGIN

    /* TODO: need to handle the signal */
    TCGv dest = gen_simt_fp_eq(ctx, a->SrcL, a->SrcR, true, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fnes_32(DisasContext *ctx, arg_simt_fnes_32 *a)
{
    SIMT_WRAP_BEGIN

    /* TODO: need to handle the signal */
    TCGv dest = gen_simt_fp_eq(ctx, a->SrcL, a->SrcR, false, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_flts_32(DisasContext *ctx, arg_simt_flts_32 *a)
{
    SIMT_WRAP_BEGIN

    /* TODO: need to handle the signal */
    TCGv dest = gen_simt_fp_lt(ctx, a->SrcL, a->SrcR, true, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fges_32(DisasContext *ctx, arg_simt_fges_32 *a)
{
    SIMT_WRAP_BEGIN

    /* TODO: need to handle the signal */
    TCGv dest = gen_simt_fp_lt(ctx, a->SrcL, a->SrcR, false, lane_id);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fabs_32(DisasContext *ctx, arg_simt_fabs_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(7, 1));
        break;
    case SIMT_HALF_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(15, 1));
        break;
    case SIMT_SINGLE_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(31, 1));
        break;
    case SIMT_DOUBLE_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(63, 1));
        break;
    default:
        g_assert_not_reached();
    }

    /* DstType doesn't seem to work */

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fsqrt_32(DisasContext *ctx, arg_simt_fsqrt_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fsqrt_b(dest, cpu_env, src_l);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fsqrt_h(dest, cpu_env, src_l);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fsqrt_s(dest, cpu_env, src_l);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fsqrt_d(dest, cpu_env, src_l);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fexp_32(DisasContext *ctx, arg_simt_fexp_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv vlen = tcg_constant_i64(a->vlen);
    TCGv rm = tcg_constant_i64(get_real_round_mode(a->rm));
    TCGv sat = tcg_constant_i64(a->sat);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_SINGLE_PRECISION:
        gen_helper_fexp_s(dest, cpu_env, src_l);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fexp_d(dest, cpu_env, src_l);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fexp_h(dest, cpu_env, src_l);
        break;
    case SIMT_LOW_PRECISION:
        gen_helper_fexp_b(dest, cpu_env, src_l);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fexp_xn(dest, cpu_env, src_l, vlen, sat, rm);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_frecip_32(DisasContext *ctx, arg_simt_frecip_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    TCGv fp8_e4m3_1 = tcg_constant_i64(0x38);

    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fdiv_b(dest, cpu_env, fp8_e4m3_1, src_l);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fdiv_h(dest, cpu_env, tcg_constant_tl(float16_one), src_l);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fdiv_s(dest, cpu_env, tcg_constant_tl(float32_one), src_l);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fdiv_d(dest, cpu_env, tcg_constant_tl(float64_one), src_l);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fclass_32(DisasContext *ctx, arg_simt_fclass_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fclass_b(dest, src_l);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fclass_h(dest, src_l);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fclass_s(dest, src_l);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fclass_d(dest, src_l);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static inline bool gen_fvec_maxmin(DisasContext *ctx, arg_simt_max_32 *a,
    DisasExtend ext, void (*func)(TCGv, TCGv, TCGv))
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    func(dest, src_l, src_r);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)
    return true;
}

static bool trans_simt_max_32(DisasContext *ctx, arg_simt_max_32 *a)
{
    return gen_fvec_maxmin(ctx, a, EXT_SIGN, tcg_gen_smax_tl);
}

static bool trans_simt_min_32(DisasContext *ctx, arg_simt_min_32 *a)
{
    return gen_fvec_maxmin(ctx, a, EXT_SIGN, tcg_gen_smin_tl);
}

#ifndef PRERELEASE
static bool trans_simt_maxu_32(DisasContext *ctx, arg_simt_maxu_32 *a)
{
    return gen_fvec_maxmin(ctx, a, EXT_ZERO, tcg_gen_umax_tl);
}

static bool trans_simt_minu_32(DisasContext *ctx, arg_simt_minu_32 *a)
{
    return gen_fvec_maxmin(ctx, a, EXT_ZERO, tcg_gen_umin_tl);
}
#endif

static bool trans_simt_fmax_32(DisasContext *ctx, arg_simt_fmax_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv vlen = tcg_constant_i64(a->vlen);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fmax_b(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fmax_h(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fmax_s(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fmax_d(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fmax_xn(dest, cpu_env, src_l, src_r, vlen);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_fmin_32(DisasContext *ctx, arg_simt_fmin_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv vlen = tcg_constant_i64(a->vlen);
    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fmin_b(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fmin_h(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fmin_s(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fmin_d(dest, cpu_env, src_l, src_r);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fmin_xn(dest, cpu_env, src_l, src_r, vlen);
        break;
    default:
        g_assert_not_reached();
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)
    return true;
}

static bool trans_simt_csel_32(DisasContext *ctx, arg_simt_csel_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    src2 = conver_src_value(ctx, src2, INSTR_TYPE_AU, a->SrcRType);
    TCGv SrcP = get_src_regx_fvec_with_width(ctx, a->SrcP, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv zero = tcg_constant_tl(0);

    tcg_gen_movcond_tl(TCG_COND_NE, dest, SrcP, zero, src2, src1);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_psel_32(DisasContext *ctx, arg_simt_csel_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src1 = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv src2 = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    src2 = conver_src_value(ctx, src2, INSTR_TYPE_AU, a->SrcRType);
    TCGv SrcP = get_src_regx_fvec_with_width(ctx, a->SrcP, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv zero = tcg_constant_tl(0);
    TCGv cnd = tcg_temp_new();
    tcg_gen_andi_tl(cnd, SrcP, 1 << lane_id);

    tcg_gen_movcond_tl(TCG_COND_NE, dest, cnd, zero, src1, src2);
    tcg_temp_free(cnd);
    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_fcvt_32(DisasContext *ctx, arg_simt_fcvt_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTZ(round_to_zero)
     * in the soft floating point library
     */
    FloatRoundMode rm = get_real_round_mode(a->rm);
    gen_helper_set_rounding_mode(cpu_env, tcg_constant_i32(rm));

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv src_r = get_src_regx_fvec(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    switch (a->DstType) {
    case SIMT_CVT_FP8:
        switch (a->SrcType) {
        case SIMT_CVT_FP64:
            gen_helper_fcvt_b_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_b_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_b_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_b_bf(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_b_lb(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case SIMT_CVT_FP16:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_h_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_h_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_h_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_h_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_h_bf(dest, cpu_env, src_l);
            break;
        default:
            assert(0);
        }
        break;
    case SIMT_CVT_FP32:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_s_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_s_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_s_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_s_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_s_bf(dest, cpu_env, src_l);
            break;
        default:
            assert(0);
        }
        break;
    case SIMT_CVT_FP64:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_d_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_d_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_d_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_d_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_d_bf(dest, cpu_env, src_l);
            break;
        default:
            assert(0);
        }
        break;
    case SIMT_CVT_FP8_1:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_lb_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_lb_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_lb_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_lb_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_lb_bf(dest, cpu_env, src_l);
            break;
        default:
            assert(0);
        }
        break;
    case SIMT_CVT_BF16:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_bf_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_bf_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_bf_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_bf_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_bf_lb(dest, cpu_env, src_l);
            break;
        default:
            assert(0);
        }
        break;
    case SIMT_CVT_HIF4x2:
        switch (a->SrcType) {
        case SIMT_CVT_BF16:
            gen_helper_bf_to_hif4x2(dest, src_l, src_r, tcg_constant_i32(rm));
            break;
        default:
            assert(0);
        }
        break;
    case SIMT_CVT_BF16x2:
        switch (a->SrcType) {
        case SIMT_CVT_BF16:
            gen_helper_bf_to_bf16x2(dest, src_l, src_r);
            break;
        default:
            assert(0);
        }
        break;
    case SIMT_CVT_E1M2x2:
        switch (a->SrcType) {
        case SIMT_CVT_BF16:
            gen_helper_bf_to_hif4x2(dest, src_l, src_r, tcg_constant_i32(rm));
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_bf_s(src_l, cpu_env, src_l);
            gen_helper_fcvt_bf_s(src_r, cpu_env, src_r);
            gen_helper_bf_to_hif4x2(dest, src_l, src_r, tcg_constant_i32(rm));
            break;
        default:
            assert(0);
        }
        break;
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)

    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_simt_fcvti_32(DisasContext *ctx, arg_simt_fcvti_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTZ(round_to_zero)
     * in the soft floating point library
     */
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_to_zero));

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    switch (a->DstType) {
    case SIMT_CVT_U64:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_ud_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_ud_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_ud_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_ud_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_ud_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_ud_bf(dest, cpu_env, src_l);
            break;
        }
        break;
    case SIMT_CVT_U32:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_uw_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_uw_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_uw_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_uw_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_uw_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_uw_bf(dest, cpu_env, src_l);
            break;
        }
        break;
    case SIMT_CVT_U16:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_uh_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_uh_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_uh_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_uh_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_uh_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_uh_bf(dest, cpu_env, src_l);
            break;
            }
        break;
    case SIMT_CVT_U8:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_ub_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_ub_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_ub_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_ub_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_ub_bf(dest, cpu_env, src_l);
            break;
        }
        break;
    case SIMT_CVT_S64:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_sd_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_sd_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_sd_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_sd_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_sd_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_sd_bf(dest, cpu_env, src_l);
            break;
        }
        break;
    case SIMT_CVT_S32:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_sw_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_sw_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_sw_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_sw_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_sw_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_sw_bf(dest, cpu_env, src_l);
            break;
        }
        break;
    case SIMT_CVT_S16:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_sh_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_sh_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_sh_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_sh_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_sh_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_sh_bf(dest, cpu_env, src_l);
            break;
        }
        break;
    case SIMT_CVT_S8:
        switch (a->SrcType) {
        case SIMT_CVT_FP8:
            gen_helper_fcvt_sb_b(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP16:
            gen_helper_fcvt_sb_h(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP32:
            gen_helper_fcvt_sb_s(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP64:
            gen_helper_fcvt_sb_d(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_FP8_1:
            gen_helper_fcvt_sb_lb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_BF16:
            gen_helper_fcvt_sb_bf(dest, cpu_env, src_l);
            break;
        }
        break;
    case SIMT_CVT_S4:
    case SIMT_CVT_U4:
        qemu_log("dst type U4 & S4 has not yet been supported!\n");
        linx_debug_not_reached();
        break;
    default:
        linx_debug_not_reached();
        break;
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)

    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));
    return true;
}


static bool gen_scalar_icvt_32(DisasContext *ctx, arg_simt_icvt_32 *a)
{

    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, 0);
    TCGv dest = temp_new(ctx);

    switch (a->DstType) {
    case SIMT_CVT_U64:
    case SIMT_CVT_S64:
        tcg_gen_mov_tl(dest, src_l);
        break;
    case SIMT_CVT_U32:
    case SIMT_CVT_S32:
        tcg_gen_ext32u_tl(dest, src_l);
        break;
    case SIMT_CVT_U16:
    case SIMT_CVT_S16:
        tcg_gen_ext16u_tl(dest, src_l);
        break;
    case SIMT_CVT_U8:
    case SIMT_CVT_S8:
        tcg_gen_ext8u_tl(dest, src_l);
        break;
    default:
        linx_debug_not_reached();
        break;
    }

    simt_scalar_set_dst(ctx, a->RegDst, dest);
    return true;
}


static bool trans_simt_icvt_32(DisasContext *ctx, arg_simt_icvt_32 *a)
{
    if (!is_vec_reg(a->RegDst)) {
        return gen_scalar_icvt_32(ctx, a);
    }

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    switch (a->DstType) {
    case SIMT_CVT_U64:
    case SIMT_CVT_S64:
        tcg_gen_mov_tl(dest, src_l);
        break;
    case SIMT_CVT_U32:
    case SIMT_CVT_S32:
        tcg_gen_ext32u_tl(dest, src_l);
        break;
    case SIMT_CVT_U16:
    case SIMT_CVT_S16:
        tcg_gen_ext16u_tl(dest, src_l);
        break;
    case SIMT_CVT_U8:
    case SIMT_CVT_S8:
        tcg_gen_ext8u_tl(dest, src_l);
        break;
    default:
        linx_debug_not_reached();
        break;
    }

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)

    return true;
}

static void icvtf_trans(TCGv src_l, TCGv dest, arg_simt_icvtf_32 *a)
{
    switch (a->DstType) {
    case SIMT_CVT_FP64:
        switch (a->SrcType) {
        case SIMT_CVT_S8:
            gen_helper_fcvt_d_sb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S16:
            gen_helper_fcvt_d_sh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S32:
            gen_helper_fcvt_d_sw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S64:
            gen_helper_fcvt_d_sd(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U8:
            gen_helper_fcvt_d_ub(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U16:
            gen_helper_fcvt_d_uh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U32:
            gen_helper_fcvt_d_uw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U64:
            gen_helper_fcvt_d_ud(dest, cpu_env, src_l);
            break;
        }
        assert(0);
        break;
    case SIMT_CVT_FP32:
        switch (a->SrcType) {
        case SIMT_CVT_S8:
            gen_helper_fcvt_s_sb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S16:
            gen_helper_fcvt_s_sh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S32:
            gen_helper_fcvt_s_sw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S64:
            gen_helper_fcvt_s_sd(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U8:
            gen_helper_fcvt_s_ub(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U16:
            gen_helper_fcvt_s_uh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U32:
            gen_helper_fcvt_s_uw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U64:
            gen_helper_fcvt_s_ud(dest, cpu_env, src_l);
            break;
        }
        assert(0);
        break;
    case SIMT_CVT_FP16:
        switch (a->SrcType) {
        case SIMT_CVT_S8:
            gen_helper_fcvt_h_sb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S16:
            gen_helper_fcvt_h_sh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S32:
            gen_helper_fcvt_h_sw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S64:
            gen_helper_fcvt_h_sd(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U8:
            gen_helper_fcvt_h_ub(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U16:
            gen_helper_fcvt_h_uh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U32:
            gen_helper_fcvt_h_uw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U64:
            gen_helper_fcvt_h_ud(dest, cpu_env, src_l);
            break;
        }
        assert(0);
        break;
    case SIMT_CVT_FP8:
        switch (a->SrcType) {
        case SIMT_CVT_S8:
            gen_helper_fcvt_b_sb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S16:
            gen_helper_fcvt_b_sh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S32:
            gen_helper_fcvt_b_sw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S64:
            gen_helper_fcvt_b_sd(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U8:
            gen_helper_fcvt_b_ub(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U16:
            gen_helper_fcvt_b_uh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U32:
            gen_helper_fcvt_b_uw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U64:
            gen_helper_fcvt_b_ud(dest, cpu_env, src_l);
            break;
        }
        assert(0);
        break;
    case SIMT_CVT_FP8_1:
        switch (a->SrcType) {
        case SIMT_CVT_S8:
            gen_helper_fcvt_lb_sb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S16:
            gen_helper_fcvt_lb_sh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S32:
            gen_helper_fcvt_lb_sw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S64:
            gen_helper_fcvt_lb_sd(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U8:
            gen_helper_fcvt_lb_ub(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U16:
            gen_helper_fcvt_lb_uh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U32:
            gen_helper_fcvt_lb_uw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U64:
            gen_helper_fcvt_lb_ud(dest, cpu_env, src_l);
            break;
        }
        assert(0);
        break;
    case SIMT_CVT_BF16:
        switch (a->SrcType) {
        case SIMT_CVT_S8:
            gen_helper_fcvt_bf_sb(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S16:
            gen_helper_fcvt_bf_sh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S32:
            gen_helper_fcvt_bf_sw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_S64:
            gen_helper_fcvt_bf_sd(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U8:
            gen_helper_fcvt_bf_ub(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U16:
            gen_helper_fcvt_bf_uh(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U32:
            gen_helper_fcvt_bf_uw(dest, cpu_env, src_l);
            break;
        case SIMT_CVT_U64:
            gen_helper_fcvt_bf_ud(dest, cpu_env, src_l);
            break;
        }
        assert(0);
        break;

    default:
        linx_debug_not_reached();
        break;
    }
    return;
}

static bool gen_scalar_icvtf_32(DisasContext *ctx, arg_simt_icvtf_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTZ(round_to_zero)
     * in the soft floating point library
     */
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_to_zero));

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, 0);
    TCGv dest = temp_new(ctx);
    icvtf_trans(src_l, dest, a);
    simt_scalar_set_dst(ctx, a->RegDst, dest);

    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_simt_icvtf_32(DisasContext *ctx, arg_simt_icvtf_32 *a)
{
    if (!is_vec_reg(a->RegDst)) {
        return gen_scalar_icvtf_32(ctx, a);
    }

    /*
     * the round mode of fcvt is aligned with the RTZ(round_to_zero)
     * in the soft floating point library
     */
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_to_zero));

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    icvtf_trans(src_l, dest, a);

    SIMT_WRAP_END_DEST(a->RegDst, REG_UTMN)

    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));
    return true;
}

#ifndef PRERELEASE
static TCGv get_fp_compare_lane_direction_mask(DisasContext *ctx, int srcl,
    int srcr, bool operation, TCGv (*func)(DisasContext *, int, int, bool, int))
{
    CPULINXState *env = ctx->cs->env_ptr;
    TCGv lane_direction_mask = tcg_temp_local_new();
    tcg_gen_movi_tl(lane_direction_mask, 0);

    /* Obtains the direction of each branch of a activate lane. */
    for (int lane_id = 0; lane_id < env->csr_lanenum; ++lane_id) {
        TCGLabel *disable_lane = gen_new_label();
        TCGv lane_exec_flag = tcg_temp_new();
        tcg_gen_andi_tl(lane_exec_flag, predm, 1 << lane_id);
        tcg_gen_brcondi_tl(TCG_COND_EQ, lane_exec_flag, 0, disable_lane);

        TCGv is_jump = func(ctx, srcl, srcr, operation, lane_id);

        tcg_gen_shli_tl(is_jump, is_jump, lane_id);
        tcg_gen_or_tl(lane_direction_mask, lane_direction_mask, is_jump);

        gen_set_label(disable_lane);
        tcg_temp_free(is_jump);
        tcg_temp_free(lane_exec_flag);
    }
    return lane_direction_mask;
}

static bool trans_simt_b_feq_32(DisasContext *ctx, arg_simt_b_feq_32 *a)
{
    TCGv lane_direction_mask = get_fp_compare_lane_direction_mask(ctx, a->SrcL,
                                                a->SrcR, true, gen_simt_fp_eq);
    handle_simt_branch(ctx, lane_direction_mask, a->br_offset, a->rc_offset);
    return true;
}

static bool trans_simt_b_fne_32(DisasContext *ctx, arg_simt_b_fne_32 *a)
{
    TCGv lane_direction_mask = get_fp_compare_lane_direction_mask(ctx, a->SrcL,
                                                a->SrcR, false, gen_simt_fp_eq);
    handle_simt_branch(ctx, lane_direction_mask, a->br_offset, a->rc_offset);
    return true;
}

static bool trans_simt_b_flt_32(DisasContext *ctx, arg_simt_b_flt_32 *a)
{
    TCGv lane_direction_mask = get_fp_compare_lane_direction_mask(ctx, a->SrcL,
                                                a->SrcR, true, gen_simt_fp_lt);
    handle_simt_branch(ctx, lane_direction_mask, a->br_offset, a->rc_offset);
    return true;
}

static bool trans_simt_b_fge_32(DisasContext *ctx, arg_simt_b_fge_32 *a)
{
    TCGv lane_direction_mask = get_fp_compare_lane_direction_mask(ctx, a->SrcL,
                                                a->SrcR, false, gen_simt_fp_lt);
    handle_simt_branch(ctx, lane_direction_mask, a->br_offset, a->rc_offset);
    return true;
}
#endif

static inline bool gen_fvec_rd(DisasContext *ctx, arg_simt_rdadd_32 *a,
    void (*func)(TCGv, TCGv, TCGv))
{
    int dst = extract16(a->RegDst, 0, 7);
    TCGv dest = tcg_temp_local_new();
    if (dst >= DST_FVEC_RO0 && dst <= DST_FVEC_RO3) {
        TCGv_i32 ro_idx = tcg_temp_new_i32();
        TCGv tmp = temp_new(ctx);
        tcg_gen_movi_i32(ro_idx, dst - DST_FVEC_RO0);
        gen_helper_get_ro_gpr(tmp, cpu_env, ro_idx);
        tcg_gen_mov_tl(dest, tmp);
        tcg_temp_free_i32(ro_idx);
    } else {
        tcg_gen_mov_tl(dest, get_src_regx_fvec_with_width(ctx, a->SrcL, 0));
    }

    ctx->c_args[0] = a->RegDst;
    linx_block_constraint_check(ctx, CONSTRAINT_A6_8);
    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);

    func(dest, dest, src_l);

    SIMT_WRAP_END

    set_dst_regx_fvec(ctx, dest, a->RegDst, 0, REG_GPR);
    tcg_temp_free(dest);
    return true;
}

static bool trans_simt_rdadd_32(DisasContext *ctx, arg_simt_rdadd_32 *a)
{
    return gen_fvec_rd(ctx, a, tcg_gen_add_tl);
}

static bool trans_simt_rdand_32(DisasContext *ctx, arg_simt_rdand_32 *a)
{
    return gen_fvec_rd(ctx, a, tcg_gen_and_tl);
}

static bool trans_simt_rdor_32(DisasContext *ctx, arg_simt_rdor_32 *a)
{
    return gen_fvec_rd(ctx, a, tcg_gen_or_tl);
}

static bool trans_simt_rdxor_32(DisasContext *ctx, arg_simt_rdxor_32 *a)
{
    return gen_fvec_rd(ctx, a, tcg_gen_xor_tl);
}

static bool trans_simt_rdmax_32(DisasContext *ctx, arg_simt_rdmax_32 *a)
{
    if (extract16(a->SrcL, 9, 1)) {
        return gen_fvec_rd(ctx, a, tcg_gen_smax_tl);
    } else {
        return gen_fvec_rd(ctx, a, tcg_gen_umax_tl);
    }
}

static bool trans_simt_rdmin_32(DisasContext *ctx, arg_simt_rdmin_32 *a)
{
    if (extract16(a->SrcL, 9, 1)) {
        return gen_fvec_rd(ctx, a, tcg_gen_smin_tl);
    } else {
        return gen_fvec_rd(ctx, a, tcg_gen_umin_tl);
    }
}

static bool trans_simt_rdfadd_32(DisasContext *ctx, arg_simt_rdfadd_32 *a)
{
    ctx->c_args[0] = a->RegDst;
    linx_block_constraint_check(ctx, CONSTRAINT_A6_8);

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fadd_b(dest, cpu_env, src_l, dest);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fadd_h(dest, cpu_env, src_l, dest);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fadd_s(dest, cpu_env, src_l, dest);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fadd_d(dest, cpu_env, src_l, dest);
        break;
    case SIMT_BF_PRECISION:
        gen_helper_fadd_bf(dest, cpu_env, src_l, dest);
        break;
    default:
        g_assert_not_reached();
    }
    tcg_temp_free(dest);
    SIMT_WRAP_END

    return true;
}

static bool trans_simt_rdfmax_32(DisasContext *ctx, arg_simt_rdfmax_32 *a)
{
    ctx->c_args[0] = a->RegDst;
    linx_block_constraint_check(ctx, CONSTRAINT_A6_8);

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fmax_b(dest, cpu_env, src_l, dest);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fmax_h(dest, cpu_env, src_l, dest);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fmax_s(dest, cpu_env, src_l, dest);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fmax_d(dest, cpu_env, src_l, dest);
        break;
    default:
        g_assert_not_reached();
    }
    tcg_temp_free(dest);
    SIMT_WRAP_END

    return true;
}

static bool trans_simt_rdfmin_32(DisasContext *ctx, arg_simt_rdfmin_32 *a)
{

    ctx->c_args[0] = a->RegDst;
    linx_block_constraint_check(ctx, CONSTRAINT_A6_8);

    SIMT_WRAP_BEGIN

    TCGv src_l = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);

    switch (extract16(a->SrcL, 7, 3)) {
    case SIMT_LOW_PRECISION:
        gen_helper_fmin_b(dest, cpu_env, src_l, dest);
        break;
    case SIMT_HALF_PRECISION:
        gen_helper_fmin_h(dest, cpu_env, src_l, dest);
        break;
    case SIMT_SINGLE_PRECISION:
        gen_helper_fmin_s(dest, cpu_env, src_l, dest);
        break;
    case SIMT_DOUBLE_PRECISION:
        gen_helper_fmin_d(dest, cpu_env, src_l, dest);
        break;
    default:
        g_assert_not_reached();
    }
    tcg_temp_free(dest);
    SIMT_WRAP_END

    return true;
}

static bool gen_simt_amo_load_32(DisasContext *ctx, arg_simt_lw_add_32 *a,
    MemOp memop, void (*func_arith)(TCGv, TCGv, TCGv),
    void (*func_amo)(TCGv, TCGv, TCGv, TCGArg, MemOp))
{
    if (a->rl) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_STRL);
    }

    SIMT_WRAP_BEGIN

    TCGv addr_base = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv data = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv res = tcg_temp_new();

    if (!a->far) {
        func_amo(dest, addr_base, data, ctx->mem_idx, memop);
        gen_helper_log(cpu_env, data);
    } else {
        if (extract16(a->SrcR, 9, 1) && memop == MO_TESL) {
            tcg_gen_ext32s_tl(data, data);
        } else if (memop == MO_TESL) {
            tcg_gen_ext32u_tl(data, data);
        }

        tcg_gen_qemu_ld_tl(dest, addr_base, ctx->mem_idx, memop);
        /* Fixme: dest whether truncation is required */
        tcg_gen_mov_tl(res, dest);
        func_arith(res, res, data);
        tcg_gen_qemu_st_tl(res, addr_base, ctx->mem_idx, memop);
    }

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(addr_base, dest, tcg_constant_tl(READ));
        gen_helper_memprnt(addr_base, res, tcg_constant_tl(WRITE));
    }
    tcg_temp_free(res);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    if (a->aq) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_LDAQ);
    }

    return true;
}

static bool trans_simt_lw_add_32(DisasContext *ctx, arg_simt_lw_add_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TESL, tcg_gen_add_tl,
                                tcg_gen_atomic_fetch_add_tl);
}

static bool trans_simt_lw_and_32(DisasContext *ctx, arg_simt_lw_and_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TESL, tcg_gen_and_tl,
                                tcg_gen_atomic_fetch_and_tl);
}

static bool trans_simt_lw_or_32(DisasContext *ctx, arg_simt_lw_or_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TESL, tcg_gen_or_tl,
                                tcg_gen_atomic_fetch_or_tl);
}

static bool trans_simt_lw_xor_32(DisasContext *ctx, arg_simt_lw_xor_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TESL, tcg_gen_xor_tl,
                                tcg_gen_atomic_fetch_xor_tl);
}

static void *get_func_arith(int symbel, int condi_max)
{
    int sign = 1;
    if (symbel == sign) {
        if (condi_max) {
            return tcg_gen_smax_tl;
        }
        return tcg_gen_smin_tl;
    } else if (condi_max) {
        return tcg_gen_umax_tl;
    }
    return tcg_gen_umin_tl;
}

static void *get_func_amo(int symbel, int condi_max)
{
    int sign = 1;
    if (symbel == sign) {
        if (condi_max) {
        return tcg_gen_atomic_fetch_smax_tl;
        }
        return tcg_gen_atomic_fetch_smin_tl;
    } else if (condi_max) {
        return tcg_gen_atomic_fetch_umax_tl;
    }
    return tcg_gen_atomic_fetch_umin_tl;
}

static bool trans_simt_lw_max_32(DisasContext *ctx, arg_simt_lw_max_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 1);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 1);

    return gen_simt_amo_load_32(ctx, a, MO_TESL, func_arith, func_amo);
}

static bool trans_simt_lw_min_32(DisasContext *ctx, arg_simt_lw_min_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 0);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 0);

    return gen_simt_amo_load_32(ctx, a, MO_TESL, func_arith, func_amo);
}

static bool trans_simt_ld_add_32(DisasContext *ctx, arg_simt_ld_add_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TEQ, tcg_gen_add_tl,
                                tcg_gen_atomic_fetch_add_tl);
}

static bool trans_simt_ld_and_32(DisasContext *ctx, arg_simt_ld_and_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TEQ, tcg_gen_and_tl,
                                tcg_gen_atomic_fetch_and_tl);
}

static bool trans_simt_ld_or_32(DisasContext *ctx, arg_simt_ld_or_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TEQ, tcg_gen_or_tl,
                                tcg_gen_atomic_fetch_or_tl);
}

static bool trans_simt_ld_xor_32(DisasContext *ctx, arg_simt_ld_xor_32 *a)
{
    return gen_simt_amo_load_32(ctx, a, MO_TEQ, tcg_gen_xor_tl,
                                tcg_gen_atomic_fetch_xor_tl);
}

static bool trans_simt_ld_max_32(DisasContext *ctx, arg_simt_ld_max_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 1);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 1);

    return gen_simt_amo_load_32(ctx, a, MO_TEQ, func_arith, func_amo);
}

static bool trans_simt_ld_min_32(DisasContext *ctx, arg_simt_ld_min_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 0);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 0);

    return gen_simt_amo_load_32(ctx, a, MO_TEQ, func_arith, func_amo);
}

static bool gen_simt_amo_store_32(DisasContext *ctx, arg_simt_sw_add_32 *a,
    MemOp memop, void (*func_arith)(TCGv, TCGv, TCGv),
    void (*func_amo)(TCGv, TCGv, TCGv, TCGArg, MemOp))
{
    if (a->rl) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_STRL);
    }

    SIMT_WRAP_BEGIN

    TCGv addr_base = get_src_regx_fvec(ctx, a->SrcL, lane_id);
    TCGv data = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = tcg_temp_new();

    /* Fixme: impl rd operation */
    func_amo(dest, addr_base, data, ctx->mem_idx, memop);

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(addr_base, dest, tcg_constant_tl(WRITE));
    }
    tcg_temp_free(dest);

    SIMT_WRAP_END

    return true;
}

static bool trans_simt_sw_add_32(DisasContext *ctx, arg_simt_sw_add_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TESL, tcg_gen_add_tl,
                                tcg_gen_atomic_fetch_add_tl);
}

static bool trans_simt_sw_and_32(DisasContext *ctx, arg_simt_sw_and_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TESL, tcg_gen_and_tl,
                                tcg_gen_atomic_fetch_and_tl);
}

static bool trans_simt_sw_or_32(DisasContext *ctx, arg_simt_sw_or_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TESL, tcg_gen_or_tl,
                                tcg_gen_atomic_fetch_or_tl);
}

static bool trans_simt_sw_xor_32(DisasContext *ctx, arg_simt_sw_xor_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TESL, tcg_gen_xor_tl,
                                tcg_gen_atomic_fetch_xor_tl);
}

static bool trans_simt_sw_max_32(DisasContext *ctx, arg_simt_sw_max_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 1);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 1);

    return gen_simt_amo_store_32(ctx, a, MO_TESL, func_arith, func_amo);
}

static bool trans_simt_sw_min_32(DisasContext *ctx, arg_simt_sw_min_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 0);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 0);

    return gen_simt_amo_store_32(ctx, a, MO_TESL, func_arith, func_amo);
}

static bool trans_simt_sd_add_32(DisasContext *ctx, arg_simt_sd_add_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TEQ, tcg_gen_add_tl,
                                tcg_gen_atomic_fetch_add_tl);
}

static bool trans_simt_sd_and_32(DisasContext *ctx, arg_simt_sd_and_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TEQ, tcg_gen_and_tl,
                                tcg_gen_atomic_fetch_and_tl);
}

static bool trans_simt_sd_or_32(DisasContext *ctx, arg_simt_sd_or_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TEQ, tcg_gen_or_tl,
                                tcg_gen_atomic_fetch_or_tl);
}

static bool trans_simt_sd_xor_32(DisasContext *ctx, arg_simt_sd_xor_32 *a)
{
    return gen_simt_amo_store_32(ctx, a, MO_TEQ, tcg_gen_xor_tl,
                                tcg_gen_atomic_fetch_xor_tl);
}

static bool trans_simt_sd_max_32(DisasContext *ctx, arg_simt_sd_max_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 1);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 1);

    return gen_simt_amo_store_32(ctx, a, MO_TEQ, func_arith, func_amo);
}

static bool trans_simt_sd_min_32(DisasContext *ctx, arg_simt_sd_min_32 *a)
{
    void *func_arith = get_func_arith(extract16(a->SrcR, 9, 1), 0);
    void *func_amo = get_func_amo(extract16(a->SrcR, 9, 1), 0);

    return gen_simt_amo_store_32(ctx, a, MO_TEQ, func_arith, func_amo);
}
static bool trans_simt_shfl_up_32(DisasContext *ctx, arg_simt_shfl_up_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv src_d = get_src_regx_fvec_with_width(ctx, a->SrcD, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv offset_lane = tcg_temp_new();
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_remu_tl(offset_lane, src_d, src_r);
    tcg_gen_sub_tl(src_lane_id, tcg_constant_tl(lane_id), offset_lane);
    gen_helper_shfl_up_set_dest(dest, cpu_env, tcg_constant_tl(a->SrcL),
                                 tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(offset_lane);
    tcg_temp_free(src_lane_id);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_shfl_down_32(DisasContext *ctx, arg_simt_shfl_down_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv src_d = get_src_regx_fvec_with_width(ctx, a->SrcD, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv offset_lane = tcg_temp_new();
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_remu_tl(offset_lane, src_d, src_r);
    tcg_gen_add_tl(src_lane_id, tcg_constant_tl(lane_id), offset_lane);
    gen_helper_shfl_down_set_dest(dest, cpu_env, src_r,
        tcg_constant_tl(a->SrcL), tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(offset_lane);
    tcg_temp_free(src_lane_id);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_shfl_bfly_32(DisasContext *ctx, arg_simt_shfl_bfly_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_d = get_src_regx_fvec_with_width(ctx, a->SrcD, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_xori_tl(src_lane_id, src_d, lane_id);
    gen_helper_shfl_set_dest(dest, cpu_env, tcg_constant_tl(a->SrcL),
                             tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(src_lane_id);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_shfl_idx_32(DisasContext *ctx, arg_simt_shfl_idx_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv src_d = get_src_regx_fvec_with_width(ctx, a->SrcD, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_remu_tl(src_lane_id, src_d, src_r);
    gen_helper_shfl_set_dest(dest, cpu_env, tcg_constant_tl(a->SrcL),
                             tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(src_lane_id);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_shfli_up_32(DisasContext *ctx, arg_simt_shfli_up_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv offset_lane = tcg_temp_new();
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_remu_tl(offset_lane, tcg_constant_tl(a->imm), src_r);
    tcg_gen_sub_tl(src_lane_id, tcg_constant_tl(lane_id), offset_lane);
    gen_helper_shfl_up_set_dest(dest, cpu_env, tcg_constant_tl(a->SrcL),
                                 tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(offset_lane);
    tcg_temp_free(src_lane_id);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_shfli_down_32(DisasContext *ctx,
                                     arg_simt_shfli_down_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv offset_lane = tcg_temp_new();
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_remu_tl(offset_lane, tcg_constant_tl(a->imm), src_r);
    tcg_gen_add_tl(src_lane_id, tcg_constant_tl(lane_id), offset_lane);
    gen_helper_shfl_down_set_dest(dest, cpu_env, src_r,
        tcg_constant_tl(a->SrcL), tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(offset_lane);
    tcg_temp_free(src_lane_id);
    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_shfli_bfly_32(DisasContext *ctx,
                                     arg_simt_shfli_bfly_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_xori_tl(src_lane_id, tcg_constant_tl(a->imm), lane_id);
    gen_helper_shfl_set_dest(dest, cpu_env, tcg_constant_tl(a->SrcL),
                                  tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(src_lane_id);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_shfli_idx_32(DisasContext *ctx, arg_simt_shfli_idx_32 *a)
{
    SIMT_WRAP_BEGIN

    TCGv src_r = get_src_regx_fvec_with_width(ctx, a->SrcR, lane_id);
    TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
    TCGv src_lane_id = tcg_temp_new();

    tcg_gen_remu_tl(src_lane_id, tcg_constant_tl(a->imm), src_r);
    gen_helper_shfl_set_dest(dest, cpu_env, tcg_constant_tl(a->SrcL),
                             tcg_constant_tl(lane_id), src_lane_id);

    tcg_temp_free(src_lane_id);

    SIMT_WRAP_END_DEST_EXT(a->RegDst)

    return true;
}

static bool trans_simt_mov_32(DisasContext *ctx, arg_simt_mov_32 *a)
{
    CPULINXState *env = ctx->cs->env_ptr;
    for (int lane_id = 0; lane_id < env->csr_lanenum; ++lane_id) {
        TCGv dest = get_dst_regx_fvec(ctx, a->RegDst, lane_id);
        TCGv src_l = get_src_regx_fvec_with_width(ctx, a->SrcL, lane_id);

        tcg_gen_mov_tl(dest, src_l);
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM)) {
            gen_helper_vec_print(dest);
        }
        set_dst_regx_fvec_with_width(ctx, dest, a->RegDst, lane_id);
        tcg_temp_free(dest);
        temp_free(ctx);
    }
    return true;
}

#define LOAD_PCR(MEMOP)                                         \
do {                                                            \
    TCGv addr = temp_new(ctx);                                  \
    tcg_gen_movi_tl(addr, ctx->base.pc_next + a->imm);          \
    gen_blk_load_32_op(ctx, MEMOP, NO_ADDR, addr, a->RegDst);   \
    return true;                                                \
} while (0)

static bool trans_l_lb_pcr(DisasContext *ctx, arg_l_lb_pcr *a)
{
    LOAD_PCR(MO_SB);
}

static bool trans_l_lh_pcr(DisasContext *ctx, arg_l_lh_pcr *a)
{
    LOAD_PCR(MO_SW);
}

static bool trans_l_lw_pcr(DisasContext *ctx, arg_l_lw_pcr *a)
{
    LOAD_PCR(MO_SL);
}

static bool trans_l_ld_pcr(DisasContext *ctx, arg_l_ld_pcr *a)
{
    LOAD_PCR(MO_Q);
}

static bool trans_l_lbu_pcr(DisasContext *ctx, arg_l_lbu_pcr *a)
{
    LOAD_PCR(MO_UB);
}

static bool trans_l_lhu_pcr(DisasContext *ctx, arg_l_lhu_pcr *a)
{
    LOAD_PCR(MO_UW);
}

static bool trans_l_lwu_pcr(DisasContext *ctx, arg_l_lwu_pcr *a)
{
    LOAD_PCR(MO_UL);
}

static bool trans_l_sb_pcr(DisasContext *ctx, arg_l_sb_pcr *a)
{
    TCGv addr = temp_new(ctx);
    TCGv data = get_src_regx(ctx, a->SrcL);
    tcg_gen_movi_tl(addr, ctx->base.pc_next + a->imm);
    gen_blk_store_32_op(ctx, MO_SB, NO_ADDR, addr, data, BYTES_LB_SB, 0);
    return true;
}

static bool trans_l_sh_pcr(DisasContext *ctx, arg_l_sh_pcr *a)
{
    TCGv addr = temp_new(ctx);
    TCGv data = get_src_regx(ctx, a->SrcL);
    tcg_gen_movi_tl(addr, ctx->base.pc_next + a->imm);
    gen_blk_store_32_op(ctx, MO_SW, NO_ADDR, addr, data, BYTES_LH_SH, 0);
    return true;
}

static bool trans_l_sw_pcr(DisasContext *ctx, arg_l_sw_pcr *a)
{
    TCGv addr = temp_new(ctx);
    TCGv data = get_src_regx(ctx, a->SrcL);
    tcg_gen_movi_tl(addr, ctx->base.pc_next + a->imm);
    gen_blk_store_32_op(ctx, MO_SL, NO_ADDR, addr, data, BYTES_LW_SW, 0);
    return true;
}

static bool trans_l_sd_pcr(DisasContext *ctx, arg_l_sd_pcr *a)
{
    TCGv addr = temp_new(ctx);
    TCGv data = get_src_regx(ctx, a->SrcL);
    tcg_gen_movi_tl(addr, ctx->base.pc_next + a->imm);
    gen_blk_store_32_op(ctx, MO_Q, NO_ADDR, addr, data, BYTES_LD_SD, 0);
    return true;
}

static void cas_instr(DisasContext *ctx, arg_l_casbp *a, int bytes, MemOp mtyp)
{
    TCGv addr = get_src_regx(ctx, a->SrcL);
    TCGv lo = get_src_regx(ctx, a->SrcR0);
    TCGv hi = get_src_regx(ctx, a->SrcR1);
    TCGv dlo = get_src_regx(ctx, a->SrcD0);
    TCGv dhi = get_src_regx(ctx, a->SrcD0);
    TCGv dest0 = get_dst_regx(ctx, a->RegDst0);
    TCGv dest1 = get_dst_regx(ctx, a->RegDst1);
    TCGv cmp = temp_new(ctx);
    TCGv srcd = temp_new(ctx);

    tcg_gen_deposit_i64(cmp, lo, hi, bytes * 8, bytes * 8);
    tcg_gen_deposit_i64(srcd, dlo, dhi, bytes * 8, bytes * 8);
    if (a->rl) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_STRL);
    }
    tcg_gen_atomic_cmpxchg_tl(dest0, addr, cmp, srcd, ctx->mem_idx, mtyp);
    if (a->aq) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_LDAQ);
    }

    tcg_gen_qemu_ld_tl(dest0, addr, ctx->mem_idx, mtyp);
    tcg_gen_addi_tl(srcd, addr, bytes);
    tcg_gen_qemu_ld_tl(dest1, srcd, ctx->mem_idx, mtyp);
    return;
}

static bool trans_l_casbp(DisasContext *ctx, arg_l_casbp *a)
{
    TCGv addr = get_src_regx(ctx, a->SrcL);
    TCGv lo = get_src_regx(ctx, a->SrcR0);
    TCGv hi = get_src_regx(ctx, a->SrcR1);
    TCGv dlo = get_src_regx(ctx, a->SrcD0);
    TCGv dhi = get_src_regx(ctx, a->SrcD0);
    TCGv dest0 = get_dst_regx(ctx, a->RegDst0);
    TCGv dest1 = get_dst_regx(ctx, a->RegDst1);
    TCGv cmp = temp_new(ctx);
    TCGv srcd = temp_new(ctx);

    tcg_gen_deposit_i64(cmp, lo, hi, 8, 8);
    tcg_gen_deposit_i64(srcd, dlo, dhi, 8, 8);
    if (a->rl) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_STRL);
    }
    tcg_gen_atomic_cmpxchg_tl(dest0, addr, cmp, srcd, ctx->mem_idx, MO_TEUW);
    if (a->aq) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_LDAQ);
    }

    tcg_gen_qemu_ld_tl(dest0, addr, ctx->mem_idx, MO_UB);
    tcg_gen_addi_tl(srcd, addr, 1);
    tcg_gen_qemu_ld_tl(dest1, srcd, ctx->mem_idx, MO_UB);
    return true;
}

static bool trans_l_cashp(DisasContext *ctx, arg_l_cashp *a)
{
    cas_instr(ctx, a, BYTES_LH_SH, MO_TEUL);
    return true;
}

static bool trans_l_caswp(DisasContext *ctx, arg_l_caswp *a)
{
    cas_instr(ctx, a, BYTES_LW_SW, MO_Q);
    return true;
}

static bool trans_l_casdp(DisasContext *ctx, arg_l_casdp *a)
{
    TCGv addr = get_src_regx(ctx, a->SrcL);
    TCGv s1 = get_src_regx(ctx, a->SrcR0);
    TCGv s2 = get_src_regx(ctx, a->SrcR1);
    TCGv t1 = get_src_regx(ctx, a->SrcD0);
    TCGv t2 = get_src_regx(ctx, a->SrcD0);

    TCGv d1 = get_dst_regx(ctx, a->RegDst0);
    TCGv d2 = get_dst_regx(ctx, a->RegDst1);

    TCGv_i64 a2 = tcg_temp_new_i64();
    TCGv_i64 c1 = tcg_temp_new_i64();
    TCGv_i64 c2 = tcg_temp_new_i64();
    TCGv_i64 zero = tcg_const_i64(0);

    /* Load the two words, in memory order.  */
    tcg_gen_qemu_ld_i64(d1, addr, ctx->mem_idx,
                        MO_TEQ);
    tcg_gen_mov_tl(t1, d1);
    tcg_gen_addi_i64(a2, addr, 8);
    tcg_gen_qemu_ld_i64(d2, a2, ctx->mem_idx, MO_TEQ);
    tcg_gen_mov_tl(t2, d2);

    /* Compare the two words, also in memory order.  */
    tcg_gen_setcond_i64(TCG_COND_EQ, c1, d1, s1);
    tcg_gen_setcond_i64(TCG_COND_EQ, c2, d2, s2);
    tcg_gen_and_i64(c2, c2, c1);

    /* If compare equal, write back new data, else write back old data.  */
    tcg_gen_movcond_i64(TCG_COND_NE, c1, c2, zero, t1, d1);
    tcg_gen_movcond_i64(TCG_COND_NE, c2, c2, zero, t2, d2);

    tcg_gen_qemu_st_i64(c1, addr, ctx->mem_idx, MO_TEQ);
    tcg_gen_qemu_st_i64(c2, a2, ctx->mem_idx, MO_TEQ);

    tcg_temp_free_i64(a2);
    tcg_temp_free_i64(c1);
    tcg_temp_free_i64(c2);
    tcg_temp_free_i64(zero);
    return true;
}

static bool  trans_simt_l_lb_brg(DisasContext *ctx, arg_simt_l_lb_brg *a)
{
    gen_simt_load_32(ctx, MO_SB, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool  trans_simt_l_lh_brg(DisasContext *ctx, arg_simt_l_lh_brg *a)
{
    gen_simt_load_32(ctx, MO_TESW, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LH_SH, NO_LC0);
    return true;
}
static bool  trans_simt_l_lw_brg(DisasContext *ctx, arg_simt_l_lw_brg *a)
{
    gen_simt_load_32(ctx, MO_TESL, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LW_SW, NO_LC0);
    return true;
}
static bool  trans_simt_l_ld_brg(DisasContext *ctx, arg_simt_l_ld_brg *a)
{
    gen_simt_load_32(ctx, MO_TEQ, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LD_SD, NO_LC0);
    return true;
}
static bool  trans_simt_l_lbu_brg(DisasContext *ctx, arg_simt_l_lbu_brg *a)
{
    gen_simt_load_32(ctx, MO_UB, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LB_SB, NO_LC0);
    return true;
}
static bool  trans_simt_l_lhu_brg(DisasContext *ctx, arg_simt_l_lhu_brg *a)
{
    gen_simt_load_32(ctx, MO_TEUW, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_lwu_brg(DisasContext *ctx, arg_simt_l_lwu_brg *a)
{
    gen_simt_load_32(ctx, MO_TEUL, NO_ADDR, a->shamt,
                    a->SrcR, a->SrcL, a->l, a->RegDst, OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_lbi_brg(DisasContext *ctx, arg_simt_l_lbi_brg *a)
{
    gen_simt_load_32_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LB_SB, OFFSET_LB_SB, NO_LC0);
    return true;
}

static bool  trans_simt_l_lhi_brg(DisasContext *ctx, arg_simt_l_lhi_brg *a)
{
    gen_simt_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LH_SH, OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_lwi_brg(DisasContext *ctx, arg_simt_l_lwi_brg *a)
{
    gen_simt_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LW_SW, OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_ldi_brg(DisasContext *ctx, arg_simt_l_ldi_brg *a)
{
    gen_simt_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LD_SD, OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool  trans_simt_l_lbui_brg(DisasContext *ctx, arg_simt_l_lbui_brg *a)
{
    gen_simt_load_32_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LB_SB, OFFSET_LB_SB, NO_LC0);
    return true;
}

static bool  trans_simt_l_lhui_brg(DisasContext *ctx, arg_simt_l_lhui_brg *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LH_SH, OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_lwui_brg(DisasContext *ctx, arg_simt_l_lwui_brg *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, OFFSET_LW_SW, OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_lhi_ubrg(DisasContext *ctx, arg_simt_l_lhi_ubrg *a)
{
    gen_simt_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_lwi_ubrg(DisasContext *ctx, arg_simt_l_lwi_ubrg *a)
{
    gen_simt_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL, a->l,
                         a->RegDst, UNSCALED, OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_ldi_ubrg(DisasContext *ctx, arg_simt_l_ldi_ubrg *a)
{
    gen_simt_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool  trans_simt_l_lhui_ubrg(DisasContext *ctx, arg_simt_l_lhui_ubrg *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_lwui_ubrg(DisasContext *ctx, arg_simt_l_lwui_ubrg *a)
{
    gen_simt_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL, a->l,
                        a->RegDst, UNSCALED, OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_sb_brg(DisasContext *ctx, arg_simt_l_sb_brg *a)
{
    gen_simt_store_32(ctx, MO_SB, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LB_SB, OFFSET_LB_SB + a->shamt,
                      OFFSET_LB_SB, NO_LC0);
    return true;
}

static bool  trans_simt_l_sh_brg(DisasContext *ctx, arg_simt_l_sh_brg *a)
{
    gen_simt_store_32(ctx, MO_TESW, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LH_SH, OFFSET_LH_SH + a->shamt,
                      OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_sw_brg(DisasContext *ctx, arg_simt_l_sw_brg *a)
{
    gen_simt_store_32(ctx, MO_TESL, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LW_SW, OFFSET_LW_SW + a->shamt,
                      OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_sd_brg(DisasContext *ctx, arg_simt_l_sd_brg *a)
{
    gen_simt_store_32(ctx, MO_TEQ, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LD_SD, OFFSET_LD_SD + a->shamt,
                      OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool  trans_simt_l_sh_ubrg(DisasContext *ctx, arg_simt_l_sh_ubrg *a)
{
    gen_simt_store_32(ctx, MO_TESW, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LH_SH, a->shamt,
                      OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_sw_ubrg(DisasContext *ctx, arg_simt_l_sw_ubrg *a)
{
    gen_simt_store_32(ctx, MO_TESL, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LW_SW, a->shamt,
                      OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_sd_ubrg(DisasContext *ctx, arg_simt_l_sd_ubrg *a)
{
    gen_simt_store_32(ctx, MO_TEQ, a->SrcD, a->SrcR, a->SrcL,
                      a->l, BYTES_LD_SD, a->shamt,
                      OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool  trans_simt_l_sbi_brg(DisasContext *ctx, arg_simt_l_sbi_brg *a)
{
    gen_simt_store_32_imm(ctx, MO_SB, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LB_SB, OFFSET_LB_SB, OFFSET_LB_SB, NO_LC0);
    return true;
}

static bool  trans_simt_l_shi_brg(DisasContext *ctx, arg_simt_l_shi_brg *a)
{
    gen_simt_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LH_SH, OFFSET_LH_SH, OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_swi_brg(DisasContext *ctx, arg_simt_l_swi_brg *a)
{
    gen_simt_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LW_SW, OFFSET_LW_SW, OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_sdi_brg(DisasContext *ctx, arg_simt_l_sdi_brg *a)
{
    gen_simt_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LD_SD, OFFSET_LD_SD, OFFSET_LD_SD, NO_LC0);
    return true;
}

static bool  trans_simt_l_shi_ubrg(DisasContext *ctx, arg_simt_l_shi_ubrg *a)
{
    gen_simt_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LH_SH, UNSCALED, OFFSET_LH_SH, NO_LC0);
    return true;
}

static bool  trans_simt_l_swi_ubrg(DisasContext *ctx, arg_simt_l_swi_ubrg *a)
{
    gen_simt_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LW_SW, UNSCALED, OFFSET_LW_SW, NO_LC0);
    return true;
}

static bool  trans_simt_l_sdi_ubrg(DisasContext *ctx, arg_simt_l_sdi_ubrg *a)
{
    gen_simt_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, a->l,
        BYTES_LD_SD, UNSCALED, OFFSET_LD_SD, NO_LC0);
    return true;
}
