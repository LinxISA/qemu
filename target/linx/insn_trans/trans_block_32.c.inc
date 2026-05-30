/*
 * Linx Block ISA translation.
 *
 * Copyright (c) 2021-2023 Hisilicon Technologies.
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

static bool trans_blk_invalid_32(DisasContext *ctx, arg_blk_invalid_32 *a)
{
    gen_exception_illegal(ctx);
    return true;
}

/* 32 bit encoding for body */
static inline bool gen_blk_arith_e(DisasContext *ctx, arg_blk_add_32 *a, int type,
                      void (*func)(TCGv, TCGv, TCGv))
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    src_r = conver_src_value(ctx, src_r, type, a->SrcRType);
    tcg_gen_shli_tl(src_r, src_r, a->shamt);

    func(dest, src_l, src_r);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_add_32(DisasContext *ctx, arg_blk_add_32 *a)
{
    return gen_blk_arith_e(ctx, a, INSTR_TYPE_AU, tcg_gen_add_tl);
}

static bool trans_blk_sub_32(DisasContext *ctx, arg_blk_sub_32 *a)
{
    return gen_blk_arith_e(ctx, a, INSTR_TYPE_AU, tcg_gen_sub_tl);
}

static bool trans_blk_and_32(DisasContext *ctx, arg_blk_and_32 *a)
{
    return gen_blk_arith_e(ctx, a, INSTR_TYPE_LU, tcg_gen_and_tl);
}

static bool trans_blk_or_32(DisasContext *ctx, arg_blk_or_32 *a)
{
    return gen_blk_arith_e(ctx, a, INSTR_TYPE_LU, tcg_gen_or_tl);
}

static bool trans_blk_xor_32(DisasContext *ctx, arg_blk_xor_32 *a)
{
    return gen_blk_arith_e(ctx, a, INSTR_TYPE_LU, tcg_gen_xor_tl);
}

