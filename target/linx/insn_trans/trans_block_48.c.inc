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

static bool trans_blk_lui_48(DisasContext *ctx, arg_blk_lui_48 *a)
{
    TCGv imm = tcg_constant_i64((int64_t)(int32_t)a->imm);
    TCGv addr = get_dst_regx(ctx, a->RegDst);
    tcg_gen_mov_tl(addr, imm);

    set_dst_regx(ctx, addr, a->RegDst);
    return true;
}

typedef enum {
    BLK_ARITHI_ADD,
    BLK_ARITHI_SUB,
    BLK_ARITHI_AND,
    BLK_ARITHI_OR,
    BLK_ARITHI_XOR,
} BlkArithI;

static bool gen_blk_arith_i_48(DisasContext *ctx, int imm, int SrcL,
                               int RegDst, BlkArithI op, bool word)
{
    TCGv src_l = get_src_regx(ctx, SrcL);
    TCGv dest = get_dst_regx(ctx, RegDst);

    switch (op) {
    case BLK_ARITHI_ADD:
        tcg_gen_addi_tl(dest, src_l, imm);
        break;
    case BLK_ARITHI_SUB:
        tcg_gen_subi_tl(dest, src_l, imm);
        break;
    case BLK_ARITHI_AND:
        tcg_gen_andi_tl(dest, src_l, imm);
        break;
    case BLK_ARITHI_OR:
        tcg_gen_ori_tl(dest, src_l, imm);
        break;
    case BLK_ARITHI_XOR:
        tcg_gen_xori_tl(dest, src_l, imm);
        break;
    }

    if (word) {
        tcg_gen_ext32s_tl(dest, dest);
    }

    set_dst_regx(ctx, dest, RegDst);
    return true;
}

static bool trans_blk_addi_48(DisasContext *ctx, arg_blk_addi_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_ADD, false);
}

static bool trans_blk_subi_48(DisasContext *ctx, arg_blk_subi_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_SUB, false);
}

static bool trans_blk_andi_48(DisasContext *ctx, arg_blk_andi_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_AND, false);
}

static bool trans_blk_ori_48(DisasContext *ctx, arg_blk_ori_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_OR, false);
}

static bool trans_blk_xori_48(DisasContext *ctx, arg_blk_xori_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_XOR, false);
}

static bool trans_blk_addiw_48(DisasContext *ctx, arg_blk_addiw_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_ADD, true);
}

static bool trans_blk_subiw_48(DisasContext *ctx, arg_blk_subiw_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_SUB, true);
}

static bool trans_blk_andiw_48(DisasContext *ctx, arg_blk_andiw_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_AND, true);
}

static bool trans_blk_oriw_48(DisasContext *ctx, arg_blk_oriw_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_OR, true);
}

static bool trans_blk_xoriw_48(DisasContext *ctx, arg_blk_xoriw_48 *a)
{
    return gen_blk_arith_i_48(ctx, a->imm, a->SrcL, a->RegDst,
                              BLK_ARITHI_XOR, true);
}

static bool trans_blk_mul_48(DisasContext *ctx, arg_blk_mul_48 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest0 = get_dst_regx(ctx, a->RegDst0);
    TCGv dest1 = get_dst_regx(ctx, a->RegDst1);

    tcg_gen_muls2_tl(dest0, dest1, src1, src2);

    set_dst_regx(ctx, dest0, a->RegDst0);
    set_dst_regx(ctx, dest1, a->RegDst1);

    return true;
}

static bool trans_blk_mulu_48(DisasContext *ctx, arg_blk_mulu_48 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest0 = get_dst_regx(ctx, a->RegDst0);
    TCGv dest1 = get_dst_regx(ctx, a->RegDst1);

    tcg_gen_mulu2_tl(dest0, dest1, src1, src2);

    set_dst_regx(ctx, dest0, a->RegDst0);
    set_dst_regx(ctx, dest1, a->RegDst1);

    return true;
}

static bool trans_blk_madd_48(DisasContext *ctx, arg_blk_madd_48 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv src3 = get_src_regx(ctx, a->SrcD);
    TCGv dest0 = get_dst_regx(ctx, a->RegDst0);
    TCGv dest1 = get_dst_regx(ctx, a->RegDst1);

    TCGv hi = temp_new(ctx);
    TCGv lo = temp_new(ctx);
    TCGv zero = tcg_const_tl(0);
    tcg_gen_muls2_tl(lo, hi, src1, src2);
    tcg_gen_add2_tl(dest0, dest1, lo, hi, src3, zero);

    set_dst_regx(ctx, dest0, a->RegDst0);
    set_dst_regx(ctx, dest1, a->RegDst1);
    return true;
}

static bool trans_blk_maddw_48(DisasContext *ctx, arg_blk_madd_48 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv src3 = get_src_regx(ctx, a->SrcD);
    TCGv dest0 = get_dst_regx(ctx, a->RegDst0);
    TCGv dest1 = get_dst_regx(ctx, a->RegDst1);

    TCGv res = temp_new(ctx);
    tcg_gen_mul_tl(res, src1, src2);
    tcg_gen_add_tl(res, src3, res);
    tcg_gen_ext32s_tl(dest0, res);

    tcg_gen_sari_tl(dest1, res, 32);
    tcg_gen_ext32s_tl(dest1, dest1);

    set_dst_regx(ctx, dest0, a->RegDst0);
    set_dst_regx(ctx, dest1, a->RegDst1);
    return true;
}

static void gen_blk_load_48_op(DisasContext *ctx, MemOp memop, bool is_addr,
                                TCGv addr, int dest0)
{
    TCGv dest_Treg = get_dst_regx(ctx, dest0);
    if (tb_cflags(ctx->base.tb) & CF_ATOMIC) {
        TCGv_i32 memop_t = tcg_constant_i32(memop);
        gen_helper_load_data(dest_Treg, cpu_env, addr, memop_t);
    } else {
        tcg_gen_qemu_ld_tl(dest_Treg, addr, ctx->mem_idx, memop);
    }
    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(addr, dest_Treg, tcg_constant_tl(READ));
    }

    if (is_addr) {
        set_dst_regx(ctx, addr, dest0);
    } else {
        set_dst_regx(ctx, dest_Treg, dest0);
    }
}

static void gen_blk_load_pr_48(DisasContext *ctx, MemOp memop, bool is_addr,
                            int shamt, int SrcRType, int SrcR_enc,
                            int SrcL_enc, int dest0, int dest1)
{
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);
    TCGv SrcRreg = get_src_regx(ctx, SrcR_enc);
    TCGv shift = conver_src_value(ctx, SrcRreg, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, shamt);

    /* shift becomes the "addr" */
    tcg_gen_add_tl(shift, addr_base, shift);

    set_dst_regx(ctx, shift, dest1);

    /* Use the (address + shift) to load data from memory */
    gen_blk_load_48_op(ctx, memop, is_addr, shift, dest0);
}

