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

/* 16 bit encoding for body */
static bool trans_blk_c_movr_16(DisasContext *ctx, arg_blk_c_movr_16 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_mov_tl(dest, src_l);
    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_c_movi_16(DisasContext *ctx, arg_blk_c_movi_16 *a)
{
    TCGv imm = tcg_constant_tl(a->imm);
    TCGv dest = get_dst_regx(ctx, a->RegDst);

    tcg_gen_mov_tl(dest, imm);
    set_dst_regx(ctx, dest, a->RegDst);
    return true;
}

static bool trans_blk_c_setret_16(DisasContext *ctx, arg_blk_c_setret_16 *a)
{
    /*
     * LLVM/MC encodes C.SETRET as a halfword-scaled PC-relative target from
     * the SETRET instruction address. During translation ctx->base.pc_next is
     * still the current SETRET PC, so the runtime target is current_pc +
     * imm*2.
     */
    TCGv imm = tcg_constant_tl((target_long)a->imm * 2);
    TCGv addr = get_dst_regx(ctx, a->RegDst);
    tcg_gen_addi_tl(addr, imm, ctx->base.pc_next);

    set_dst_regx(ctx, addr, a->RegDst);
    tcg_gen_mov_tl(next_bpc, addr);
    ctx->next_bpc_explicit = true;
    ctx->next_bpc_target = ctx->base.pc_next + (target_ulong)a->imm * 2;
    return true;
}

static inline bool gen_comp_arith(DisasContext *ctx, arg_blk_c_add_16 *a,
                      void (*func)(TCGv, TCGv, TCGv))
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv src_r = get_src_regx(ctx, a->SrcR);
    TCGv dest = get_dst_regx(ctx, TARGET_REG_T);

    func(dest, src_l, src_r);

    set_dst_regx(ctx, dest, TARGET_REG_T);
    return true;
}

static bool trans_blk_c_add_16(DisasContext *ctx, arg_blk_c_add_16 *a)
{
    return gen_comp_arith(ctx, a, tcg_gen_add_tl);
}

static bool trans_blk_c_sub_16(DisasContext *ctx, arg_blk_c_sub_16 *a)
{
    return gen_comp_arith(ctx, a, tcg_gen_sub_tl);
}

static bool trans_blk_c_and_16(DisasContext *ctx, arg_blk_c_and_16 *a)
{
    return gen_comp_arith(ctx, a, tcg_gen_and_tl);
}

static bool trans_blk_c_or_16(DisasContext *ctx, arg_blk_c_or_16 *a)
{
    return gen_comp_arith(ctx, a, tcg_gen_or_tl);
}

static bool gen_comp_setc(DisasContext *ctx, arg_blk_c_setc_eq_16 *a,
                          TCGCond cond)
{
    allow_setc_writer(ctx);

    TCGv jump = temp_new(ctx);
    TCGv src1 = get_src_regx(ctx, a->SrcL);
    TCGv src2 = get_src_regx(ctx, a->SrcR);

    tcg_gen_movi_tl(carg_flag, BSTATE_CARG_CLEAN);
    tcg_gen_setcond_tl(cond, jump, src1, src2);
    tcg_gen_shli_tl(jump, jump, CARG_FLAG_PREDICATE_BIT);
    tcg_gen_or_tl(carg_flag, jump, carg_flag);
    if (ctx->base.pc_next == 0xffffffff809270eeULL ||
        ctx->base.pc_next == 0x809270eeULL) {
        fprintf(stderr,
                "LINX_SETC_270EE pc=0x%llx srcl=%d srcr=%d cond=%d\n",
                (unsigned long long)ctx->base.pc_next, a->SrcL, a->SrcR, cond);
    }
    return true;
}

static bool trans_blk_c_setc_eq_16(DisasContext *ctx, arg_blk_c_setc_eq_16 *a)
{
    return gen_comp_setc(ctx, a, TCG_COND_EQ);
}

static bool trans_blk_c_setc_ne_16(DisasContext *ctx, arg_blk_c_setc_ne_16 *a)
{
    return gen_comp_setc(ctx, a, TCG_COND_NE);
}

static void gen_comp_load(DisasContext *ctx, MemOp memop, arg_blk_c_lwi_16 *a,
                          int scaled_shift)
{
    TCGv addr_base = get_src_regx(ctx, a->SrcL);
    TCGv dest_Treg = get_dst_regx(ctx, TARGET_REG_T);

    TCGv addr = temp_new(ctx);
    tcg_gen_addi_tl(addr, addr_base, a->imm << scaled_shift);

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

    set_dst_regx(ctx, dest_Treg, TARGET_REG_T);
}