static bool gen_blk_shift_32(DisasContext *ctx, arg_blk_srl_32 *a,
                      void (*func)(TCGv, TCGv, TCGv))
{
    TCGv dest, src_l, src_r, lo;
    src_l = get_src_regx(ctx, a->SrcL);
    src_r = get_src_regx(ctx, a->SrcR);
    dest = get_dst_regx(ctx, a->RegDst);
    lo = temp_new(ctx);

    tcg_gen_andi_tl(lo, src_r, 0x3f);
    func(dest, src_l, lo);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_srl_32(DisasContext *ctx, arg_blk_srl_32 *a)
{
    return gen_blk_shift_32(ctx, a, tcg_gen_shr_tl);
}

static bool trans_blk_sra_32(DisasContext *ctx, arg_blk_sra_32 *a)
{
    return gen_blk_shift_32(ctx, a, tcg_gen_sar_tl);
}

static bool trans_blk_sll_32(DisasContext *ctx, arg_blk_sll_32 *a)
{
    return gen_blk_shift_32(ctx, a, tcg_gen_shl_tl);
}

static bool trans_blk_addi_32(DisasContext *ctx, arg_blk_addi_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_addi_tl(dest, src_l, a->imm);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_subi_32(DisasContext *ctx, arg_blk_subi_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_subi_tl(dest, src_l, a->imm);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_andi_32(DisasContext *ctx, arg_blk_andi_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    //TCGv  = tcg_constant_tl(12);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_andi_tl(dest, src_l, a->imm);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_ori_32(DisasContext *ctx, arg_blk_ori_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_ori_tl(dest, src_l, a->imm);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_xori_32(DisasContext *ctx, arg_blk_xori_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_xori_tl(dest, src_l, a->imm);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_srli_32(DisasContext *ctx, arg_blk_srli_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_shri_tl(dest, src_l, a->shamt);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_srai_32(DisasContext *ctx, arg_blk_srai_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_sari_tl(dest, src_l, a->shamt);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_slli_32(DisasContext *ctx, arg_blk_slli_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_shli_tl(dest, src_l, a->shamt);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}


static inline bool gen_blk_arith_w(DisasContext *ctx, arg_blk_addw_32 *a,
                                   int type, void (*func)(TCGv, TCGv, TCGv))
{
    TCGv dest, src_l, src_r;
    src_l = get_src_regx(ctx, a->SrcL);
    src_r = get_src_regx(ctx, a->SrcR);
    dest = get_dst_regx(ctx, a->RegDst);
    src_r = conver_src_value(ctx, src_r, type, a->SrcRType);
    tcg_gen_shli_tl(src_r, src_r, a->shamt);

    func(dest, src_l, src_r);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_addw_32(DisasContext *ctx, arg_blk_addw_32 *a)
{
    return gen_blk_arith_w(ctx, a, INSTR_TYPE_AU, tcg_gen_add_tl);
}

static bool trans_blk_subw_32(DisasContext *ctx, arg_blk_subw_32 *a)
{
    return gen_blk_arith_w(ctx, a, INSTR_TYPE_AU, tcg_gen_sub_tl);
}

static bool trans_blk_andw_32(DisasContext *ctx, arg_blk_andw_32 *a)
{
    return gen_blk_arith_w(ctx, a, INSTR_TYPE_LU, tcg_gen_and_tl);
}

static bool trans_blk_orw_32(DisasContext *ctx, arg_blk_orw_32 *a)
{
    return gen_blk_arith_w(ctx, a, INSTR_TYPE_LU, tcg_gen_or_tl);
}

static bool trans_blk_xorw_32(DisasContext *ctx, arg_blk_xorw_32 *a)
{
    return gen_blk_arith_w(ctx, a, INSTR_TYPE_LU, tcg_gen_xor_tl);
}

static bool gen_blk_shift_w(DisasContext *ctx, arg_blk_srlw_32 *a,
                      DisasExtend ext, void (*func)(TCGv, TCGv, TCGv))
{
    TCGv dest, src_l, src_r, lo;
    src_l = get_src_reg_extx(ctx, a->SrcL, ext);
    src_r = get_src_regx(ctx, a->SrcR);
    dest = get_dst_regx(ctx, a->RegDst);
    lo = temp_new(ctx);

    tcg_gen_andi_tl(lo, src_r, 0x1f);
    func(dest, src_l, lo);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src_l);
    return true;
}

static bool trans_blk_srlw_32(DisasContext *ctx, arg_blk_srlw_32 *a)
{
    return gen_blk_shift_w(ctx, a, EXT_ZERO, tcg_gen_shr_tl);
}

static bool trans_blk_sraw_32(DisasContext *ctx, arg_blk_sraw_32 *a)
{
    return gen_blk_shift_w(ctx, a, EXT_SIGN, tcg_gen_sar_tl);
}

static bool trans_blk_sllw_32(DisasContext *ctx, arg_blk_sllw_32 *a)
{
    return gen_blk_shift_w(ctx, a, EXT_NONE, tcg_gen_shl_tl);
}

static bool trans_blk_addiw_32(DisasContext *ctx, arg_blk_addiw_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_addi_tl(dest, src_l, a->imm);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_subiw_32(DisasContext *ctx, arg_blk_subiw_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_subi_tl(dest, src_l, a->imm);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_andiw_32(DisasContext *ctx, arg_blk_oriw_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_andi_tl(dest, src_l, a->imm);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_oriw_32(DisasContext *ctx, arg_blk_oriw_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_ori_tl(dest, src_l, a->imm);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_xoriw_32(DisasContext *ctx, arg_blk_xoriw_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_xori_tl(dest, src_l, a->imm);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_srliw_32(DisasContext *ctx, arg_blk_srliw_32 *a)
{
    TCGv src_l = get_src_reg_extx(ctx, a->SrcL, EXT_ZERO);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_shri_tl(dest, src_l, a->shamt);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src_l);
    return true;
}

static bool trans_blk_sraiw_32(DisasContext *ctx, arg_blk_sraiw_32 *a)
{
    TCGv src_l = get_src_reg_extx(ctx, a->SrcL, EXT_SIGN);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_sari_tl(dest, src_l, a->shamt);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src_l);
    return true;
}

static bool trans_blk_slliw_32(DisasContext *ctx, arg_blk_slliw_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_shli_tl(dest, src_l, a->shamt);

    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool blk_gen_cmp(DisasContext *ctx, arg_blk_cmp_eq_32 *a, int type,
                        TCGCond cond)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    src2 = conver_src_value(ctx, src2, type, a->SrcRType);

    tcg_gen_setcond_tl(cond, dest, src1, src2);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_cmp_eq_32(DisasContext *ctx, arg_blk_cmp_eq_32 *a)
{
    return blk_gen_cmp(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_EQ);
}

static bool trans_blk_cmp_ne_32(DisasContext *ctx, arg_blk_cmp_ne_32 *a)
{
    return blk_gen_cmp(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_NE);
}

static bool trans_blk_cmp_lt_32(DisasContext *ctx, arg_blk_cmp_lt_32 *a)
{
    return blk_gen_cmp(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_LT);
}

static bool trans_blk_cmp_ge_32(DisasContext *ctx, arg_blk_cmp_ge_32 *a)
{
    return blk_gen_cmp(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_GE);
}

static bool trans_blk_cmp_ltu_32(DisasContext *ctx, arg_blk_cmp_ltu_32 *a)
{
    return blk_gen_cmp(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_LTU);
}

static bool trans_blk_cmp_geu_32(DisasContext *ctx, arg_blk_cmp_geu_32 *a)
{
    return blk_gen_cmp(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_GEU);
}

static bool gen_blk_cmp_arith(DisasContext *ctx, arg_blk_cmp_and_32 *a,
                         int type, void (*func)(TCGv, TCGv, TCGv))
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    src_r = conver_src_value(ctx, src_r, type, a->SrcRType);

    TCGv res = temp_new(ctx);
    func(res, src_l, src_r);

    tcg_gen_setcondi_tl(TCG_COND_NE, dest, res, 0);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_cmp_and_32(DisasContext *ctx, arg_blk_cmp_and_32 *a)
{
    return gen_blk_cmp_arith(ctx, a, INSTR_TYPE_LU, tcg_gen_and_tl);
}

static bool trans_blk_cmp_or_32(DisasContext *ctx, arg_blk_cmp_or_32 *a)
{
    return gen_blk_cmp_arith(ctx, a, INSTR_TYPE_LU, tcg_gen_or_tl);
}

static bool blk_gen_cmpi(DisasContext *ctx, int SrcL, int imm, int regDst,
                         TCGCond cond)
{
    TCGv src1 = get_src_regx(ctx, SrcL);
    TCGv src2 = tcg_constant_tl(imm);
    TCGv dest = get_dst_regx(ctx, regDst);

    tcg_gen_setcond_tl(cond, dest, src1, src2);

    set_dst_regx(ctx, dest, regDst);

    return true;
}

static bool trans_blk_cmp_eqi_32(DisasContext *ctx, arg_blk_cmp_eqi_32 *a)
{
    return blk_gen_cmpi(ctx, a->SrcL, a->imm, a->RegDst, TCG_COND_EQ);
}

static bool trans_blk_cmp_nei_32(DisasContext *ctx, arg_blk_cmp_nei_32 *a)
{
    return blk_gen_cmpi(ctx, a->SrcL, a->imm, a->RegDst, TCG_COND_NE);
}

static bool trans_blk_cmp_lti_32(DisasContext *ctx, arg_blk_cmp_lti_32 *a)
{
    return blk_gen_cmpi(ctx, a->SrcL, a->imm, a->RegDst, TCG_COND_LT);
}

static bool trans_blk_cmp_gei_32(DisasContext *ctx, arg_blk_cmp_gei_32 *a)
{
    return blk_gen_cmpi(ctx, a->SrcL, a->imm, a->RegDst, TCG_COND_GE);
}

static bool trans_blk_cmp_ltui_32(DisasContext *ctx, arg_blk_cmp_ltui_32 *a)
{
    return blk_gen_cmpi(ctx, a->SrcL, a->imm, a->RegDst, TCG_COND_LTU);
}

static bool trans_blk_cmp_geui_32(DisasContext *ctx, arg_blk_cmp_geui_32 *a)
{
    return blk_gen_cmpi(ctx, a->SrcL, a->imm, a->RegDst, TCG_COND_GEU);
}

static bool gen_blk_cmp_arith_i(DisasContext *ctx, arg_blk_cmp_andi_32 *a,
                         void (*func)(TCGv, TCGv, int64_t))
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    TCGv res = temp_new(ctx);
    func(res, src1, a->imm);

    tcg_gen_setcondi_tl(TCG_COND_NE, dest, res, 0);
    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_cmp_andi_32(DisasContext *ctx, arg_blk_cmp_andi_32 *a)
{
    return gen_blk_cmp_arith_i(ctx, a, tcg_gen_andi_tl);
}

static bool trans_blk_cmp_ori_32(DisasContext *ctx, arg_blk_cmp_ori_32 *a)
{
    return gen_blk_cmp_arith_i(ctx, a, tcg_gen_ori_tl);
}

static bool blk_gen_setc(DisasContext *ctx, arg_blk_setc_eq_32 *a, int type,
                         TCGCond cond)
{
    allow_setc_writer(ctx);

    TCGv jump = temp_new(ctx);
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    src2 = conver_src_value(ctx, src2, type, a->SrcRType);

    tcg_gen_movi_tl(carg_flag, BSTATE_CARG_CLEAN);
    tcg_gen_setcond_tl(cond, jump, src1, src2);
    tcg_gen_shli_tl(jump, jump, CARG_FLAG_PREDICATE_BIT);
    tcg_gen_or_tl(carg_flag, jump, carg_flag);
    return true;
}

static bool trans_blk_setc_eq_32(DisasContext *ctx, arg_blk_setc_eq_32 *a)
{
    return blk_gen_setc(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_EQ);
}

static bool trans_blk_setc_ne_32(DisasContext *ctx, arg_blk_setc_ne_32 *a)
{
    return blk_gen_setc(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_NE);
}

static bool trans_blk_setc_lt_32(DisasContext *ctx, arg_blk_setc_lt_32 *a)
{
    return blk_gen_setc(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_LT);
}

static bool trans_blk_setc_ge_32(DisasContext *ctx, arg_blk_setc_ge_32 *a)
{
    return blk_gen_setc(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_GE);
}

static bool trans_blk_setc_ltu_32(DisasContext *ctx, arg_blk_setc_ltu_32 *a)
{
    return blk_gen_setc(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_LTU);
}

static bool trans_blk_setc_geu_32(DisasContext *ctx, arg_blk_setc_geu_32 *a)
{
    return blk_gen_setc(ctx, a, INSTR_TYPE_CMP_SETC_LD_ST, TCG_COND_GEU);
}

static bool blk_gen_setc_arith(DisasContext *ctx, arg_blk_setc_and_32 *a,
                                void (*func)(TCGv, TCGv, TCGv))
{
    allow_setc_writer(ctx);

    TCGv jump = temp_new(ctx);
    TCGv res = temp_new(ctx);
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    src2 = conver_src_value(ctx, src2, INSTR_TYPE_LU, a->SrcRType);

    tcg_gen_movi_tl(carg_flag, BSTATE_CARG_CLEAN);

    func(res, src1, src2);

    tcg_gen_setcondi_tl(TCG_COND_NE, jump, res, 0);
    tcg_gen_shli_tl(jump, jump, CARG_FLAG_PREDICATE_BIT);
    tcg_gen_or_tl(carg_flag, jump, carg_flag);
    return true;
}

static bool trans_blk_setc_and_32(DisasContext *ctx, arg_blk_setc_and_32 *a)
{
    return blk_gen_setc_arith(ctx, a, tcg_gen_and_tl);
}

static bool trans_blk_setc_or_32(DisasContext *ctx, arg_blk_setc_or_32 *a)
{
    return blk_gen_setc_arith(ctx, a, tcg_gen_or_tl);
}

static bool gen_blk_setci(DisasContext *ctx, arg_blk_setc_eqi_32 *a,
                          TCGCond cond)
{
    allow_setc_writer(ctx);

    TCGv jump = temp_new(ctx);
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = tcg_constant_tl(a->imm);
    tcg_gen_shli_tl(src2, src2, a->shamt);

    tcg_gen_movi_tl(carg_flag, BSTATE_CARG_CLEAN);
    tcg_gen_setcond_tl(cond, jump, src1, src2);
    tcg_gen_shli_tl(jump, jump, CARG_FLAG_PREDICATE_BIT);
    tcg_gen_or_tl(carg_flag, jump, carg_flag);
    return true;
}

static bool trans_blk_setc_eqi_32(DisasContext *ctx, arg_blk_setc_eqi_32 *a)
{
    return gen_blk_setci(ctx, a, TCG_COND_EQ);
}

static bool trans_blk_setc_nei_32(DisasContext *ctx, arg_blk_setc_nei_32 *a)
{
    return gen_blk_setci(ctx, a, TCG_COND_NE);
}

static bool trans_blk_setc_lti_32(DisasContext *ctx, arg_blk_setc_lti_32 *a)
{
    return gen_blk_setci(ctx, a, TCG_COND_LT);
}

static bool trans_blk_setc_gei_32(DisasContext *ctx, arg_blk_setc_gei_32 *a)
{
    return gen_blk_setci(ctx, a, TCG_COND_GE);
}

static bool trans_blk_setc_ltui_32(DisasContext *ctx, arg_blk_setc_ltui_32 *a)
{
    return gen_blk_setci(ctx, a, TCG_COND_LTU);
}

static bool trans_blk_setc_geui_32(DisasContext *ctx, arg_blk_setc_geui_32 *a)
{
    return gen_blk_setci(ctx, a, TCG_COND_GEU);
}

static bool blk_gen_setc_arith_i(DisasContext *ctx, arg_blk_setc_andi_32 *a,
                                void (*func)(TCGv, TCGv, int64_t))
{
    allow_setc_writer(ctx);

    TCGv jump = temp_new(ctx);
    TCGv res = temp_new(ctx);
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    int64_t imm = (int64_t)a->imm << a->shamt;

    tcg_gen_movi_tl(carg_flag, BSTATE_CARG_CLEAN);

    func(res, src1, imm);

    tcg_gen_setcondi_tl(TCG_COND_NE, jump, res, 0);
    tcg_gen_shli_tl(jump, jump, CARG_FLAG_PREDICATE_BIT);
    tcg_gen_or_tl(carg_flag, jump, carg_flag);
    return true;
}

static bool trans_blk_setc_andi_32(DisasContext *ctx, arg_blk_setc_andi_32 *a)
{
    return blk_gen_setc_arith_i(ctx, a, tcg_gen_andi_tl);
}

static bool trans_blk_setc_ori_32(DisasContext *ctx, arg_blk_setc_ori_32 *a)
{
    return blk_gen_setc_arith_i(ctx, a, tcg_gen_ori_tl);
}

static bool trans_blk_bxs_32(DisasContext *ctx, arg_blk_bxs_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv tmp = temp_new(ctx);

    tcg_gen_rotri_tl(tmp, src1, a->M);
    tcg_gen_sextract_tl(dest, tmp, 0, a->N + 1);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_bxu_32(DisasContext *ctx, arg_blk_bxu_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv tmp = temp_new(ctx);

    tcg_gen_rotri_tl(tmp, src1, a->M);
    tcg_gen_extract_tl(dest, tmp, 0, a->N + 1);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_bic_32(DisasContext *ctx, arg_blk_bic_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv tmp = temp_new(ctx);
    /* 生成第M位后N位连续为0，其他位全为1的掩码 */
    if(a->N < 63) {
        TCGv mask0 = tcg_const_tl(~(((1ULL << (a->N + 1)) - 1) << a->M));
        tcg_gen_and_tl(tmp, src1, mask0);
        if(a->N + a->M - 63 > 0) {
            TCGv mask1 = tcg_const_tl(~(((1ULL << (a->N + (a->M - 63))) - 1) ));
            tcg_gen_and_tl(tmp, tmp, mask1);
        }
        tcg_gen_mov_tl(dest, tmp);
    }
    else {
        TCGv mask2 = tcg_const_tl(0ULL);
        tcg_gen_and_tl(tmp, src1, mask2);
        tcg_gen_mov_tl(dest, tmp);
    }
    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_bis_32(DisasContext *ctx, arg_blk_bis_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv tmp = temp_new(ctx);
    /* 生成第M位后N位连续为1，其他位全为0的掩码 */
    if(a->N < 63) {
        TCGv mask0 = tcg_const_tl(((1ULL << (a->N + 1)) - 1) << a->M);
        tcg_gen_or_tl(tmp, src1, mask0);
        if(a->N + a->M - 63 > 0) {
            TCGv mask1 = tcg_const_tl((((1ULL << (a->N + (a->M - 63))) - 1) ));
            tcg_gen_or_tl(tmp, tmp, mask1);
        }
        tcg_gen_mov_tl(dest, tmp);
    }
    else {
        TCGv mask2 = tcg_const_tl(0xffffffffffffffff);
        tcg_gen_or_tl(tmp, src1, mask2);
        tcg_gen_mov_tl(dest, tmp);
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_ctz_32(DisasContext *ctx, arg_blk_ctz_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    uint8_t M = a->M, N = a->N + 1;

    uint64_t mask = (N >= 64) ? 0ULL : ~((1ULL << N) - 1);

    TCGv tmp = temp_new(ctx);
    tcg_gen_mov_tl(tmp, src1);

    if (M + N > 64) {
        tcg_gen_rotri_tl(tmp, tmp, M);
    } else {
        tcg_gen_shri_tl(tmp, tmp, M);
    }

    tcg_gen_ori_tl(tmp, tmp, mask);
    tcg_gen_ctzi_tl(dest, tmp, TARGET_LONG_BITS);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_clz_32(DisasContext *ctx, arg_blk_clz_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    uint8_t M = a->M, N = a->N + 1;

    uint8_t shift = TARGET_LONG_BITS - N;
    uint64_t mask = (N >= 64) ? 0ULL : ((1ULL << shift) - 1);

    TCGv tmp = temp_new(ctx);
    tcg_gen_mov_tl(tmp, src1);

    if (M + N > 64) {
        tcg_gen_rotri_tl(tmp, tmp, M);
    } else {
        tcg_gen_shri_tl(tmp, tmp, M);
    }
    tcg_gen_shli_tl(tmp, tmp, shift);

    tcg_gen_ori_tl(tmp, tmp, mask);
    tcg_gen_clzi_tl(dest, tmp, TARGET_LONG_BITS);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_bcnt_32(DisasContext *ctx, arg_blk_bcnt_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    uint8_t M = a->M, N = a->N + 1;

    uint64_t mask = (N >= 64) ? ~0ULL : ((1ULL << N) - 1);

    TCGv tmp = temp_new(ctx);
    tcg_gen_mov_tl(tmp, src1);

    if (M + N > 64) {
        tcg_gen_rotri_tl(tmp, tmp, M);
    } else {
        tcg_gen_shri_tl(tmp, tmp, M);
    }

    tcg_gen_andi_tl(tmp, tmp, mask);
    tcg_gen_ctpop_tl(dest, tmp);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_rev_32(DisasContext *ctx, arg_blk_rev_32 *a)
{
    uint32_t M = a->M + 1, N = a->N;

    if (N > M) {
        gen_exception_illegal(ctx);
        return false;
    }

    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_helper_reverse_bits(dest, src1, tcg_constant_i32(M),
                            tcg_constant_i32(N));

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_csel_32(DisasContext *ctx, arg_blk_csel_32 *a)
{
    /* v0.56 compiler lowering expects pred != 0 ? SrcR : SrcL. */
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    /* .neg field */
    src2 = conver_src_value(ctx, src2, INSTR_TYPE_AU, a->SrcRType);
    TCGv SrcP = get_src_regx(ctx, a->SrcP);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv zero = tcg_constant_tl(0);

    tcg_gen_movcond_tl(TCG_COND_NE, dest, SrcP, zero, src2, src1);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_mul_32(DisasContext *ctx, arg_blk_mul_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv h64 = temp_new(ctx);

    tcg_gen_muls2_tl(dest, h64, src1, src2);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_mulu_32(DisasContext *ctx, arg_blk_mulu_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv h64 = temp_new(ctx);

    tcg_gen_mulu2_tl(dest, h64, src1, src2);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_mulw_32(DisasContext *ctx, arg_blk_mulw_32 *a)
{
    TCGv src1 = get_src_reg_extx(ctx, a->SrcL, EXT_SIGN);
    TCGv src2 = get_src_reg_extx(ctx, a->SrcR, EXT_SIGN);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_mul_tl(dest, src1, src2);
    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src1);
    tcg_temp_free(src2);
    return true;
}

static bool trans_blk_muluw_32(DisasContext *ctx, arg_blk_muluw_32 *a)
{
    TCGv src1 = get_src_reg_extx(ctx, a->SrcL, EXT_ZERO);
    TCGv src2 = get_src_reg_extx(ctx, a->SrcR, EXT_ZERO);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_mul_tl(dest, src1, src2);
    tcg_gen_ext32u_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src1);
    tcg_temp_free(src2);
    return true;
}

static bool gen_blk_div(TCGv dest, TCGv src1, TCGv src2)
{
    TCGv temp1, temp2, zero, one, mone, min;

    temp1 = tcg_temp_new();
    temp2 = tcg_temp_new();
    zero = tcg_constant_tl(0);
    one = tcg_constant_tl(1);
    mone = tcg_constant_tl(-1);
    min = tcg_constant_tl(1ull << (TARGET_LONG_BITS - 1));

    /*
     * If overflow, set temp2 to 1, else source2.
     * This produces the required result of min.
     */
    tcg_gen_setcond_tl(TCG_COND_EQ, temp1, src1, min);
    tcg_gen_setcond_tl(TCG_COND_EQ, temp2, src2, mone);
    tcg_gen_and_tl(temp1, temp1, temp2);
    tcg_gen_movcond_tl(TCG_COND_NE, temp2, temp1, zero, one, src2);

    /*
     * If div by zero, set temp1 to -1 and temp2 to 1 to
     * produce the required result of -1.
     */
    tcg_gen_movcond_tl(TCG_COND_EQ, temp1, src2, zero, mone, src1);
    tcg_gen_movcond_tl(TCG_COND_EQ, temp2, src2, zero, one, temp2);

    tcg_gen_div_tl(dest, temp1, temp2);

    tcg_temp_free(temp1);
    tcg_temp_free(temp2);
    return true;
}

static bool gen_blk_divu(TCGv dest, TCGv src1, TCGv src2)
{
    TCGv temp1, temp2, zero, one, max;

    temp1 = tcg_temp_new();
    temp2 = tcg_temp_new();
    zero = tcg_constant_tl(0);
    one = tcg_constant_tl(1);
    max = tcg_constant_tl(~0);

    /*
     * If div by zero, set temp1 to max and temp2 to 1 to
     * produce the required result of max.
     */
    tcg_gen_movcond_tl(TCG_COND_EQ, temp1, src2, zero, max, src1);
    tcg_gen_movcond_tl(TCG_COND_EQ, temp2, src2, zero, one, src2);
    tcg_gen_divu_tl(dest, temp1, temp2);

    tcg_temp_free(temp1);
    tcg_temp_free(temp2);
    return true;
}

/* copy the div by zero and overflow defination of rv, copy related rv code */
static bool gen_blk_rem(TCGv dest, TCGv src1, TCGv src2)
{
    TCGv temp1, temp2, zero, one, mone, min;

    temp1 = tcg_temp_new();
    temp2 = tcg_temp_new();
    zero = tcg_constant_tl(0);
    one = tcg_constant_tl(1);
    mone = tcg_constant_tl(-1);
    min = tcg_constant_tl(1ull << (TARGET_LONG_BITS - 1));

    /*
     * If overflow, set temp1 to 0, else source1.
     * This avoids a possible host trap, and produces the required result of 0.
     */
    tcg_gen_setcond_tl(TCG_COND_EQ, temp1, src1, min);
    tcg_gen_setcond_tl(TCG_COND_EQ, temp2, src2, mone);
    tcg_gen_and_tl(temp1, temp1, temp2);
    tcg_gen_movcond_tl(TCG_COND_NE, temp1, temp1, zero, zero, src1);

    /*
     * If div by zero, set temp2 to 1, else source2.
     * This avoids a possible host trap, but produces an incorrect result.
     */
    tcg_gen_movcond_tl(TCG_COND_EQ, temp2, src2, zero, one, src2);

    tcg_gen_rem_tl(temp1, temp1, temp2);

    /* If div by zero, the required result is the original dividend. */
    tcg_gen_movcond_tl(TCG_COND_EQ, dest, src2, zero, src1, temp1);

    tcg_temp_free(temp1);
    tcg_temp_free(temp2);
    return true;
}

/* copy the div by zero and overflow defination of rv, copy related rv code */
static bool gen_blk_remu(TCGv dest, TCGv src1, TCGv src2)
{
    TCGv temp, zero, one;

    temp = tcg_temp_new();
    zero = tcg_constant_tl(0);
    one = tcg_constant_tl(1);

    /*
     * If div by zero, set temp to 1, else source2.
     * This avoids a possible host trap, but produces an incorrect result.
     */
    tcg_gen_movcond_tl(TCG_COND_EQ, temp, src2, zero, one, src2);

    tcg_gen_remu_tl(temp, src1, temp);

    /* If div by zero, the required result is the original dividend. */
    tcg_gen_movcond_tl(TCG_COND_EQ, dest, src2, zero, src1, temp);

    tcg_temp_free(temp);
    return true;
}

static bool trans_blk_div_32(DisasContext *ctx, arg_blk_div_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_div(dest, src1, src2);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_divu_32(DisasContext *ctx, arg_blk_divu_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_divu(dest, src1, src2);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_divw_32(DisasContext *ctx, arg_blk_divw_32 *a)
{
    TCGv src1 = get_src_reg_extx(ctx, a->SrcL, EXT_SIGN);
    TCGv src2 = get_src_reg_extx(ctx, a->SrcR, EXT_SIGN);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_div(dest, src1, src2);
    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src1);
    tcg_temp_free(src2);
    return true;
}

static bool trans_blk_divuw_32(DisasContext *ctx, arg_blk_divuw_32 *a)
{
    TCGv src1 = get_src_reg_extx(ctx, a->SrcL, EXT_ZERO);
    TCGv src2 = get_src_reg_extx(ctx, a->SrcR, EXT_ZERO);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_divu(dest, src1, src2);
    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src1);
    tcg_temp_free(src2);
    return true;
}

static bool trans_blk_rem_32(DisasContext *ctx, arg_blk_rem_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_rem(dest, src1, src2);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_remu_32(DisasContext *ctx, arg_blk_remu_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_remu(dest, src1, src2);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_remw_32(DisasContext *ctx, arg_blk_remw_32 *a)
{
    TCGv src1 = get_src_reg_extx(ctx, a->SrcL, EXT_SIGN);
    TCGv src2 = get_src_reg_extx(ctx, a->SrcR, EXT_SIGN);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_rem(dest, src1, src2);
    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src1);
    tcg_temp_free(src2);
    return true;
}

static bool trans_blk_remuw_32(DisasContext *ctx, arg_blk_remuw_32 *a)
{
    TCGv src1 = get_src_reg_extx(ctx, a->SrcL, EXT_ZERO);
    TCGv src2 = get_src_reg_extx(ctx, a->SrcR, EXT_ZERO);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_blk_remu(dest, src1, src2);
    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);

    tcg_temp_free(src1);
    tcg_temp_free(src2);
    return true;
}

static bool trans_blk_madd_32(DisasContext *ctx, arg_blk_madd_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv src3 = get_src_regx(ctx, a->SrcD);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    TCGv res = temp_new(ctx);
    tcg_gen_mul_tl(res, src1, src2);
    tcg_gen_add_tl(dest, src3, res);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_maddw_32(DisasContext *ctx, arg_blk_maddw_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv src3 = get_src_regx(ctx, a->SrcD);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    TCGv res = temp_new(ctx);
    tcg_gen_mul_tl(res, src1, src2);
    tcg_gen_add_tl(dest, src3, res);
    tcg_gen_ext32s_tl(dest, dest);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_addtpc_32(DisasContext *ctx, arg_blk_addtpc_32 *a)
{
    /* ADDTPC carries the page-scaled HI20 part for PC-relative materialization. */
    TCGv imm = tcg_constant_tl((target_long)a->imm);
    TCGv addr = get_dst_regx(ctx, a->RegDst);
    tcg_gen_movi_tl(addr, ctx->base.pc_next & TARGET_PAGE_MASK);
    tcg_gen_add_tl(addr, addr, imm);

    set_dst_regx(ctx, addr, a->RegDst);
    return true;
}

static bool trans_blk_setret_32(DisasContext *ctx, arg_blk_setret_32 *a)
{
    /*
     * setret immediates are already byte offsets from the following PC.
     * Doubling them skips the in-block fixup window after call sites.
     */
    TCGv imm = tcg_constant_tl((target_long)a->imm);
    TCGv addr = get_dst_regx(ctx, a->RegDst);
    tcg_gen_addi_tl(addr, imm, ctx->base.pc_next);

    set_dst_regx(ctx, addr, a->RegDst);
    tcg_gen_mov_tl(next_bpc, addr);
    ctx->next_bpc_explicit = true;
    ctx->next_bpc_target = ctx->base.pc_next + (target_ulong)a->imm;
    return true;
}

static bool trans_blk_lui_32(DisasContext *ctx, arg_blk_lui_32 *a)
{
    TCGv imm = tcg_constant_i64(a->imm);
    TCGv addr = get_dst_regx(ctx, a->RegDst);
    tcg_gen_mov_tl(addr, imm);

    set_dst_regx(ctx, addr, a->RegDst);
    return true;
}

static bool gen_blk_bcmp(DisasContext *ctx, int SrcL, int SrcR, int imm,
                         TCGCond cond)
{
    TCGv src1 = get_src_regx(ctx, SrcL);
    TCGv src2 = get_src_regx(ctx, SrcR);
    target_ulong next_pc;
    TCGLabel *branch_taken = gen_new_label();

    next_pc = ctx->base.pc_next + imm;

    tcg_gen_brcond_tl(cond, src1, src2, branch_taken);

    gen_goto_tb(ctx, 0, ctx->pc_succ_insn);

    gen_set_label(branch_taken);

    gen_goto_tb(ctx, 1, next_pc);

    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_blk_b_eq_32(DisasContext *ctx, arg_blk_b_eq_32 *a)
{
    return gen_blk_bcmp(ctx, a->SrcL, a->SrcR, a->imm, TCG_COND_EQ);
}

static bool trans_blk_b_ne_32(DisasContext *ctx, arg_blk_b_ne_32 *a)
{
    return gen_blk_bcmp(ctx, a->SrcL, a->SrcR, a->imm, TCG_COND_NE);
}

static bool trans_blk_b_lt_32(DisasContext *ctx, arg_blk_b_lt_32 *a)
{
    return gen_blk_bcmp(ctx, a->SrcL, a->SrcR, a->imm, TCG_COND_LT);
}

static bool trans_blk_b_ge_32(DisasContext *ctx, arg_blk_b_ge_32 *a)
{
    return gen_blk_bcmp(ctx, a->SrcL, a->SrcR, a->imm, TCG_COND_GE);
}

static bool trans_blk_b_ltu_32(DisasContext *ctx, arg_blk_b_ltu_32 *a)
{
    return gen_blk_bcmp(ctx, a->SrcL, a->SrcR, a->imm, TCG_COND_LTU);
}

static bool trans_blk_b_geu_32(DisasContext *ctx, arg_blk_b_geu_32 *a)
{
    return gen_blk_bcmp(ctx, a->SrcL, a->SrcR, a->imm, TCG_COND_GEU);
}

static bool trans_blk_jr_32(DisasContext *ctx, arg_blk_jr_32 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv tmp = tcg_temp_local_new();
    TCGLabel *out_of_block = gen_new_label();

    tcg_gen_addi_tl(tmp, src1, a->imm);
    tcg_gen_brcond_tl(TCG_COND_LT, tmp, tpc1, out_of_block);

    tcg_gen_mov_tl(cpu_pc, tmp);
    tcg_temp_free(tmp);

    tcg_gen_lookup_and_goto_ptr();

    gen_set_label(out_of_block);
    gen_exception_illegal(ctx);

    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool gen_blk_dir_jump(DisasContext *ctx, int imm)
{
    target_ulong next_pc = ctx->base.pc_next + imm;
    gen_goto_tb(ctx, 1, next_pc);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_blk_j_32(DisasContext *ctx, arg_blk_j_32 *a)
{
    return gen_blk_dir_jump(ctx, a->imm);
}

static bool trans_blk_b_z_32(DisasContext *ctx, arg_blk_b_z_32 *a)
{
    TCGLabel *branch_taken = gen_new_label();

    tcg_gen_brcondi_tl(TCG_COND_NE, predm, 0, branch_taken);
    gen_goto_tb(ctx, 0, ctx->pc_succ_insn);

    gen_set_label(branch_taken);
    gen_blk_dir_jump(ctx, a->imm);

    return true;
}

static bool trans_blk_b_nz_32(DisasContext *ctx, arg_blk_b_nz_32 *a)
{
    TCGLabel *branch_taken = gen_new_label();

    tcg_gen_brcondi_tl(TCG_COND_EQ, predm, 0, branch_taken);
    gen_goto_tb(ctx, 0, ctx->pc_succ_insn);

    gen_set_label(branch_taken);
    gen_blk_dir_jump(ctx, a->imm);

    return true;
}

static bool trans_blk_ssrget_32(DisasContext *ctx, arg_blk_ssrget_32 *a)
{
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_csrr(dest, cpu_env, tcg_constant_i32(a->SSRID));

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_ssrset_32(DisasContext *ctx, arg_blk_ssrset_32 *a)
{
    TCGv src = get_src_regx(ctx, a->SrcL);
    gen_csrw(ctx, a->SSRID, src);

    return true;
}

static bool trans_blk_ssrswap_32(DisasContext *ctx, arg_blk_ssrswap_32 *a)
{
    TCGv src = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_helper_ssrswap(dest, cpu_env, src, tcg_constant_i32(a->SSRID));

    return true;
}

static bool trans_blk_lsrget_32(DisasContext *ctx, arg_blk_lsrget_32 *a)
{
    uint32_t flag;
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch(a->SSRID) {
    case 0:
        tcg_gen_mov_tl(dest, bpc);
        break;
    case 1:
        tcg_gen_mov_tl(dest, carg_tgt);
        break;
    case 2:
        tcg_gen_mov_tl(dest, next_bpc);
        break;
    case 3:
        flag = get_block_para(ctx);
        gen_helper_get_carg_flag(dest, carg_flag, tcg_constant_i32(flag));
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_setc_tgt_32(DisasContext *ctx, arg_blk_setc_tgt_32 *a)
{
    TCGv tgt = get_src_regx(ctx, a->SrcL);
    tcg_gen_mov_tl(carg_tgt, tgt);
    return true;
}

static void gen_blk_load_32_op(DisasContext *ctx, MemOp memop, bool is_addr,
                                TCGv addr, int dest_reg_enc)
{
    linx_block_constraint_check(ctx, CONSTRAINT_A6_4);

    const bool discard_result = (dest_reg_enc == REGDST_0);
    TCGv dest_Treg = discard_result ? temp_new(ctx) : get_dst_regx(ctx, dest_reg_enc);
    if (linx_use_atomic_store_buf(ctx)) {
        TCGv_i32 memop_t = tcg_constant_i32(memop);
        gen_helper_load_data(dest_Treg, cpu_env, addr, memop_t);
    } else {
        tcg_gen_qemu_ld_tl(dest_Treg, addr, ctx->mem_idx, memop);
    }
    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(addr, dest_Treg, tcg_constant_tl(READ));
    }

    if (discard_result) {
        tcg_temp_free(dest_Treg);
    } else if (is_addr) {
        set_dst_regx(ctx, addr, dest_reg_enc);
    } else {
        set_dst_regx(ctx, dest_Treg, dest_reg_enc);
    }
}

static void gen_blk_load_32(DisasContext *ctx, MemOp memop, bool is_addr,
                            int shamt, int SrcRType, int SrcR_enc,
                            int SrcL_enc, int dest_reg_enc)
{
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);
    TCGv SrcRreg = get_src_regx(ctx, SrcR_enc);
    TCGv shift = conver_src_value(ctx, SrcRreg, INSTR_TYPE_CMP_SETC_LD_ST, SrcRType);

    tcg_gen_shli_tl(shift, shift, shamt);

    /* shift becomes the "addr" */
    tcg_gen_add_tl(shift, addr_base, shift);

    gen_blk_load_32_op(ctx, memop, is_addr, shift, dest_reg_enc);
}

static void gen_blk_load_32_imm(DisasContext *ctx, MemOp memop, bool is_addr,
                                int imm, int SrcL_enc, int dest_reg_enc,
                                int scaled_shift)
{
    /* get SrcLreg  addr_base */
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    gen_blk_load_32_op(ctx, memop, is_addr, addr, dest_reg_enc);
}

static void gen_blk_load_32_symbol(DisasContext *ctx, MemOp memop,
                                   bool is_addr, int imm,
                                   int dest_reg_enc, int scaled_shift)
{
    TCGv addr_base = tcg_constant_tl(ctx->base.pc_next);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    gen_blk_load_32_op(ctx, memop, is_addr, addr, dest_reg_enc);
}

static bool trans_blk_lb_32(DisasContext *ctx, arg_blk_lb_32 *a)
{
    gen_blk_load_32(ctx, MO_SB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_lh_32(DisasContext *ctx, arg_blk_lh_32 *a)
{
    gen_blk_load_32(ctx, MO_TESW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_lw_32(DisasContext *ctx, arg_blk_lw_32 *a)
{
    gen_blk_load_32(ctx, MO_TESL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_ld_32(DisasContext *ctx, arg_blk_ld_32 *a)
{
    gen_blk_load_32(ctx, MO_TEQ, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_lbu_32(DisasContext *ctx, arg_blk_lbu_32 *a)
{
    gen_blk_load_32(ctx, MO_UB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_lhu_32(DisasContext *ctx, arg_blk_lhu_32 *a)
{
    gen_blk_load_32(ctx, MO_TEUW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_lwu_32(DisasContext *ctx, arg_blk_lwu_32 *a)
{
    gen_blk_load_32(ctx, MO_TEUL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_prf_32(DisasContext *ctx, arg_blk_prf_32 *a)
{
    gen_blk_load_32(ctx, MO_TEQ, NO_ADDR, a->shamt, a->SrcRType,
                    a->SrcR, a->SrcL, REGDST_0);
    return true;
}

static bool trans_blk_lb_i_32(DisasContext *ctx, arg_blk_lb_i_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lh_i_32(DisasContext *ctx, arg_blk_lh_i_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lw_i_32(DisasContext *ctx, arg_blk_lw_i_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_ld_i_32(DisasContext *ctx, arg_blk_ld_i_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, OFFSET_LD_SD);
    return true;
}

static bool trans_blk_lbu_i_32(DisasContext *ctx, arg_blk_lbu_i_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhu_i_32(DisasContext *ctx, arg_blk_lhu_i_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwu_i_32(DisasContext *ctx, arg_blk_lwu_i_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_lh_ui_32(DisasContext *ctx, arg_blk_lh_ui_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lw_ui_32(DisasContext *ctx, arg_blk_lw_ui_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                         a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_ld_ui_32(DisasContext *ctx, arg_blk_ld_ui_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lhu_ui_32(DisasContext *ctx, arg_blk_lhu_ui_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lwu_ui_32(DisasContext *ctx, arg_blk_lwu_ui_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_prfi_u_32(DisasContext *ctx, arg_blk_prfi_u_32 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        REGDST_0, UNSCALED);
    return true;
}

static bool trans_blk_lbl_32(DisasContext *ctx, arg_blk_lbl_32 *a)
{
    gen_blk_load_32_symbol(ctx, MO_SB, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lhl_32(DisasContext *ctx, arg_blk_lhl_32 *a)
{
    gen_blk_load_32_symbol(ctx, MO_TESW, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lwl_32(DisasContext *ctx, arg_blk_lwl_32 *a)
{
    gen_blk_load_32_symbol(ctx, MO_TESL, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_ldl_32(DisasContext *ctx, arg_blk_ldl_32 *a)
{
    gen_blk_load_32_symbol(ctx, MO_TEQ, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lbul_32(DisasContext *ctx, arg_blk_lbul_32 *a)
{
    gen_blk_load_32_symbol(ctx, MO_UB, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lhul_32(DisasContext *ctx, arg_blk_lhul_32 *a)
{
    gen_blk_load_32_symbol(ctx, MO_TEUW, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lwul_32(DisasContext *ctx, arg_blk_lwul_32 *a)
{
    gen_blk_load_32_symbol(ctx, MO_TEUL, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static void gen_blk_store_32_op(DisasContext *ctx, MemOp memop, bool is_addr,
                                TCGv dest, TCGv data, int width, int RegDst)
{
    linx_block_constraint_check(ctx, CONSTRAINT_A6_4);

    if (linx_use_atomic_store_buf(ctx)) {
        TCGv_i32 size = tcg_constant_i32(width);
        gen_helper_store_data(cpu_env, dest, data, size);
    } else {
        tcg_gen_qemu_st_tl(data, dest, ctx->mem_idx, memop);
    }

    if (is_addr) {
        set_dst_regx(ctx, dest, RegDst);
    }
    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(dest, data, tcg_constant_tl(WRITE));
    }
}

static void gen_blk_store_32(DisasContext *ctx, MemOp memop,
                             int SrcD, int SrcRType,
                             int SrcR, int SrcL,
                             int width, int scaled_shift,
                             bool is_addr, int RegDst)
{
    TCGv addr_base = get_src_regx(ctx, SrcL);
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv shift_base = get_src_regx(ctx, SrcR);
    TCGv shift = conver_src_value(ctx, shift_base, INSTR_TYPE_CMP_SETC_LD_ST, SrcRType);

    tcg_gen_shli_tl(shift, shift, scaled_shift);

    /* shift become dest */
    tcg_gen_add_tl(shift, addr_base, shift);

    gen_blk_store_32_op(ctx, memop, is_addr, shift, data, width, RegDst);
}

static void gen_blk_store_32_imm(DisasContext *ctx, MemOp memop,
                                 int shift, int SrcR,
                                 int SrcL, int width, int scaled_shift,
                                 bool is_addr, int RegDst)
{
    TCGv addr_base = get_src_regx(ctx, SrcR);
    TCGv data = get_src_regx(ctx, SrcL);
    TCGv dest = temp_new(ctx);

    tcg_gen_addi_tl(dest, addr_base, shift << scaled_shift);

    gen_blk_store_32_op(ctx, memop, is_addr, dest, data, width, RegDst);
}

static void gen_blk_store_32_symbol(DisasContext *ctx, MemOp memop,
                                    int shift, int SrcL, int width,
                                    int scaled_shift, bool is_addr,
                                    int RegDst)
{
    TCGv addr_base = tcg_constant_tl(ctx->base.pc_next);
    TCGv data = get_src_regx(ctx, SrcL);
    TCGv dest = temp_new(ctx);

    tcg_gen_addi_tl(dest, addr_base, shift << scaled_shift);

    gen_blk_store_32_op(ctx, memop, is_addr, dest, data, width, RegDst);
}

static bool trans_blk_sb_32(DisasContext *ctx, arg_blk_sb_32 *a)
{
    gen_blk_store_32(ctx, MO_SB, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LB_SB,
                     OFFSET_LB_SB, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sh_32(DisasContext *ctx, arg_blk_sh_32 *a)
{
    gen_blk_store_32(ctx, MO_TESW, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LH_SH,
                     OFFSET_LH_SH, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sw_32(DisasContext *ctx, arg_blk_sw_32 *a)
{
    gen_blk_store_32(ctx, MO_TESL, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LW_SW,
                     OFFSET_LW_SW, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sd_32(DisasContext *ctx, arg_blk_sd_32 *a)
{
    gen_blk_store_32(ctx, MO_TEQ, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LD_SD,
                     OFFSET_LD_SD, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sh_u_32(DisasContext *ctx, arg_blk_sh_u_32 *a)
{
    gen_blk_store_32(ctx, MO_TESW, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LH_SH,
                     UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sw_u_32(DisasContext *ctx, arg_blk_sw_u_32 *a)
{
    gen_blk_store_32(ctx, MO_TESL, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LW_SW,
                     UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sd_u_32(DisasContext *ctx, arg_blk_sd_u_32 *a)
{
    gen_blk_store_32(ctx, MO_TEQ, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LD_SD,
                     UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sb_i_32(DisasContext *ctx, arg_blk_sb_i_32 *a)
{
    gen_blk_store_32_imm(ctx, MO_SB, a->imm,  a->SrcR, a->SrcL, BYTES_LB_SB,
                         OFFSET_LB_SB, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sh_i_32(DisasContext *ctx, arg_blk_sh_i_32 *a)
{
    gen_blk_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, BYTES_LH_SH,
                         OFFSET_LH_SH, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sw_i_32(DisasContext *ctx, arg_blk_sw_i_32 *a)
{
    gen_blk_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, BYTES_LW_SW,
                         OFFSET_LW_SW, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sd_i_32(DisasContext *ctx, arg_blk_sd_i_32 *a)
{
    gen_blk_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, BYTES_LD_SD,
                         OFFSET_LD_SD, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sh_ui_32(DisasContext *ctx, arg_blk_sh_ui_32 *a)
{
    gen_blk_store_32_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcL, BYTES_LH_SH,
                         UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sw_ui_32(DisasContext *ctx, arg_blk_sw_ui_32 *a)
{
    gen_blk_store_32_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcL, BYTES_LW_SW,
                         UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sd_ui_32(DisasContext *ctx, arg_blk_sd_ui_32 *a)
{
    gen_blk_store_32_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcL, BYTES_LD_SD,
                         UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sbl_32(DisasContext *ctx, arg_blk_sbl_32 *a)
{
    gen_blk_store_32_symbol(ctx, MO_SB, a->imm, a->SrcL, BYTES_LB_SB,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_shl_32(DisasContext *ctx, arg_blk_shl_32 *a)
{
    gen_blk_store_32_symbol(ctx, MO_TESW, a->imm, a->SrcL, BYTES_LH_SH,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_swl_32(DisasContext *ctx, arg_blk_swl_32 *a)
{
    gen_blk_store_32_symbol(ctx, MO_TESL, a->imm, a->SrcL, BYTES_LW_SW,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sdl_32(DisasContext *ctx, arg_blk_sdl_32 *a)
{
    gen_blk_store_32_symbol(ctx, MO_TEQ, a->imm, a->SrcL, BYTES_LD_SD,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool gen_blk_lr_32(DisasContext *ctx, arg_blk_lr_b_32 *a, MemOp mop)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS));

    if (linx_is_atomic_blk(get_blk_atomic(ctx->header_info))) {
        qemu_log("lr should not be in atomic block, bpc: 0x%lx, tpc: 0x%lx\n",
                 ctx->bpc, ctx->base.pc_next);
        gen_exception_illegal(ctx);
        return true;
    }

    TCGv src = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    if (a->rl) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_STRL);
    }

    /* Put addr in linx_load_res, data in linx_load_val. */
    tcg_gen_qemu_ld_tl(linx_load_val, src, ctx->mem_idx, mop);

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(src, linx_load_val, tcg_constant_tl(READ));
    }

    if (a->aq) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_LDAQ);
    }

    tcg_gen_mov_tl(linx_load_res, src);
    tcg_gen_mov_tl(dest, linx_load_val);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_lr_b_32(DisasContext *ctx, arg_blk_lr_b_32 *a)
{
    return gen_blk_lr_32(ctx, a, MO_ALIGN | MO_SB);
}

static bool trans_blk_lr_h_32(DisasContext *ctx, arg_blk_lr_h_32 *a)
{
    return gen_blk_lr_32(ctx, a, MO_ALIGN | MO_TESW);
}

static bool trans_blk_lr_w_32(DisasContext *ctx, arg_blk_lr_w_32 *a)
{
    return gen_blk_lr_32(ctx, a, MO_ALIGN | MO_TESL);
}

static bool trans_blk_lr_d_32(DisasContext *ctx, arg_blk_lr_d_32 *a)
{
    return gen_blk_lr_32(ctx, a, MO_ALIGN | MO_TEQ);
}

static bool gen_blk_sc_32(DisasContext *ctx, arg_blk_sc_b_32 *a, MemOp mop)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS));
    TCGv data, addr;
    TCGLabel *l1 = gen_new_label();
    TCGLabel *l2 = gen_new_label();
    TCGv ret = temp_new(ctx);

    if (linx_is_atomic_blk(get_blk_atomic(ctx->header_info))) {
        qemu_log("sc should not be in atomic block, bpc: 0x%lx, tpc: 0x%lx\n",
                 ctx->bpc, ctx->base.pc_next);
        gen_exception_illegal(ctx);
        return true;
    }

    addr = get_src_regx(ctx, a->SrcR);
    tcg_gen_brcond_tl(TCG_COND_NE, linx_load_res, addr, l1);

    data = get_src_regx(ctx, a->SrcL);
    tcg_gen_atomic_cmpxchg_tl(ret, linx_load_res, linx_load_val, data,
                              ctx->mem_idx, mop);
    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(linx_load_res, linx_load_val, tcg_constant_tl(READ));
        gen_helper_memprnt(linx_load_res, data, tcg_constant_tl(WRITE));
    }
    tcg_gen_setcond_tl(TCG_COND_NE, ret, ret, linx_load_val);

    TCGv dest = get_dst_regx(ctx, a->RegDst);
    tcg_gen_mov_tl(dest, ret);

    tcg_gen_br(l2);

    gen_set_label(l1);
    /*
     * Address comparison failure. However, we still need to provide the memory
     * barrier implied by AQ/RL
     */
    tcg_gen_mb(TCG_MO_ALL + a->aq * TCG_BAR_LDAQ + a->rl * TCG_BAR_STRL);
    tcg_gen_movi_tl(dest, 1);

    gen_set_label(l2);
    /*
     * Clear the load reservation, since an SC must fail if there is
     * an SC to any address, in between an LR and SC pair.
     */
    tcg_gen_movi_tl(linx_load_res, -1);

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_sc_b_32(DisasContext *ctx, arg_blk_sc_b_32 *a)
{
    return gen_blk_sc_32(ctx, a, MO_ALIGN | MO_SB);
}

static bool trans_blk_sc_h_32(DisasContext *ctx, arg_blk_sc_h_32 *a)
{
    return gen_blk_sc_32(ctx, a, MO_ALIGN | MO_TESW);
}

static bool trans_blk_sc_w_32(DisasContext *ctx, arg_blk_sc_w_32 *a)
{
    return gen_blk_sc_32(ctx, a, MO_ALIGN | MO_TESL);
}

static bool trans_blk_sc_d_32(DisasContext *ctx, arg_blk_sc_d_32 *a)
{
    return gen_blk_sc_32(ctx, a, MO_ALIGN | MO_TEQ);
}

static bool gen_amo(DisasContext *ctx, arg_arg_atomic *a,
                    void(*func)(TCGv, TCGv, TCGv, TCGArg, MemOp),
                    MemOp mop, bool set_old_to_dest)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
    TCGv dest, src1, src2;

    if (set_old_to_dest) {
        dest = get_dst_regx(ctx, a->RegDst);
    } else {
        dest = temp_new(ctx);
    }
    src1 = get_src_regx(ctx, a->SrcL);
    src2 = get_src_regx(ctx, a->SrcR);

    func(dest, src1, src2, ctx->mem_idx, mop);

    if (set_old_to_dest) {
        set_dst_regx(ctx, dest, a->RegDst);
    }
    return true;
}

static bool trans_blk_bse_32(DisasContext *ctx, arg_blk_bse_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
    return false;
}

static bool trans_blk_bwe_32(DisasContext *ctx, arg_blk_bwe_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
    return false;
}

static bool trans_blk_bwi_32(DisasContext *ctx, arg_blk_bwi_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
#ifndef CONFIG_USER_ONLY
    gen_helper_wfi(cpu_env);
#endif
    return true;
}

static bool trans_blk_bwa_32(DisasContext *ctx, arg_blk_bwa_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
    return false;
}

static bool trans_blk_assert_32(DisasContext *ctx, arg_blk_assert_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
    TCGLabel *l = gen_new_label();
    TCGv srcl = get_src_regx(ctx, a->SrcL);

    tcg_gen_brcondi_tl(TCG_COND_NE, srcl, 0, l);
    generate_exception(ctx, LINX_EXCP_ASSERT);

    gen_set_label(l);

    return true;
}

static bool trans_blk_fence_d_32(DisasContext *ctx, arg_blk_fence_d_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS));
#ifndef CONFIG_USER_ONLY
    gen_helper_tlb_flush(cpu_env);
#endif
    /* do nothing here, however, we do commit when find fenced */
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_fence_i_32(DisasContext *ctx, arg_blk_fence_i_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS));
#ifndef CONFIG_USER_ONLY
    gen_helper_tlb_flush(cpu_env);
#endif
    /* do nothing here, however, we do commit when find fencei */
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_acrc_32(DisasContext *ctx, arg_blk_acrc_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
    if (get_brhtype(ctx->header_info) != BRANCH_FALL) {
        qemu_log("acrc do not in the fall block.\n");
    }

#ifndef CONFIG_USER_ONLY
    gen_helper_acrc(cpu_env, tcg_constant_i32(a->RST_Type));
#else
    /* In user mode, we do not check request_type. */
    tcg_gen_movi_i32(scall_arg, a->RST_Type);
    tcg_gen_movi_tl(next_bpc, ctx->pc_succ_insn);
    gen_helper_raise_exception(cpu_env, tcg_constant_i32(LINX_EXCP_SCALL));
#endif

    return true;
}

static bool trans_blk_acre_32(DisasContext *ctx, arg_blk_acre_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_SYS);
#ifndef CONFIG_USER_ONLY
    TCGv retpc = temp_new(ctx);
    tcg_gen_movi_i32(scall_arg, a->RRA_Type);
    if (g_getenv("LINX_LOG_ACRE")) {
        gen_helper_trace_pc(cpu_env, tcg_constant_tl(0xaced0001));
    }
    gen_helper_acre(retpc, cpu_env, scall_arg);
    if (g_getenv("LINX_LOG_ACRE")) {
        gen_helper_trace_pc(cpu_env, tcg_constant_tl(0xaced0002));
    }
    tcg_gen_mov_tl(cpu_pc, retpc);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
#else
    return false;
#endif
}

static bool trans_blk_ebreak(DisasContext *ctx, arg_blk_ebreak *a)
{
    tcg_gen_movi_i32(scall_arg, a->imm);
    generate_exception(ctx, LINX_EXCP_BREAKPOINT);
    return true;
}

static bool trans_blk_lw_add_32(DisasContext *ctx, arg_blk_lw_add_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_add_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_lw_and_32(DisasContext *ctx, arg_blk_lw_and_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_and_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_lw_or_32(DisasContext *ctx, arg_blk_lw_or_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_or_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_lw_xor_32(DisasContext *ctx, arg_blk_lw_xor_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_xor_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_lw_smax_32(DisasContext *ctx, arg_blk_lw_smax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smax_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_lw_smin_32(DisasContext *ctx, arg_blk_lw_smin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smin_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_lw_umax_32(DisasContext *ctx, arg_blk_lw_umax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umax_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_lw_umin_32(DisasContext *ctx, arg_blk_lw_umin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umin_tl, (MO_ALIGN | MO_TESL),
                   true);
}

static bool trans_blk_ld_add_32(DisasContext *ctx, arg_blk_ld_add_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_add_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_ld_and_32(DisasContext *ctx, arg_blk_ld_and_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_and_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_ld_or_32(DisasContext *ctx, arg_blk_ld_or_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_or_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_ld_xor_32(DisasContext *ctx, arg_blk_ld_xor_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_xor_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_ld_smax_32(DisasContext *ctx, arg_blk_ld_smax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smax_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_ld_smin_32(DisasContext *ctx, arg_blk_ld_smin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smin_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_ld_umax_32(DisasContext *ctx, arg_blk_ld_umax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umax_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_ld_umin_32(DisasContext *ctx, arg_blk_ld_umin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umin_tl, (MO_ALIGN | MO_TEQ),
                   true);
}

static bool trans_blk_sw_add_32(DisasContext *ctx, arg_blk_sw_add_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_add_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sw_and_32(DisasContext *ctx, arg_blk_sw_and_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_and_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sw_or_32(DisasContext *ctx, arg_blk_sw_or_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_or_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sw_xor_32(DisasContext *ctx, arg_blk_sw_xor_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_xor_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sw_smax_32(DisasContext *ctx, arg_blk_sw_smax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smax_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sw_smin_32(DisasContext *ctx, arg_blk_sw_smin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smin_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sw_umax_32(DisasContext *ctx, arg_blk_sw_umax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umax_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sw_umin_32(DisasContext *ctx, arg_blk_sw_umin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umin_tl, (MO_ALIGN | MO_TESL),
                   false);
}

static bool trans_blk_sd_add_32(DisasContext *ctx, arg_blk_sd_add_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_add_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_sd_and_32(DisasContext *ctx, arg_blk_sd_and_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_and_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_sd_or_32(DisasContext *ctx, arg_blk_sd_or_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_or_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_sd_xor_32(DisasContext *ctx, arg_blk_sd_xor_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_xor_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_sd_smax_32(DisasContext *ctx, arg_blk_sd_smax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smax_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_sd_smin_32(DisasContext *ctx, arg_blk_sd_smin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_smin_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_sd_umax_32(DisasContext *ctx, arg_blk_sd_umax_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umax_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_sd_umin_32(DisasContext *ctx, arg_blk_sd_umin_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_fetch_umin_tl, (MO_ALIGN | MO_TEQ),
                   false);
}

static bool trans_blk_dma_32(DisasContext *ctx, arg_blk_dma_32 *a)
{
    TCGv srcl = get_src_regx(ctx, a->SrcL);
    TCGv srcr = get_src_regx(ctx, a->SrcR);

    gen_helper_dma(srcr, srcl);
    return true;
}

static bool trans_blk_swap_b_32(DisasContext *ctx, arg_blk_swap_b_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_xchg_tl, (MO_ALIGN | MO_UB), true);
}

static bool trans_blk_swap_h_32(DisasContext *ctx, arg_blk_swap_h_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_xchg_tl, (MO_ALIGN | MO_TEUW), true);
}

static bool trans_blk_swap_w_32(DisasContext *ctx, arg_blk_swap_w_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_xchg_tl, (MO_ALIGN | MO_TEUL), true);
}

static bool trans_blk_swap_d_32(DisasContext *ctx, arg_blk_swap_d_32 *a)
{
    return gen_amo(ctx, a, tcg_gen_atomic_xchg_tl, (MO_ALIGN | MO_TEQ), true);
}

static bool trans_blk_bc_iva_32(DisasContext *ctx, arg_blk_bc_iva_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_bc_iall_32(DisasContext *ctx, arg_blk_bc_iall_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_ic_iva_32(DisasContext *ctx, arg_blk_ic_iva_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_ic_iall_32(DisasContext *ctx, arg_blk_ic_iall_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_iva_32(DisasContext *ctx, arg_blk_dc_iva_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_iall_32(DisasContext *ctx, arg_blk_dc_iall_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_cva_32(DisasContext *ctx, arg_blk_dc_cva_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_civa_32(DisasContext *ctx, arg_blk_dc_civa_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_isw_32(DisasContext *ctx, arg_blk_dc_isw_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_csw_32(DisasContext *ctx, arg_blk_dc_csw_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_cisw_32(DisasContext *ctx, arg_blk_dc_cisw_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_dc_zva_32(DisasContext *ctx, arg_blk_dc_zva_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    return false;
}

static bool trans_blk_tc_ia_32(DisasContext *ctx, arg_blk_tc_ia_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
#ifndef CONFIG_USER_ONLY
    gen_helper_tlb_flush(cpu_env);
    return true;
#endif
    return false;
}

static bool trans_blk_tc_iv_32(DisasContext *ctx, arg_blk_tc_iv_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
#ifndef CONFIG_USER_ONLY
    gen_helper_tlb_flush(cpu_env);
    return true;
#endif
    return false;
}

static bool trans_blk_tc_iav_32(DisasContext *ctx, arg_blk_tc_iav_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
#ifndef CONFIG_USER_ONLY
    gen_helper_tlb_flush(cpu_env);
    return true;
#endif
    return false;
}

static bool trans_blk_tc_iall_32(DisasContext *ctx, arg_blk_tc_iall_32 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
#ifndef CONFIG_USER_ONLY
    gen_helper_tlb_flush(cpu_env);
    return true;
#endif
    return false;
}

#include "fpu/softfloat.h"

static bool trans_blk_fadd_32(DisasContext *ctx, arg_blk_fadd_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fadd_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_fadd_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_fadd_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fadd_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fsub_32(DisasContext *ctx, arg_blk_fsub_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fsub_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_fsub_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_fsub_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fsub_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fmul_32(DisasContext *ctx, arg_blk_fmul_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fmul_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_fmul_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_fmul_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fmul_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fdiv_32(DisasContext *ctx, arg_blk_fdiv_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fdiv_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_fdiv_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_fdiv_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fdiv_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fmadd_32(DisasContext *ctx, arg_blk_fmadd_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv src_t = get_src_regx(ctx, a->SrcA);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fmadd_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case HALF_PRECISION:
        gen_helper_fmadd_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SINGLE_PRECISION:
        gen_helper_fmadd_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fmadd_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fmsub_32(DisasContext *ctx, arg_blk_fmsub_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv src_t = get_src_regx(ctx, a->SrcA);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fmsub_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case HALF_PRECISION:
        gen_helper_fmsub_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SINGLE_PRECISION:
        gen_helper_fmsub_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fmsub_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fnmadd_32(DisasContext *ctx, arg_blk_fnmadd_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv src_t = get_src_regx(ctx, a->SrcA);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fnmadd_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case HALF_PRECISION:
        gen_helper_fnmadd_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SINGLE_PRECISION:
        gen_helper_fnmadd_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fnmadd_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fnmsub_32(DisasContext *ctx, arg_blk_fnmsub_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv src_t = get_src_regx(ctx, a->SrcA);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fnmsub_b(dest, cpu_env, src_l, src_r, src_t);
        break;
    case HALF_PRECISION:
        gen_helper_fnmsub_h(dest, cpu_env, src_l, src_r, src_t);
        break;
    case SINGLE_PRECISION:
        gen_helper_fnmsub_s(dest, cpu_env, src_l, src_r, src_t);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fnmsub_d(dest, cpu_env, src_l, src_r, src_t);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool gen_fp_eq(DisasContext *ctx, arg_blk_feq_32 *a, bool is_eq)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_feq_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_feq_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_feq_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_feq_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    if (!is_eq) {
        tcg_gen_not_tl(dest, dest);
        tcg_gen_andi_tl(dest, dest, 1);
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_feq_32(DisasContext *ctx, arg_blk_feq_32 *a)
{
    return gen_fp_eq(ctx, a, true);
}

static bool trans_blk_fne_32(DisasContext *ctx, arg_blk_fne_32 *a)
{
    return gen_fp_eq(ctx, a, false);
}

static bool gen_fp_lt_32(DisasContext *ctx, arg_blk_flt_32 *a, bool is_lt)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_flt_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_flt_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_flt_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_flt_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    if (!is_lt) {
        tcg_gen_not_tl(dest, dest);
        tcg_gen_andi_tl(dest, dest, 1);
    }
    /* DstType doesn't seem to work */
    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_flt_32(DisasContext *ctx, arg_blk_flt_32 *a)
{
    return gen_fp_lt_32(ctx, a, true);
}

static bool trans_blk_fge_32(DisasContext *ctx, arg_blk_fge_32 *a)
{
    return gen_fp_lt_32(ctx, a, false);
}

static bool trans_blk_feqs_32(DisasContext *ctx, arg_blk_feqs_32 *a)
{
    /* TODO: need to handle the signal */
    return gen_fp_eq(ctx, a, true);
}

static bool trans_blk_fnes_32(DisasContext *ctx, arg_blk_fnes_32 *a)
{
    /* TODO: need to handle the signal */
    return gen_fp_eq(ctx, a, false);
}

static bool trans_blk_flts_32(DisasContext *ctx, arg_blk_flts_32 *a)
{
    /* TODO: need to handle the signal */
    return gen_fp_lt_32(ctx, a, true);
}

static bool trans_blk_fges_32(DisasContext *ctx, arg_blk_fges_32 *a)
{
    /* TODO: need to handle the signal */
    return gen_fp_lt_32(ctx, a, false);
}

static inline bool gen_maxmin(DisasContext *ctx, arg_blk_max_32 *a,
    DisasExtend ext, void (*func)(TCGv, TCGv, TCGv))
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    func(dest, src_l, src_r);
    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_max_32(DisasContext *ctx, arg_blk_max_32 *a)
{
    return gen_maxmin(ctx, a, EXT_SIGN, tcg_gen_smax_tl);
}

static bool trans_blk_maxu_32(DisasContext *ctx, arg_blk_maxu_32 *a)
{
    return gen_maxmin(ctx, a, EXT_ZERO, tcg_gen_umax_tl);
}

static bool trans_blk_min_32(DisasContext *ctx, arg_blk_min_32 *a)
{
    return gen_maxmin(ctx, a, EXT_SIGN, tcg_gen_smin_tl);
}

static bool trans_blk_minu_32(DisasContext *ctx, arg_blk_minu_32 *a)
{
    return gen_maxmin(ctx, a, EXT_ZERO, tcg_gen_umin_tl);
}

static bool trans_blk_fmax_32(DisasContext *ctx, arg_blk_fmax_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fmax_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_fmax_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_fmax_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fmax_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fmin_32(DisasContext *ctx, arg_blk_fmin_32 *a)
{
    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fmin_b(dest, cpu_env, src_l, src_r);
        break;
    case HALF_PRECISION:
        gen_helper_fmin_h(dest, cpu_env, src_l, src_r);
        break;
    case SINGLE_PRECISION:
        gen_helper_fmin_s(dest, cpu_env, src_l, src_r);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fmin_d(dest, cpu_env, src_l, src_r);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static TCGv handel_float_to_int(TCGv SrcL, TCGv RegDst,
                                int SrcType, int DstType)
{
    switch (DstType) {
    case CVT_FTOI_U64:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_ud_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_ud_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_ud_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_ud_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FTOI_U32:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_uw_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_uw_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_uw_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_uw_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FTOI_U16:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_uh_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_uh_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_uh_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_uh_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FTOI_U8:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_ub_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_ub_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_ub_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_ub_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FTOI_S64:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_sd_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_sd_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_sd_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_sd_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FTOI_S32:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_sw_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_sw_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_sw_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_sw_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FTOI_S16:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_sh_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_sh_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_sh_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_sh_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FTOI_S8:
        switch (SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_sb_d(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP32:
            gen_helper_fcvt_sb_s(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP16:
            gen_helper_fcvt_sb_h(RegDst, cpu_env, SrcL);
            break;
        case CVT_FP8:
            gen_helper_fcvt_sb_b(RegDst, cpu_env, SrcL);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    default:
        linx_debug_not_reached();
        break;
    }

    return RegDst;
}

static bool trans_blk_fcvt_32(DisasContext *ctx, arg_blk_fcvt_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTZ(round_to_zero)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));

    switch (a->DstType) {
    case CVT_FP64:
        switch (a->SrcType) {
        case CVT_FP32:
            gen_helper_fcvt_d_s(dest, cpu_env, src_l);
            break;
        case CVT_FP16:
            gen_helper_fcvt_d_h(dest, cpu_env, src_l);
            break;
        case CVT_FP8:
            gen_helper_fcvt_d_b(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP32:
        switch (a->SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_s_d(dest, cpu_env, src_l);
            break;
        case CVT_FP16:
            gen_helper_fcvt_s_h(dest, cpu_env, src_l);
            break;
        case CVT_FP8:
            gen_helper_fcvt_s_b(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP16:
        switch (a->SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_h_d(dest, cpu_env, src_l);
            break;
        case CVT_FP32:
            gen_helper_fcvt_h_s(dest, cpu_env, src_l);
            break;
        case CVT_FP8:
            gen_helper_fcvt_h_b(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP8:
        switch (a->SrcType) {
        case CVT_FP64:
            gen_helper_fcvt_b_d(dest, cpu_env, src_l);
            break;
        case CVT_FP32:
            gen_helper_fcvt_b_s(dest, cpu_env, src_l);
            break;
        case CVT_FP16:
            gen_helper_fcvt_b_h(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    default:
        linx_debug_not_reached();
        break;
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_blk_fcvta_32(DisasContext *ctx, arg_blk_fcvta_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTA(float_round_ties_away)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_ties_away));

    if (a->SrcType >= CVT_FP64 && a->SrcType <= CVT_FP8) {
        dest = handel_float_to_int(src_l, dest, a->SrcType, a->DstType);
    } else {
        linx_debug_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_blk_fcvtm_32(DisasContext *ctx, arg_blk_fcvtm_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTN(float_round_down)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_down));

    if (a->SrcType >= CVT_FP64 && a->SrcType <= CVT_FP8) {
        dest = handel_float_to_int(src_l, dest, a->SrcType, a->DstType);
    } else {
        linx_debug_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_blk_fcvtn_32(DisasContext *ctx, arg_blk_fcvtn_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTE(float_round_nearest_even)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));

    if (a->SrcType >= CVT_FP64 && a->SrcType <= CVT_FP8) {
        dest = handel_float_to_int(src_l, dest, a->SrcType, a->DstType);
    } else {
        linx_debug_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_blk_fcvtp_32(DisasContext *ctx, arg_blk_fcvtp_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTP(float_round_up)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_up));

    if (a->SrcType >= CVT_FP64 && a->SrcType <= CVT_FP8) {
        dest = handel_float_to_int(src_l, dest, a->SrcType, a->DstType);
    } else {
        linx_debug_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_blk_fcvtz_32(DisasContext *ctx, arg_blk_fcvtz_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTZ(float_round_to_zero)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_to_zero));

    if (a->SrcType >= CVT_FP64 && a->SrcType <= CVT_FP8) {
        dest = handel_float_to_int(src_l, dest, a->SrcType, a->DstType);
    } else {
            linx_debug_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_blk_scvtf_32(DisasContext *ctx, arg_blk_scvtf_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTZ(round_to_zero)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));

    switch (a->DstType) {
    case CVT_FP64:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_d_sd(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_d_sw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_d_sh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_d_sb(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP32:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_s_sd(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_s_sw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_s_sh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_s_sb(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP16:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_h_sd(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_h_sw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_h_sh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_h_sb(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP8:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_b_sd(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_b_sw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_b_sh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_b_sb(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    default:
        linx_debug_not_reached();
        break;
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}

static bool trans_blk_ucvtf_32(DisasContext *ctx, arg_blk_ucvtf_32 *a)
{
    /*
     * the round mode of fcvt is aligned with the RTZ(round_to_zero)
     * in the soft floating point library
     */

    static_blk_type_check(ctx, 1 << HEAD_TYPE_FP);
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                 tcg_constant_i32(float_round_nearest_even));

    switch (a->DstType) {
    case CVT_FP64:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_d_ud(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_d_uw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_d_uh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_d_ub(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP32:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_s_ud(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_s_uw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_s_uh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_s_ub(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP16:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_h_ud(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_h_uw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_h_uh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_h_ub(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    case CVT_FP8:
        switch (a->SrcType) {
        case CVT_ITOF_64:
            gen_helper_fcvt_b_ud(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_32:
            gen_helper_fcvt_b_uw(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_16:
            gen_helper_fcvt_b_uh(dest, cpu_env, src_l);
            break;
        case CVT_ITOF_8:
            gen_helper_fcvt_b_ub(dest, cpu_env, src_l);
            break;
        default:
            linx_debug_not_reached();
            break;
        }
        break;
    default:
        linx_debug_not_reached();
        break;
    }

    set_dst_regx(ctx, dest, a->RegDst);
    gen_helper_set_rounding_mode(cpu_env,
                                tcg_constant_i32(float_round_nearest_even));
    return true;
}
static bool trans_blk_fabs_32(DisasContext *ctx, arg_blk_fabs_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(7, 1));
        break;
    case HALF_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(15, 1));
        break;
    case SINGLE_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(31, 1));
        break;
    case DOUBLE_PRECISION:
        tcg_gen_andi_i64(dest, src_l, ~MAKE_64BIT_MASK(63, 1));
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);

    return true;
}

static bool trans_blk_fsqrt_32(DisasContext *ctx, arg_blk_fsqrt_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fsqrt_b(dest, cpu_env, src_l);
        break;
    case HALF_PRECISION:
        gen_helper_fsqrt_h(dest, cpu_env, src_l);
        break;
    case SINGLE_PRECISION:
        gen_helper_fsqrt_s(dest, cpu_env, src_l);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fsqrt_d(dest, cpu_env, src_l);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_frecip_32(DisasContext *ctx, arg_blk_frecip_32 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    TCGv fp8_e4m3_1 = tcg_constant_i64(0x38);

    switch (a->SrcType) {
    case LOW_PRECISION:
        gen_helper_fdiv_b(dest, cpu_env, fp8_e4m3_1, src_l);
        break;
    case HALF_PRECISION:
        gen_helper_fdiv_h(dest, cpu_env, tcg_constant_tl(float16_one), src_l);
        break;
    case SINGLE_PRECISION:
        gen_helper_fdiv_s(dest, cpu_env, tcg_constant_tl(float32_one), src_l);
        break;
    case DOUBLE_PRECISION:
        gen_helper_fdiv_d(dest, cpu_env, tcg_constant_tl(float64_one), src_l);
        break;
    default:
        g_assert_not_reached();
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_fexp_32(DisasContext *ctx, arg_blk_fexp_32 *a)
{
    return false;
}