static bool trans_blk_lb_pr_48(DisasContext *ctx, arg_blk_lb_pr_48 *a)
{
    gen_blk_load_pr_48(ctx, MO_SB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lh_pr_48(DisasContext *ctx, arg_blk_lh_pr_48 *a)
{
    gen_blk_load_pr_48(ctx, MO_TESW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lw_pr_48(DisasContext *ctx, arg_blk_lw_pr_48 *a)
{
    gen_blk_load_pr_48(ctx, MO_TESL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_ld_pr_48(DisasContext *ctx, arg_blk_ld_pr_48 *a)
{
    gen_blk_load_pr_48(ctx, MO_TEQ, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lbu_pr_48(DisasContext *ctx, arg_blk_lbu_pr_48 *a)
{
    gen_blk_load_pr_48(ctx, MO_UB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lhu_pr_48(DisasContext *ctx, arg_blk_lhu_pr_48 *a)
{
    gen_blk_load_pr_48(ctx, MO_TEUW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lwu_pr_48(DisasContext *ctx, arg_blk_lwu_pr_48 *a)
{
    gen_blk_load_pr_48(ctx, MO_TEUL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static void gen_blk_load_pr_48_imm(DisasContext *ctx, MemOp memop, bool is_addr,
                                int imm, int SrcL_enc, int dest0, int dest1,
                                int scaled_shift)
{
    /* get SrcLreg  addr_base */
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    set_dst_regx(ctx, addr, dest1);

    gen_blk_load_48_op(ctx, memop, is_addr, addr, dest0);
}

static bool trans_blk_lbi_pr_48(DisasContext *ctx, arg_blk_lbi_pr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhi_pr_48(DisasContext *ctx, arg_blk_lhi_pr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwi_pr_48(DisasContext *ctx, arg_blk_lwi_pr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_ldi_pr_48(DisasContext *ctx, arg_blk_ldi_pr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LD_SD);
    return true;
}

static bool trans_blk_lbui_pr_48(DisasContext *ctx, arg_blk_lbui_pr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhui_pr_48(DisasContext *ctx, arg_blk_lhui_pr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwui_pr_48(DisasContext *ctx, arg_blk_lwui_pr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_lhi_upr_48(DisasContext *ctx, arg_blk_lhi_upr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_lwi_upr_48(DisasContext *ctx, arg_blk_lwi_upr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                         a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_ldi_upr_48(DisasContext *ctx, arg_blk_ldi_upr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_lhui_upr_48(DisasContext *ctx, arg_blk_lhui_upr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_lwui_upr_48(DisasContext *ctx, arg_blk_lwui_upr_48 *a)
{
    gen_blk_load_pr_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static void gen_blk_load_po_48(DisasContext *ctx, MemOp memop, bool is_addr,
                            int shamt, int SrcRType, int SrcR_enc,
                            int SrcL_enc, int dest0, int dest1)
{
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);
    TCGv SrcRreg = get_src_regx(ctx, SrcR_enc);
    TCGv shift = conver_src_value(ctx, SrcRreg, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, shamt);

    /* Current v0.56 bits[5:4]=01 encodes HL.*P load-pair, not old *.PO. */
    tcg_gen_add_tl(shift, addr_base, shift);
    TCGv addr1 = temp_new(ctx);
    tcg_gen_addi_tl(addr1, shift, memop_size(memop));

    gen_blk_load_48_op(ctx, memop, is_addr, shift, dest0);
    gen_blk_load_48_op(ctx, memop, is_addr, addr1, dest1);
}

static bool trans_blk_lb_po_48(DisasContext *ctx, arg_blk_lb_po_48 *a)
{
    gen_blk_load_po_48(ctx, MO_SB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lh_po_48(DisasContext *ctx, arg_blk_lh_po_48 *a)
{
    gen_blk_load_po_48(ctx, MO_TESW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lw_po_48(DisasContext *ctx, arg_blk_lw_po_48 *a)
{
    gen_blk_load_po_48(ctx, MO_TESL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_ld_po_48(DisasContext *ctx, arg_blk_ld_po_48 *a)
{
    gen_blk_load_po_48(ctx, MO_TEQ, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}


static bool trans_blk_lbu_po_48(DisasContext *ctx, arg_blk_lbu_po_48 *a)
{
    gen_blk_load_po_48(ctx, MO_UB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lhu_po_48(DisasContext *ctx, arg_blk_lhu_po_48 *a)
{
    gen_blk_load_po_48(ctx, MO_TEUW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static bool trans_blk_lwu_po_48(DisasContext *ctx, arg_blk_lwu_po_48 *a)
{
    gen_blk_load_po_48(ctx, MO_TEUL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1);
    return true;
}

static void gen_blk_load_po_48_imm(DisasContext *ctx, MemOp memop, bool is_addr,
                                int imm, int SrcL_enc, int dest0, int dest1,
                                int scaled_shift)
{
    /* get SrcLreg  addr_base */
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);
    TCGv addr1 = temp_new(ctx);
    tcg_gen_addi_tl(addr1, addr, memop_size(memop));

    /* Current v0.56 bits[5:4]=01 encodes HL.*IP load-pair, not old *.I.PO. */
    gen_blk_load_48_op(ctx, memop, is_addr, addr, dest0);
    gen_blk_load_48_op(ctx, memop, is_addr, addr1, dest1);
}

static bool trans_blk_lbi_po_48(DisasContext *ctx, arg_blk_lbi_po_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhi_po_48(DisasContext *ctx, arg_blk_lhi_po_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwi_po_48(DisasContext *ctx, arg_blk_lwi_po_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_ldi_po_48(DisasContext *ctx, arg_blk_ldi_po_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LD_SD);
    return true;
}

static bool trans_blk_lbui_po_48(DisasContext *ctx, arg_blk_lbui_po_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhui_po_48(DisasContext *ctx, arg_blk_lhui_po_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwui_po_48(DisasContext *ctx, arg_blk_lwui_po_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_lhi_upo_48(DisasContext *ctx, arg_blk_lhi_upo_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_lwi_upo_48(DisasContext *ctx, arg_blk_lwi_upo_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                         a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_ldi_upo_48(DisasContext *ctx, arg_blk_ldi_upo_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_lhui_upo_48(DisasContext *ctx, arg_blk_lhui_upo_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static bool trans_blk_lwui_upo_48(DisasContext *ctx, arg_blk_lwui_upo_48 *a)
{
    gen_blk_load_po_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, UNSCALED);
    return true;
}

static void gen_blk_load_p_pr_48(DisasContext *ctx, MemOp memop, bool is_addr,
                            int shamt, int SrcRType, int SrcR_enc,
                            int SrcL_enc, int dest0, int dest1, int dest2)
{
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);
    TCGv SrcRreg = get_src_regx(ctx, SrcR_enc);
    TCGv shift = conver_src_value(ctx, SrcRreg, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, shamt);

    /* shift becomes the "addr" */
    tcg_gen_add_tl(shift, addr_base, shift);

    set_dst_regx(ctx, shift, dest2);

    gen_blk_load_48_op(ctx, memop, is_addr, addr_base, dest0);
    gen_blk_load_48_op(ctx, memop, is_addr, shift, dest1);
}

static bool trans_blk_lbp_pr_48(DisasContext *ctx, arg_blk_lbp_pr_48 *a)
{
    gen_blk_load_p_pr_48(ctx, MO_SB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lhp_pr_48(DisasContext *ctx, arg_blk_lhp_pr_48 *a)
{
    gen_blk_load_p_pr_48(ctx, MO_TESW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lwp_pr_48(DisasContext *ctx, arg_blk_lwp_pr_48 *a)
{
    gen_blk_load_p_pr_48(ctx, MO_TESL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_ldp_pr_48(DisasContext *ctx, arg_blk_ldp_pr_48 *a)
{
    gen_blk_load_p_pr_48(ctx, MO_TEQ, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lbup_pr_48(DisasContext *ctx, arg_blk_lbup_pr_48 *a)
{
    gen_blk_load_p_pr_48(ctx, MO_UB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lhup_pr_48(DisasContext *ctx, arg_blk_lhup_pr_48 *a)
{
    gen_blk_load_p_pr_48(ctx, MO_TEUW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lwup_pr_48(DisasContext *ctx, arg_blk_lwup_pr_48 *a)
{
    gen_blk_load_p_pr_48(ctx, MO_TEUL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static void gen_blk_load_p_pr_48_imm(DisasContext *ctx, MemOp memop,
                                bool is_addr, int imm, int SrcL_enc, int dest0,
                                int dest1, int dest2, int scaled_shift)
{
    /* get SrcLreg  addr_base */
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    set_dst_regx(ctx, addr_base, dest2);

    gen_blk_load_48_op(ctx, memop, is_addr, addr_base, dest0);
    gen_blk_load_48_op(ctx, memop, is_addr, addr, dest1);
}

static bool trans_blk_lbip_pr_48(DisasContext *ctx, arg_blk_lbip_pr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhip_pr_48(DisasContext *ctx, arg_blk_lhip_pr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwip_pr_48(DisasContext *ctx, arg_blk_lwip_pr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_ldip_pr_48(DisasContext *ctx, arg_blk_ldip_pr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LD_SD);
    return true;
}

static bool trans_blk_lbuip_pr_48(DisasContext *ctx, arg_blk_lbuip_pr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhuip_pr_48(DisasContext *ctx, arg_blk_lhuip_pr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwuip_pr_48(DisasContext *ctx, arg_blk_lwuip_pr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_lhip_upr_48(DisasContext *ctx, arg_blk_lhip_upr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_lwip_upr_48(DisasContext *ctx, arg_blk_lwip_upr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                         a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_ldip_upr_48(DisasContext *ctx, arg_blk_ldip_upr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_lhuip_upr_48(DisasContext *ctx, arg_blk_lhuip_upr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_lwuip_upr_48(DisasContext *ctx, arg_blk_lwuip_upr_48 *a)
{
    gen_blk_load_p_pr_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static void gen_blk_load_p_po_48(DisasContext *ctx, MemOp memop, bool is_addr,
                            int shamt, int SrcRType, int SrcR_enc,
                            int SrcL_enc, int dest0, int dest1, int dest2)
{
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);
    TCGv SrcRreg = get_src_regx(ctx, SrcR_enc);
    TCGv shift = conver_src_value(ctx, SrcRreg, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, shamt);

    /* shift becomes the "addr" */
    tcg_gen_add_tl(shift, addr_base, shift);

    gen_blk_load_48_op(ctx, memop, is_addr, addr_base, dest0);
    set_dst_regx(ctx, shift, dest1);
}

static bool trans_blk_lbp_po_48(DisasContext *ctx, arg_blk_lbp_po_48 *a)
{
    gen_blk_load_p_po_48(ctx, MO_SB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lhp_po_48(DisasContext *ctx, arg_blk_lhp_po_48 *a)
{
    gen_blk_load_p_po_48(ctx, MO_TESW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lwp_po_48(DisasContext *ctx, arg_blk_lwp_po_48 *a)
{
    gen_blk_load_p_po_48(ctx, MO_TESL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_ldp_po_48(DisasContext *ctx, arg_blk_ldp_po_48 *a)
{
    gen_blk_load_p_po_48(ctx, MO_TEQ, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lbup_po_48(DisasContext *ctx, arg_blk_lbup_po_48 *a)
{
    gen_blk_load_p_po_48(ctx, MO_UB, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lhup_po_48(DisasContext *ctx, arg_blk_lhup_po_48 *a)
{
    gen_blk_load_p_po_48(ctx, MO_TEUW, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static bool trans_blk_lwup_po_48(DisasContext *ctx, arg_blk_lwup_po_48 *a)
{
    gen_blk_load_p_po_48(ctx, MO_TEUL, NO_ADDR, a->shamt, a->SrcRType,
                        a->SrcR, a->SrcL, a->RegDst0, a->RegDst1, a->RegDst2);
    return true;
}

static void gen_blk_load_p_po_48_imm(DisasContext *ctx, MemOp memop,
                                bool is_addr, int imm, int SrcL_enc, int dest0,
                                int dest1, int dest2, int scaled_shift)
{
    /* get SrcLreg  addr_base */
    TCGv addr_base = get_src_regx(ctx, SrcL_enc);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    gen_blk_load_48_op(ctx, memop, is_addr, addr_base, dest0);
    set_dst_regx(ctx, addr, dest1);
}

static bool trans_blk_lbip_po_48(DisasContext *ctx, arg_blk_lbip_po_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_SB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhip_po_48(DisasContext *ctx, arg_blk_lhip_po_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwip_po_48(DisasContext *ctx, arg_blk_lwip_po_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_ldip_po_48(DisasContext *ctx, arg_blk_ldip_po_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LD_SD);
    return true;
}

static bool trans_blk_lbuip_po_48(DisasContext *ctx, arg_blk_lbuip_po_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_UB, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LB_SB);
    return true;
}

static bool trans_blk_lhuip_po_48(DisasContext *ctx, arg_blk_lhuip_po_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LH_SH);
    return true;
}

static bool trans_blk_lwuip_po_48(DisasContext *ctx, arg_blk_lwuip_po_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_lhip_upo_48(DisasContext *ctx, arg_blk_lhip_upo_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TESW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_lwip_upo_48(DisasContext *ctx, arg_blk_lwip_upo_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TESL, NO_ADDR, a->imm, a->SrcL,
                         a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_ldip_upo_48(DisasContext *ctx, arg_blk_ldip_upo_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_lhuip_upo_48(DisasContext *ctx, arg_blk_lhuip_upo_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TEUW, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}

static bool trans_blk_lwuip_upo_48(DisasContext *ctx, arg_blk_lwuip_upo_48 *a)
{
    gen_blk_load_p_po_48_imm(ctx, MO_TEUL, NO_ADDR, a->imm, a->SrcL,
                        a->RegDst0, a->RegDst1, a->RegDst2, UNSCALED);
    return true;
}


static void gen_blk_store_48_op(DisasContext *ctx, MemOp memop, bool is_addr,
                                TCGv dest, TCGv data, int width, int RegDst)
{
    if (tb_cflags(ctx->base.tb) & CF_ATOMIC) {
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

static void gen_blk_store_pr_48(DisasContext *ctx, MemOp memop,
                             int SrcD, int SrcRType,
                             int SrcR, int SrcL,
                             int width, int scaled_shift,
                             bool is_addr, int RegDst, int dest)
{
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv addr_base = get_src_regx(ctx, SrcL);
    TCGv shift_base = get_src_regx(ctx, SrcR);
    TCGv shift = conver_src_value(ctx, shift_base, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, scaled_shift);

    /* shift become dest */
    tcg_gen_add_tl(shift, addr_base, shift);

    set_dst_regx(ctx, shift, dest);

    gen_blk_store_48_op(ctx, memop, is_addr, shift, data, width, RegDst);
}

static bool trans_blk_sb_pr_48(DisasContext *ctx, arg_blk_sb_pr_48 *a)
{
    gen_blk_store_pr_48(ctx, MO_SB, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LB_SB,
                     OFFSET_LB_SB, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sh_pr_48(DisasContext *ctx, arg_blk_sh_pr_48 *a)
{
    gen_blk_store_pr_48(ctx, MO_TESW, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LH_SH,
                     OFFSET_LH_SH, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sw_pr_48(DisasContext *ctx, arg_blk_sw_pr_48 *a)
{
    gen_blk_store_pr_48(ctx, MO_TESL, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LW_SW,
                     OFFSET_LW_SW, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sd_pr_48(DisasContext *ctx, arg_blk_sd_pr_48 *a)
{
    gen_blk_store_pr_48(ctx, MO_TEQ, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LD_SD,
                        OFFSET_LD_SD, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sh_upr_48(DisasContext *ctx, arg_blk_sh_upr_48 *a)
{
    gen_blk_store_pr_48(ctx, MO_TESW, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LH_SH,
                        UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sw_upr_48(DisasContext *ctx, arg_blk_sw_upr_48 *a)
{
    gen_blk_store_pr_48(ctx, MO_TESL, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LW_SW,
                        UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sd_upr_48(DisasContext *ctx, arg_blk_sd_upr_48 *a)
{
    gen_blk_store_pr_48(ctx, MO_TEQ, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LD_SD,
                        UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}
static void gen_blk_store_pr_48_imm(DisasContext *ctx, MemOp memop,
                                    int imm, int SrcR,
                                    int SrcD, int width, int scaled_shift,
                                    bool is_addr, int RegDst, int reg)
{
    TCGv addr_base = get_src_regx(ctx, SrcR);
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv addr = temp_new(ctx);

    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    set_dst_regx(ctx, addr, reg);

    gen_blk_store_48_op(ctx, memop, is_addr, addr, data, width, RegDst);
}

static bool trans_blk_sbi_pr_48(DisasContext *ctx, arg_blk_sbi_pr_48 *a)
{
    gen_blk_store_pr_48_imm(ctx, MO_SB, a->imm,  a->SrcR, a->SrcD, BYTES_LB_SB,
                            OFFSET_LB_SB, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shi_pr_48(DisasContext *ctx, arg_blk_shi_pr_48 *a)
{
    gen_blk_store_pr_48_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcD, BYTES_LH_SH,
                            OFFSET_LH_SH, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swi_pr_48(DisasContext *ctx, arg_blk_swi_pr_48 *a)
{
    gen_blk_store_pr_48_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcD, BYTES_LW_SW,
                            OFFSET_LW_SW, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdi_pr_48(DisasContext *ctx, arg_blk_sdi_pr_48 *a)
{
    gen_blk_store_pr_48_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcD, BYTES_LD_SD,
                            OFFSET_LD_SD, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shi_upr_48(DisasContext *ctx, arg_blk_shi_upr_48 *a)
{
    gen_blk_store_pr_48_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcD, BYTES_LH_SH,
                            UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swi_upr_48(DisasContext *ctx, arg_blk_swi_upr_48 *a)
{
    gen_blk_store_pr_48_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcD, BYTES_LW_SW,
                            UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdi_upr_48(DisasContext *ctx, arg_blk_sdi_upr_48 *a)
{
    gen_blk_store_pr_48_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcD, BYTES_LD_SD,
                            UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}


static void gen_blk_store_po_48(DisasContext *ctx, MemOp memop,
                             int SrcD, int SrcRType,
                             int SrcR, int SrcL,
                             int width, int scaled_shift,
                             bool is_addr, int RegDst, int dest)
{
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv addr_base = get_src_regx(ctx, SrcL);
    TCGv shift_base = get_src_regx(ctx, SrcR);
    TCGv shift = conver_src_value(ctx, shift_base, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, scaled_shift);

    /* shift become dest */
    tcg_gen_add_tl(shift, addr_base, shift);

    set_dst_regx(ctx, shift, dest);

    gen_blk_store_48_op(ctx, memop, is_addr, addr_base, data, width, RegDst);
}

static bool trans_blk_sb_po_48(DisasContext *ctx, arg_blk_sb_po_48 *a)
{
    gen_blk_store_po_48(ctx, MO_SB, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LB_SB,
                     OFFSET_LB_SB, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sh_po_48(DisasContext *ctx, arg_blk_sh_po_48 *a)
{
    gen_blk_store_po_48(ctx, MO_TESW, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LH_SH,
                     OFFSET_LH_SH, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sw_po_48(DisasContext *ctx, arg_blk_sw_po_48 *a)
{
    gen_blk_store_po_48(ctx, MO_TESL, a->SrcD, a->SrcRType,
                     a->SrcR, a->SrcL, BYTES_LW_SW,
                     OFFSET_LW_SW, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sd_po_48(DisasContext *ctx, arg_blk_sd_po_48 *a)
{
    gen_blk_store_po_48(ctx, MO_TEQ, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LD_SD,
                        OFFSET_LD_SD, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sh_upo_48(DisasContext *ctx, arg_blk_sh_upo_48 *a)
{
    gen_blk_store_po_48(ctx, MO_TESW, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LH_SH,
                        UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sw_upo_48(DisasContext *ctx, arg_blk_sw_upo_48 *a)
{
    gen_blk_store_po_48(ctx, MO_TESL, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LW_SW,
                        UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sd_upo_48(DisasContext *ctx, arg_blk_sd_upo_48 *a)
{
    gen_blk_store_po_48(ctx, MO_TEQ, a->SrcD, a->SrcRType,
                        a->SrcR, a->SrcL, BYTES_LD_SD,
                        UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}
static void gen_blk_store_po_48_imm(DisasContext *ctx, MemOp memop,
                                    int imm, int SrcR,
                                    int SrcD, int width, int scaled_shift,
                                    bool is_addr, int RegDst, int reg)
{
    TCGv addr_base = get_src_regx(ctx, SrcR);
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv addr = temp_new(ctx);

    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    set_dst_regx(ctx, addr, reg);

    gen_blk_store_48_op(ctx, memop, is_addr, addr_base, data, width, RegDst);
}

static bool trans_blk_sbi_po_48(DisasContext *ctx, arg_blk_sbi_po_48 *a)
{
    gen_blk_store_po_48_imm(ctx, MO_SB, a->imm,  a->SrcR, a->SrcD, BYTES_LB_SB,
                            OFFSET_LB_SB, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shi_po_48(DisasContext *ctx, arg_blk_shi_po_48 *a)
{
    gen_blk_store_po_48_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcD, BYTES_LH_SH,
                            OFFSET_LH_SH, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swi_po_48(DisasContext *ctx, arg_blk_swi_po_48 *a)
{
    gen_blk_store_po_48_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcD, BYTES_LW_SW,
                            OFFSET_LW_SW, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdi_po_48(DisasContext *ctx, arg_blk_sdi_po_48 *a)
{
    gen_blk_store_po_48_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcD, BYTES_LD_SD,
                            OFFSET_LD_SD, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shi_upo_48(DisasContext *ctx, arg_blk_shi_upo_48 *a)
{
    gen_blk_store_po_48_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcD, BYTES_LH_SH,
                            UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swi_upo_48(DisasContext *ctx, arg_blk_swi_upo_48 *a)
{
    gen_blk_store_po_48_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcD, BYTES_LW_SW,
                            UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdi_upo_48(DisasContext *ctx, arg_blk_sdi_upo_48 *a)
{
    gen_blk_store_po_48_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcD, BYTES_LD_SD,
                            UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static void gen_blk_store_pair_48_imm(DisasContext *ctx, MemOp memop,
                                      int imm, int SrcR, int SrcD,
                                      int SrcD1, int width,
                                      int scaled_shift, bool is_addr,
                                      int RegDst)
{
    TCGv addr_base = get_src_regx(ctx, SrcR);
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv data1 = get_src_regx(ctx, SrcD1);
    TCGv addr = temp_new(ctx);
    TCGv addr1 = temp_new(ctx);

    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);
    tcg_gen_addi_tl(addr1, addr, width);

    gen_blk_store_48_op(ctx, memop, is_addr, addr, data, width, RegDst);
    gen_blk_store_48_op(ctx, memop, is_addr, addr1, data1, width, RegDst);
}

static bool trans_blk_sbip_48(DisasContext *ctx, arg_blk_sbip_48 *a)
{
    gen_blk_store_pair_48_imm(ctx, MO_SB, a->imm, a->SrcR, a->SrcD,
                              a->SrcD1, BYTES_LB_SB, OFFSET_LB_SB,
                              NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_ship_48(DisasContext *ctx, arg_blk_ship_48 *a)
{
    gen_blk_store_pair_48_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcD,
                              a->SrcD1, BYTES_LH_SH, OFFSET_LH_SH,
                              NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_swip_48(DisasContext *ctx, arg_blk_swip_48 *a)
{
    gen_blk_store_pair_48_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcD,
                              a->SrcD1, BYTES_LW_SW, OFFSET_LW_SW,
                              NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sdip_48(DisasContext *ctx, arg_blk_sdip_48 *a)
{
    gen_blk_store_pair_48_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcD,
                              a->SrcD1, BYTES_LD_SD, OFFSET_LD_SD,
                              NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_ship_u_48(DisasContext *ctx, arg_blk_ship_u_48 *a)
{
    gen_blk_store_pair_48_imm(ctx, MO_TESW, a->imm, a->SrcR, a->SrcD,
                              a->SrcD1, BYTES_LH_SH, UNSCALED,
                              NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_swip_u_48(DisasContext *ctx, arg_blk_swip_u_48 *a)
{
    gen_blk_store_pair_48_imm(ctx, MO_TESL, a->imm, a->SrcR, a->SrcD,
                              a->SrcD1, BYTES_LW_SW, UNSCALED,
                              NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sdip_u_48(DisasContext *ctx, arg_blk_sdip_u_48 *a)
{
    gen_blk_store_pair_48_imm(ctx, MO_TEQ, a->imm, a->SrcR, a->SrcD,
                              a->SrcD1, BYTES_LD_SD, UNSCALED,
                              NO_ADDR, REGDST_0);
    return true;
}


static void gen_blk_store_p_pr_48(DisasContext *ctx, MemOp memop,
                                  int SrcD, int SrcD1, int SrcRType,
                                  int SrcR, int SrcL,
                                  int width, int scaled_shift,
                                  bool is_addr, int RegDst, int dest)
{
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv data1 = get_src_regx(ctx, SrcD1);
    TCGv addr_base = get_src_regx(ctx, SrcL);
    TCGv shift_base = get_src_regx(ctx, SrcR);
    TCGv shift = conver_src_value(ctx, shift_base, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, scaled_shift);

    /* shift become dest */
    tcg_gen_add_tl(shift, addr_base, shift);

    set_dst_regx(ctx, shift, dest);

    gen_blk_store_48_op(ctx, memop, is_addr, shift, data, width, RegDst);
    gen_blk_store_48_op(ctx, memop, is_addr, addr_base, data1, width, RegDst);
}

static bool trans_blk_sbp_pr_48(DisasContext *ctx, arg_blk_sbp_pr_48 *a)
{
    gen_blk_store_p_pr_48(ctx, MO_SB, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LB_SB,
                          OFFSET_LB_SB, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shp_pr_48(DisasContext *ctx, arg_blk_shp_pr_48 *a)
{
    gen_blk_store_p_pr_48(ctx, MO_TESW, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LH_SH,
                          OFFSET_LH_SH, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swp_pr_48(DisasContext *ctx, arg_blk_swp_pr_48 *a)
{
    gen_blk_store_p_pr_48(ctx, MO_TESL, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LW_SW,
                          OFFSET_LW_SW, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdp_pr_48(DisasContext *ctx, arg_blk_sdp_pr_48 *a)
{
    gen_blk_store_p_pr_48(ctx, MO_TEQ, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LD_SD,
                          OFFSET_LD_SD, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shp_upr_48(DisasContext *ctx, arg_blk_shp_upr_48 *a)
{
    gen_blk_store_p_pr_48(ctx, MO_TESW, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LH_SH,
                          UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swp_upr_48(DisasContext *ctx, arg_blk_swp_upr_48 *a)
{
    gen_blk_store_p_pr_48(ctx, MO_TESL, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LW_SW,
                          UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdp_upr_48(DisasContext *ctx, arg_blk_sdp_upr_48 *a)
{
    gen_blk_store_p_pr_48(ctx, MO_TEQ, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LD_SD,
                          UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}
static void gen_blk_store_p_pr_48_imm(DisasContext *ctx, MemOp memop,
                                    int imm, int SrcR, int SrcD,
                                    int SrcD1, int width, int scaled_shift,
                                    bool is_addr, int RegDst, int reg)
{
    TCGv addr_base = get_src_regx(ctx, SrcR);
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv data1 = get_src_regx(ctx, SrcD1);
    TCGv addr = temp_new(ctx);

    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    set_dst_regx(ctx, addr, reg);

    gen_blk_store_48_op(ctx, memop, is_addr, addr, data, width, RegDst);
    gen_blk_store_48_op(ctx, memop, is_addr, addr_base, data1, width, RegDst);
}

static bool trans_blk_sbip_pr_48(DisasContext *ctx, arg_blk_sbip_pr_48 *a)
{
    gen_blk_store_p_pr_48_imm(ctx, MO_SB, a->imm,  a->SrcR,
                              a->SrcD, a->SrcD1, BYTES_LB_SB,
                              OFFSET_LB_SB, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_ship_pr_48(DisasContext *ctx, arg_blk_ship_pr_48 *a)
{
    gen_blk_store_p_pr_48_imm(ctx, MO_TESW, a->imm, a->SrcR,
                              a->SrcD, a->SrcD1, BYTES_LH_SH,
                              OFFSET_LH_SH, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swip_pr_48(DisasContext *ctx, arg_blk_swip_pr_48 *a)
{
    gen_blk_store_p_pr_48_imm(ctx, MO_TESL, a->imm, a->SrcR,
                              a->SrcD, a->SrcD1, BYTES_LW_SW,
                              OFFSET_LW_SW, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdip_pr_48(DisasContext *ctx, arg_blk_sdip_pr_48 *a)
{
    gen_blk_store_p_pr_48_imm(ctx, MO_TEQ, a->imm, a->SrcR,
                              a->SrcD, a->SrcD1, BYTES_LD_SD,
                              OFFSET_LD_SD, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_ship_upr_48(DisasContext *ctx, arg_blk_ship_upr_48 *a)
{
    gen_blk_store_p_pr_48_imm(ctx, MO_TESW, a->imm, a->SrcR,
                              a->SrcD, a->SrcD1, BYTES_LH_SH,
                              UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swip_upr_48(DisasContext *ctx, arg_blk_swip_upr_48 *a)
{
    gen_blk_store_p_pr_48_imm(ctx, MO_TESL, a->imm, a->SrcR,
                              a->SrcD, a->SrcD1, BYTES_LW_SW,
                              UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdip_upr_48(DisasContext *ctx, arg_blk_sdip_upr_48 *a)
{
    gen_blk_store_p_pr_48_imm(ctx, MO_TEQ, a->imm, a->SrcR,
                              a->SrcD, a->SrcD1, BYTES_LD_SD,
                              UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}


static void gen_blk_store_p_po_48(DisasContext *ctx, MemOp memop,
                                  int SrcD, int SrcD1, int SrcRType,
                                  int SrcR, int SrcL,
                                  int width, int scaled_shift,
                                  bool is_addr, int RegDst, int dest)
{
    TCGv data = get_src_regx(ctx, SrcD);
    TCGv data1 = get_src_regx(ctx, SrcD1);
    TCGv addr_base = get_src_regx(ctx, SrcL);
    TCGv shift_base = get_src_regx(ctx, SrcR);
    TCGv shift = conver_src_value(ctx, shift_base, INSTR_TYPE_CMP_SETC_LD_ST,
                                  SrcRType);

    tcg_gen_shli_tl(shift, shift, scaled_shift);

    /* shift become dest */
    tcg_gen_add_tl(shift, addr_base, shift);

    set_dst_regx(ctx, shift, dest);

    gen_blk_store_48_op(ctx, memop, is_addr, addr_base, data1, width, RegDst);
    gen_blk_store_48_op(ctx, memop, is_addr, addr_base, data, width, RegDst);
}

static bool trans_blk_sbp_po_48(DisasContext *ctx, arg_blk_sbp_po_48 *a)
{
    gen_blk_store_p_po_48(ctx, MO_SB, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LB_SB,
                          OFFSET_LB_SB, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shp_po_48(DisasContext *ctx, arg_blk_shp_po_48 *a)
{
    gen_blk_store_p_po_48(ctx, MO_TESW, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LH_SH,
                          OFFSET_LH_SH, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swp_po_48(DisasContext *ctx, arg_blk_swp_po_48 *a)
{
    gen_blk_store_p_po_48(ctx, MO_TESL, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LW_SW,
                          OFFSET_LW_SW, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdp_po_48(DisasContext *ctx, arg_blk_sdp_po_48 *a)
{
    gen_blk_store_p_po_48(ctx, MO_TEQ, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LD_SD,
                          OFFSET_LD_SD, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_shp_upo_48(DisasContext *ctx, arg_blk_shp_upo_48 *a)
{
    gen_blk_store_p_po_48(ctx, MO_TESW, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LH_SH,
                          UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_swp_upo_48(DisasContext *ctx, arg_blk_swp_upo_48 *a)
{
    gen_blk_store_p_po_48(ctx, MO_TESL, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LW_SW,
                          UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}

static bool trans_blk_sdp_upo_48(DisasContext *ctx, arg_blk_sdp_upo_48 *a)
{
    gen_blk_store_p_po_48(ctx, MO_TEQ, a->SrcD, a->SrcD1, a->SrcRType,
                          a->SrcR, a->SrcL, BYTES_LD_SD,
                          UNSCALED, NO_ADDR, REGDST_0, a->RegDst);
    return true;
}
static void gen_blk_load_48_symbol(DisasContext *ctx, MemOp memop,
                                   bool is_addr, int imm,
                                   int dest_reg_enc, int scaled_shift)
{
    TCGv addr_base = tcg_constant_tl(ctx->base.pc_next);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, imm << scaled_shift);

    gen_blk_load_32_op(ctx, memop, is_addr, addr, dest_reg_enc);
}

static bool trans_blk_lb_pcr_48(DisasContext *ctx, arg_blk_lb_pcr_48 *a)
{
    gen_blk_load_48_symbol(ctx, MO_SB, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lh_pcr_48(DisasContext *ctx, arg_blk_lh_pcr_48 *a)
{
    gen_blk_load_48_symbol(ctx, MO_TESW, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lw_pcr_48(DisasContext *ctx, arg_blk_lw_pcr_48 *a)
{
    gen_blk_load_48_symbol(ctx, MO_TESL, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_ld_pcr_48(DisasContext *ctx, arg_blk_ld_pcr_48 *a)
{
    gen_blk_load_48_symbol(ctx, MO_TEQ, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lbu_pcr_48(DisasContext *ctx, arg_blk_lbu_pcr_48 *a)
{
    gen_blk_load_48_symbol(ctx, MO_UB, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lhu_pcr_48(DisasContext *ctx, arg_blk_lhu_pcr_48 *a)
{
    gen_blk_load_48_symbol(ctx, MO_TEUW, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_lwu_pcr_48(DisasContext *ctx, arg_blk_lwu_pcr_48 *a)
{
    gen_blk_load_48_symbol(ctx, MO_TEUL, NO_ADDR, a->imm,
                           a->RegDst, UNSCALED);
    return true;
}

static void gen_blk_store_48_symbol(DisasContext *ctx, MemOp memop,
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

static bool trans_blk_sb_pcr_48(DisasContext *ctx, arg_blk_sb_pcr_48 *a)
{
    gen_blk_store_48_symbol(ctx, MO_SB, a->imm, a->SrcL, BYTES_LB_SB,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sh_pcr_48(DisasContext *ctx, arg_blk_sh_pcr_48 *a)
{
    gen_blk_store_48_symbol(ctx, MO_TESW, a->imm, a->SrcL, BYTES_LH_SH,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sw_pcr_48(DisasContext *ctx, arg_blk_sw_pcr_48 *a)
{
    gen_blk_store_48_symbol(ctx, MO_TESL, a->imm, a->SrcL, BYTES_LW_SW,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_sd_pcr_48(DisasContext *ctx, arg_blk_sd_pcr_48 *a)
{
    gen_blk_store_48_symbol(ctx, MO_TEQ, a->imm, a->SrcL, BYTES_LD_SD,
                            UNSCALED, NO_ADDR, REGDST_0);
    return true;
}

static bool trans_blk_prf_48(DisasContext *ctx, arg_blk_prf_48 *a)
{
    gen_blk_load_32(ctx, MO_TEQ, NO_ADDR, a->shamt, a->SrcRType,
                    a->SrcR, a->SrcL, REGDST_0);
    return true;
}

static bool trans_blk_prf_a_48(DisasContext *ctx, arg_blk_prf_a_48 *a)
{
    gen_blk_load_32(ctx, MO_TEQ, IS_ADDR, a->shamt, a->SrcRType,
                    a->SrcR, a->SrcL, a->RegDst);
    return true;
}

static bool trans_blk_prfi_u_48(DisasContext *ctx, arg_blk_prfi_u_48 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEQ, NO_ADDR, a->imm, a->SrcL,
                        REGDST_0, UNSCALED);
    return true;
}

static bool trans_blk_prfi_ua_48(DisasContext *ctx, arg_blk_prfi_ua_48 *a)
{
    gen_blk_load_32_imm(ctx, MO_TEQ, IS_ADDR, a->imm, a->SrcL,
                        a->RegDst, UNSCALED);
    return true;
}

static bool trans_blk_ssrget_48(DisasContext *ctx, arg_blk_ssrget_48 *a)
{
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    gen_helper_csrr(dest, cpu_env, tcg_constant_i32(a->SSRID));

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_ssrset_48(DisasContext *ctx, arg_blk_ssrset_48 *a)
{
    TCGv src = get_src_regx(ctx, a->SrcL);
    gen_helper_csrw(cpu_env, tcg_constant_i32(a->SSRID), src);

    return true;
}

static bool gen_amo_cmp_swap(DisasContext *ctx, arg_blk_casb_48 *a,
                             MemOp memop)
{
    TCGv addr_base = get_src_regx(ctx, a->SrcL);
    TCGv srcr = get_src_regx(ctx, a->SrcR);
    TCGv srcd = get_src_regx(ctx, a->SrcD);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    if (a->rl) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_STRL);
    }

    tcg_gen_atomic_cmpxchg_tl(dest, addr_base, srcr, srcd, ctx->mem_idx, memop);

    if (a->aq) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_LDAQ);
    }

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_casb_48(DisasContext *ctx, arg_blk_casb_48 *a)
{
    return gen_amo_cmp_swap(ctx, a, MO_SB);
}

static bool trans_blk_cash_48(DisasContext *ctx, arg_blk_cash_48 *a)
{
    return gen_amo_cmp_swap(ctx, a, MO_TESW);
}

static bool trans_blk_casw_48(DisasContext *ctx, arg_blk_casw_48 *a)
{
    return gen_amo_cmp_swap(ctx, a, MO_TESL);
}

static bool trans_blk_casd_48(DisasContext *ctx, arg_blk_casd_48 *a)
{
    return gen_amo_cmp_swap(ctx, a, MO_TEQ);
}

static bool load_immediate(DisasContext *ctx, arg_blk_lis_48 *a,
                      void (*func)(TCGv, TCGv))
{
    TCGv imm = tcg_constant_i64(a->imm);

    func(imm, imm);

    TCGv addr = get_dst_regx(ctx, a->RegDst);
    tcg_gen_mov_tl(addr, imm);

    set_dst_regx(ctx, addr, a->RegDst);
    return true;
}

static bool trans_blk_lis_48(DisasContext *ctx, arg_blk_lis_48 *a)
{
    return load_immediate(ctx, a, tcg_gen_ext32s_tl);
}

static bool trans_blk_liu_48(DisasContext *ctx, arg_blk_liu_48 *a)
{
    return load_immediate(ctx, a, tcg_gen_ext32u_tl);
}

static bool trans_blk_miadd_48(DisasContext *ctx, arg_blk_miadd_48 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    TCGv res = temp_new(ctx);
    tcg_gen_muli_tl(res, src2, a->imm);
    tcg_gen_add_tl(dest, src1, res);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_misub_48(DisasContext *ctx, arg_blk_misub_48 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    TCGv res = temp_new(ctx);
    tcg_gen_muli_tl(res, src2, a->imm);
    tcg_gen_sub_tl(dest, src1, res);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_bfi_48(DisasContext *ctx, arg_blk_bfi_48 *a)
{
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, a->RegDst);
    TCGv tmp = temp_new(ctx);

    int pos = a->M * BITS_PER_BYTE;
    int len = (a->N + 1) * BITS_PER_BYTE;

    tcg_gen_rotri_tl(tmp, src1, pos);
    tcg_gen_deposit_tl(tmp, tmp, src2, 0, len);
    tcg_gen_rotli_tl(dest, tmp, pos);

    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_ccat_48(DisasContext *ctx, arg_blk_ccat_48 *a)
{
    /*
     * 1. When shamt < 64:
     *    Upper 64bit (T register): src1<<shamt || src2>>(64-shamt)
     *    Low 64bit(RegDst)       : src2<<shamt || src1>>(64-shamt)
     * 2. When shamt >= 64:
     *    Upper 64bit (T register): src2<<(shamt-64) || src1>>(128-shamt)
     *    Low 64bit(RegDst)       : src1<<(shamt-64) || src2>>(128-shamt)
     */
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    int shamt = 0;

    if (a->shamt == 0) {
        set_dst_regx(ctx, src1, a->RegDst1);
        set_dst_regx(ctx, src2, a->RegDst0);
        return true;
    }
    if (a->shamt == 64) {
        set_dst_regx(ctx, src2, a->RegDst1);
        set_dst_regx(ctx, src1, a->RegDst0);
        return true;
    }

    if (a->shamt < 64) {
        shamt = a->shamt;
    } else {
        shamt = a->shamt - 64;
    }

    TCGv temp1 = tcg_temp_new();
    TCGv temp2 = tcg_temp_new();
    TCGv dest1 = tcg_temp_new();
    TCGv dest2 = tcg_temp_new();

    /* half 64bit */
    tcg_gen_shli_tl(temp1, src1, shamt);
    tcg_gen_shri_tl(temp2, src2, 64 - shamt);
    tcg_gen_or_tl(dest1, temp1, temp2);

    /* anthor half 64bit */
    tcg_gen_shli_tl(temp1, src2, shamt);
    tcg_gen_shri_tl(temp2, src1, 64 - shamt);
    tcg_gen_or_tl(dest2, temp1, temp2);

    if (a->shamt < 64) {
        set_dst_regx(ctx, dest1, a->RegDst1);
        set_dst_regx(ctx, dest2, a->RegDst0);
    } else {
        set_dst_regx(ctx, dest2, a->RegDst1);
        set_dst_regx(ctx, dest1, a->RegDst0);
    }

    tcg_temp_free(temp1);
    tcg_temp_free(temp2);
    tcg_temp_free(dest1);
    tcg_temp_free(dest2);
    return true;
}

static bool trans_blk_ccatw_48(DisasContext *ctx, arg_blk_ccatw_48 *a)
{
    /*
     * 1. When shamt < 64:
     *    Upper 32bit (T register): src1<<shamt || src2>>(64-shamt)
     *    Low 32bit(RegDst)       : src2<<shamt || src1>>(64-shamt)
     * 2. When shamt >= 64:
     *    Upper 32bit (T register): src2<<(shamt-64) || src1>>(128-shamt)
     *    Low 32bit(RegDst)       : src1<<(shamt-64) || src2>>(128-shamt)
     */
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    tcg_gen_ext32s_tl(src1, src1);
    TCGv src2 = get_src_regx(ctx, a->SrcR);
    tcg_gen_ext32s_tl(src2, src2);
    int shamt = 0;

    if (a->shamt == 0) {
        set_dst_regx(ctx, src1, a->RegDst1);
        set_dst_regx(ctx, src2, a->RegDst0);
        return true;
    }
    if (a->shamt == 64) {
        set_dst_regx(ctx, src2, a->RegDst1);
        set_dst_regx(ctx, src1, a->RegDst0);
        return true;
    }

    if (a->shamt < 64) {
        shamt = a->shamt;
    } else {
        shamt = a->shamt - 64;
    }

    TCGv temp1 = tcg_temp_new();
    TCGv temp2 = tcg_temp_new();
    TCGv dest1 = tcg_temp_new();
    TCGv dest2 = tcg_temp_new();

    /* half 64bit */
    tcg_gen_shli_tl(temp1, src1, shamt);
    tcg_gen_shri_tl(temp2, src2, 64 - shamt);
    tcg_gen_or_tl(dest1, temp1, temp2);

    /* anthor half 64bit */
    tcg_gen_shli_tl(temp1, src2, shamt);
    tcg_gen_shri_tl(temp2, src1, 64 - shamt);
    tcg_gen_or_tl(dest2, temp1, temp2);

    if (a->shamt < 64) {
        set_dst_regx(ctx, dest1, a->RegDst1);
        set_dst_regx(ctx, dest2, a->RegDst0);
    } else {
        set_dst_regx(ctx, dest2, a->RegDst1);
        set_dst_regx(ctx, dest1, a->RegDst0);
    }

    tcg_temp_free(temp1);
    tcg_temp_free(temp2);
    tcg_temp_free(dest1);
    tcg_temp_free(dest2);
    return true;
}

static bool trans_blk_qmt_48(DisasContext *ctx, arg_blk_qmt_48 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dst = get_dst_regx(ctx, a->RegDst);

    target_ulong cmd = 0;
    cmd = FIELD_DP64(cmd, GQM_CMD, I, a->i);
    cmd = FIELD_DP64(cmd, GQM_CMD, E, a->e);
    cmd = FIELD_DP64(cmd, GQM_CMD, S, a->s);
    cmd = FIELD_DP64(cmd, GQM_CMD, R, a->r);

    gen_helper_gqm_qmt(dst, cpu_env, tcg_constant_tl(cmd), src_l, src_r);

    set_dst_regx(ctx, dst, a->RegDst);

    return true;
}

static bool trans_blk_qpush_48(DisasContext *ctx, arg_blk_qpush_48 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dst = get_dst_regx(ctx, a->RegDst);

    target_ulong cmd = 0;
    cmd = FIELD_DP64(cmd, GQM_CMD, H, a->h);
    cmd = FIELD_DP64(cmd, GQM_CMD, E, a->e);
    cmd = FIELD_DP64(cmd, GQM_CMD, R, a->r);

    if (a->r) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_STRL);
    }
    gen_helper_gqm_qpush(dst, cpu_env, tcg_constant_tl(cmd), src_l, src_r);

    set_dst_regx(ctx, dst, a->RegDst);

    return true;
}

static bool trans_blk_qpop_48(DisasContext *ctx, arg_blk_qpop_48 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);

    target_ulong cmd = 0;
    cmd = FIELD_DP64(cmd, GQM_CMD, E, a->e);
    cmd = FIELD_DP64(cmd, GQM_CMD, R, a->r);

    gen_helper_gqm_qpop(cpu_env, tcg_constant_tl(cmd), src_l,
                        tcg_constant_tl(a->RegDst0));
    if (a->r) {
        tcg_gen_mb(TCG_MO_ALL | TCG_BAR_LDAQ);
    }
    return true;
}