static bool trans_blk_c_lwi_16(DisasContext *ctx, arg_blk_c_lwi_16 *a)
{
    gen_comp_load(ctx, MO_TESL, a, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_c_ldi_16(DisasContext *ctx, arg_blk_c_ldi_16 *a)
{
    gen_comp_load(ctx, MO_TEQ, a, OFFSET_LD_SD);
    return true;
}

static void gen_comp_store(DisasContext *ctx, MemOp memop, arg_blk_c_swi_16 *a,
                           int width, int scaled_shift)
{
    TCGv data = get_src_regx(ctx, T_REG_OFFSET_1);
    TCGv addr_base = get_src_regx(ctx, a->SrcL);
    TCGv dest = temp_new(ctx);

    tcg_gen_addi_tl(dest, addr_base, a->imm << scaled_shift);

    if (linx_use_atomic_store_buf(ctx)) {
        TCGv_i32 size = tcg_constant_i32(width);
        gen_helper_store_data(cpu_env, dest, data, size);
    } else {
        tcg_gen_qemu_st_tl(data, dest, ctx->mem_idx, memop);
    }

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(ctx->base.pc_next)) {
        gen_helper_memprnt(dest, data, tcg_constant_tl(WRITE));
    }
}

static bool trans_blk_c_swi_16(DisasContext *ctx, arg_blk_c_swi_16 *a)
{
    gen_comp_store(ctx, MO_TESL, a, BYTES_LW_SW, OFFSET_LW_SW);
    return true;
}

static bool trans_blk_c_sdi_16(DisasContext *ctx, arg_blk_c_sdi_16 *a)
{
    gen_comp_store(ctx, MO_TEQ, a, BYTES_LD_SD, OFFSET_LD_SD);
    return true;
}

static bool trans_blk_c_addi_16(DisasContext *ctx, arg_blk_c_addi_16 *a)
{
    TCGv src_l = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, TARGET_REG_T);

    tcg_gen_addi_tl(dest, src_l, a->imm);

    set_dst_regx(ctx, dest, TARGET_REG_T);
    return true;
}

static bool trans_blk_c_setc_tgt_16(DisasContext *ctx, arg_blk_c_setc_tgt_16 *a)
{
    TCGv srcl = get_src_regx(ctx, a->SrcL);

    tcg_gen_mov_tl(carg_tgt, srcl);
    return true;
}

static bool gen_ext_16(DisasContext *ctx, arg_blk_sext_b_16 *a,
                       void (*func)(TCGv, TCGv))
{
    TCGv srcl = get_src_regx(ctx, a->SrcL);
    TCGv dest = get_dst_regx(ctx, TARGET_REG_T);

    func(dest, srcl);

    set_dst_regx(ctx, dest, TARGET_REG_T);
    return true;
}

static bool trans_blk_sext_b_16(DisasContext *ctx, arg_blk_sext_b_16 *a)
{
    return gen_ext_16(ctx, a, tcg_gen_ext8s_tl);
}

static bool trans_blk_sext_h_16(DisasContext *ctx, arg_blk_sext_h_16 *a)
{
    return gen_ext_16(ctx, a, tcg_gen_ext16s_tl);
}

static bool trans_blk_sext_w_16(DisasContext *ctx, arg_blk_sext_w_16 *a)
{
    return gen_ext_16(ctx, a, tcg_gen_ext32s_tl);
}

static bool trans_blk_zext_b_16(DisasContext *ctx, arg_blk_zext_b_16 *a)
{
    return gen_ext_16(ctx, a, tcg_gen_ext8u_tl);
}

static bool trans_blk_zext_h_16(DisasContext *ctx, arg_blk_zext_h_16 *a)
{
    return gen_ext_16(ctx, a, tcg_gen_ext16u_tl);
}

static bool trans_blk_zext_w_16(DisasContext *ctx, arg_blk_zext_w_16 *a)
{
    return gen_ext_16(ctx, a, tcg_gen_ext32u_tl);
}

static bool gen_comp_cmp(DisasContext *ctx, arg_blk_c_cmp_eqi_16 *a,
                         TCGCond cond)
{
    TCGv src1 = get_src_regx(ctx, T_REG_OFFSET_1);
    TCGv src2 = tcg_constant_tl(a->imm);
    TCGv dest = get_dst_regx(ctx, TARGET_REG_T);

    tcg_gen_setcond_tl(cond, dest, src1, src2);

    set_dst_regx(ctx, dest, TARGET_REG_T);

    return true;
}

static bool trans_blk_c_cmp_eqi_16(DisasContext *ctx, arg_blk_c_cmp_eqi_16 *a)
{
    return gen_comp_cmp(ctx, a, TCG_COND_EQ);
}

static bool trans_blk_c_cmp_nei_16(DisasContext *ctx, arg_blk_c_cmp_nei_16 *a)
{
    return gen_comp_cmp(ctx, a, TCG_COND_NE);
}

static bool trans_blk_c_slli_16(DisasContext *ctx, arg_blk_c_slli_16 *a)
{
    TCGv data = get_src_regx(ctx, T_REG_OFFSET_1);
    TCGv dest = get_dst_regx(ctx, TARGET_REG_T);

    tcg_gen_shli_tl(dest, data, a->imm);

    set_dst_regx(ctx, dest, TARGET_REG_T);
    return true;
}

static bool trans_blk_c_srli_16(DisasContext *ctx, arg_blk_c_srli_16 *a)
{
    TCGv data = get_src_regx(ctx, T_REG_OFFSET_1);
    TCGv dest = get_dst_regx(ctx, TARGET_REG_T);

    tcg_gen_shri_tl(dest, data, a->imm);

    set_dst_regx(ctx, dest, TARGET_REG_T);
    return true;
}

static bool trans_blk_c_ssrget_16(DisasContext *ctx, arg_blk_c_ssrget_16 *a)
{
    static_blk_type_check(ctx, (1 << HEAD_TYPE_SYS) | (1 << HEAD_TYPE_STD));
    TCGv dest = get_dst_regx(ctx, TARGET_REG_T);
    gen_helper_csrr(dest, cpu_env, tcg_constant_i32(a->SSRID));

    set_dst_regx(ctx, dest, TARGET_REG_T);
    return true;
}

static bool trans_blk_c_ebreak(DisasContext *ctx, arg_blk_c_ebreak *a)
{
    tcg_gen_movi_i32(scall_arg, a->imm);
    generate_exception(ctx, LINX_EXCP_BREAKPOINT);
    return true;
}
