/*
 * Linx Block ISA Header translation.
 *
 * Copyright (c) 2023 Hisilicon Technologies.
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

static bool is_template_block(uint32_t blktype)
{
    if (blktype >= HEAD_TYPE_MCOPY && blktype <= HEAD_TYPE_FRET_STK) {
        return true;
    }
    return false;
}

static void gen_memory_barrier(uint32_t block_atomic)
{
    switch (block_atomic & (BLK_NONE_AQ | BLK_NONE_RL)) {
        case (BLK_NONE_AQ | BLK_NONE_RL):
            tcg_gen_mb(TCG_BAR_SC);
            break;
        case BLK_NONE_AQ:
            tcg_gen_mb(TCG_BAR_LDAQ);
            break;
        case BLK_NONE_RL:
            tcg_gen_mb(TCG_BAR_STRL);
            break;
        default:
            break;
    }
}

#ifdef CONFIG_USER_ONLY
extern bool cs_call;
static void gen_check_cs_call(uint32_t blktype, uint32_t brhtype,
                              int64_t bnext, uint64_t bpc_t, uint64_t nbpc)
{
    if (!cs_call) {
        return;
    }
    TCGLabel *l;
    TCGv temp = tcg_temp_new();
    TCGv b_next= tcg_temp_new();
    switch (blktype) {
    case HEAD_TYPE_STD:
    case HEAD_TYPE_FP:
        switch (brhtype) {
        case BRANCH_CONDITIONAL:
            l = gen_new_label();
            TCGv predicate = tcg_constant_tl(CARG_FLAG_PREDICATE);
            tcg_gen_and_tl(temp, carg_flag, predicate);
            tcg_gen_brcondi_tl(TCG_COND_EQ, temp, BSTATE_CARG_COND_JUMP, l);
            gen_helper_blk_cpu_state_dump(cpu_env, bpc, tcg_constant_tl(nbpc));
            gen_set_label(l);
            gen_helper_blk_cpu_state_dump(cpu_env, bpc, b_next);
            break;
        case BRANCH_DIRECT_LINK:
        case BRANCH_CALL:
            gen_helper_blk_cpu_state_dump(cpu_env, bpc, b_next);
            break;
        case BRANCH_IND:
        case BRANCH_INDCALL:
        case BRANCH_RET:
            gen_helper_blk_cpu_state_dump(cpu_env, bpc, carg_tgt);
            break;
        default:
            break;
        }
    }
    tcg_temp_free(temp);
    tcg_temp_free(b_next);
}
#endif

static void gen_linx_reset_bstate(void)
{
    tcg_gen_movi_i32(in_body, false);
    tcg_gen_movi_tl(bpc, LINX_ILLEGAL_INSTR_ADDR);

#ifndef CONFIG_USER_ONLY
    tcg_gen_movi_tl(tpc, LINX_ILLEGAL_INSTR_ADDR);
    tcg_gen_movi_tl(next_bpc, LINX_ILLEGAL_INSTR_ADDR);
    tcg_gen_movi_tl(tpc1, LINX_ILLEGAL_INSTR_ADDR);
#endif
    return;
}

static void gen_do_jump(CPULINXState *env, DisasContext *ctx)
{
    TCGLabel *l;
    TCGv temp2 = tcg_temp_new();
#ifndef CONFIG_USER_ONLY
    TCGv retpc = tcg_temp_new();
#endif
    switch (ctx->brh_type) {
    case BRANCH_FALL:
        /* Whether the fall block is a decouple block */
        if (get_blkdcp(ctx->header_info)) {
            tcg_gen_mov_tl(cpu_pc, next_bpc);
            gen_linx_reset_bstate();
            tcg_gen_lookup_and_goto_ptr();
        } else {
            gen_linx_reset_bstate();
            if (ctx->blk_type == HEAD_TYPE_SYS) {
                tcg_gen_movi_tl(cpu_pc, ctx->pc_succ_insn);
                tcg_gen_exit_tb(NULL, 0);
            } else {
                gen_goto_tb(ctx, 0, ctx->pc_succ_insn);
            }
        }
        break;
    case BRANCH_INDCALL:
    case BRANCH_RET:
    case BRANCH_IND:
        tcg_gen_mov_tl(cpu_pc, carg_tgt);
        gen_linx_reset_bstate();
        tcg_gen_lookup_and_goto_ptr();
        break;
    case BRANCH_CALL:
    case BRANCH_DIRECT_LINK:
        gen_linx_reset_bstate();
        gen_goto_tb(ctx, 0, ctx->carg_tgt);
        break;
    case BRANCH_CONDITIONAL:
        l = gen_new_label();
        TCGv predicate = tcg_const_tl(CARG_FLAG_PREDICATE);
        tcg_gen_and_tl(temp2, carg_flag, predicate);
        gen_linx_reset_bstate();
        tcg_gen_brcondi_tl(TCG_COND_EQ, temp2, 1, l);
        gen_goto_tb(ctx, 0, ctx->pc_succ_insn);
        /* carg is BSTATE_CARG_COND_JUMP */
        gen_set_label(l);
        gen_goto_tb(ctx, 1, ctx->carg_tgt);
        break;
#ifndef CONFIG_USER_ONLY
    case BRANCH_ACRE:
        gen_helper_acre(retpc, cpu_env, scall_arg);
        tcg_gen_mov_tl(cpu_pc, retpc);
        tcg_temp_free(retpc);
        tcg_gen_lookup_and_goto_ptr();
        break;
#endif
    default:
        linx_debug_not_reached();
    }
    tcg_temp_free(temp2);
}

/* gen header jump with dynamic data change (for use in body decode) */
static void gen_dym_linx_blk_do_jump(CPULINXState *env, DisasContext *ctx)
{
#ifdef CONFIG_USER_ONLY
    gen_check_cs_call(ctx->blk_type,
                      ctx->brh_type,
                      env->bnext, env->bpc, env->next_bpc);
#endif
    TCGLabel *lvec;
    ctx->base.is_jmp = DISAS_NORETURN;
    TCGv tmp = temp_new(ctx);
    switch (ctx->blk_type) {
    case HEAD_TYPE_SIMT:
    {
        if (ctx->tileop_type == TILEOP_MPAR ||
            ctx->tileop_type == TILEOP_MSEQ ||
            ctx->tileop_type == TILEOP_VPAR ||
            ctx->tileop_type == TILEOP_VSEQ) {
            if (!get_blkdcp(ctx->header_info)) {
                env->tpc1 = ctx->pc_succ_insn;
            }
            gen_helper_update_predm(tmp, cpu_env);
            lvec = gen_new_label();
            tcg_gen_brcondi_tl(TCG_COND_EQ, tmp, 0, lvec);
            /* Continue block execution when loop is not finished */
            tcg_gen_movi_tl(cpu_pc, env->tpc1);
            tcg_gen_lookup_and_goto_ptr();
            gen_set_label(lvec);
            /* End of cycle */
            tcg_gen_mov_tl(cpu_pc, next_bpc);
            gen_linx_reset_bstate();
            /*
             * Clear header_info so that cpu_get_tb_cpu_state can use correct
             * flags.
             */
            tcg_gen_movi_i64(header_info, 0);
            tcg_gen_lookup_and_goto_ptr();
        /* Tileop Template Block */
        } else {
            gen_helper_tileop(cpu_env);
            tcg_gen_movi_tl(next_bpc, ctx->pc_succ_insn);
            gen_linx_reset_bstate();
            gen_goto_tb(ctx, 0, ctx->pc_succ_insn);
        }
        break;
    }
    case HEAD_TYPE_STD:
    case HEAD_TYPE_FP:
    case HEAD_TYPE_MCOPY:
    case HEAD_TYPE_MSET:
    case HEAD_TYPE_MPUSH:
    case HEAD_TYPE_MPOP:
    case HEAD_TYPE_FENTRY:
    case HEAD_TYPE_FEXIT:
    case HEAD_TYPE_FRET_RA:
    case HEAD_TYPE_FRET_STK:
    case HEAD_TYPE_SYS:
        /*
         * inline blcok set next_bpc. else is decouple block set next_bpc in
         * gen_linx_decouple_head_jump_body
         */
        if (!get_blkdcp(ctx->header_info)) {
            tcg_gen_movi_tl(next_bpc, ctx->pc_succ_insn);
        }
        gen_do_jump(env, ctx);
        break;
    default:
        linx_debug_not_reached();
    }
}

static void
gen_linx_decouple_head_jump_body(DisasContext *ctx)
{
    tcg_gen_movi_tl(next_bpc, ctx->pc_succ_insn);

    uint8_t blk_t = get_blktype(ctx->header_info);

    if (blk_t == HEAD_TYPE_SIMT) {
        TCGLabel *lvec;
        TCGv tmp = temp_new(ctx);
        gen_helper_enable_loop(tmp, cpu_env);
        lvec = gen_new_label();
        tcg_gen_brcondi_tl(TCG_COND_EQ, tmp, 0, lvec);

        /* make predicate mask */
        tcg_gen_umin_tl(tmp, csr_lb_sum, lane_num);
        tcg_gen_umin_tl(tmp, csr_lb[0], tmp);
        tcg_gen_mov_tl(enable_lane_num, tmp);
        tcg_gen_sub_tl(tmp, tcg_constant_tl(64), tmp);

        tcg_gen_movi_tl(cpu_pc, ctx->tpc1);
        tcg_gen_lookup_and_goto_ptr();
        gen_set_label(lvec);
        gen_linx_reset_bstate();
        /*
         * Clear header_info so that cpu_get_tb_cpu_state can use correct
         * flags.
         */
        tcg_gen_movi_i64(header_info, 0);
        gen_goto_tb(ctx, 1, ctx->pc_succ_insn);
    } else {
        gen_goto_tb(ctx, 0, ctx->tpc1);
    }

    ctx->base.is_jmp = DISAS_NORETURN;
}

static inline
void gen_init_header_info(DisasContext *ctx, int blk_type, int brh_type)
{
    uint64_t header = 0;
    header = deposit64(header, HEADER_INFO_BRHTYPE_START,
                       HEADER_INFO_BRHTYPE_LEN, brh_type);
    header = deposit64(header, HEADER_INFO_BLKTYPE_START,
                       HEADER_INFO_BLKTYPE_LEN, blk_type);
    header = deposit64(header, HEADER_INFO_SRCTYP_START,
                       HEADER_INFO_SRCTYP_LEN, 0x1F);
    ctx->blk_type = blk_type;
    ctx->brh_type = brh_type;
    ctx->header_info = header;
    tcg_gen_movi_i64(header_info, header);
}

static inline
void gen_linx_init_head_common(DisasContext *ctx, int brh_type, int blk_type,
                               int bnext_off)
{
    CPULINXState *env = ctx->cs->env_ptr;
    if (blk_type == HEAD_TYPE_SIMT) {
        tcg_gen_movi_tl(csr_lc[0], 0);
        tcg_gen_movi_tl(csr_lc[1], 0);
        tcg_gen_movi_tl(csr_lc[2], 0);
        tcg_gen_movi_tl(csr_lb[0], 1);
        tcg_gen_movi_tl(csr_lb[1], 1);
        tcg_gen_movi_tl(csr_lb[2], 1);
        ctx->tile_reg_src_num = 0;
        ctx->tile_reg_dst_num = 0;
        tcg_gen_movi_i32(tile_reg_dst_num, 0);
        tcg_gen_movi_i32(tile_reg_src_num, 0);
        ctx->ri_idx = 0;
        ctx->ro_idx = 0;
        tcg_gen_movi_i32(ri_idx, 0);
        tcg_gen_movi_i32(ro_idx, 0);
        for (int i = 0; i < CPU_NB_LANE_NUM; ++i) {
            env->fvec_tumn_width[i] = 0;
            env->fvec_tumn_valid[i] = 0;
            tcg_gen_movi_tl(fvec_tumn_width[i], 0);
            tcg_gen_movi_tl(fvec_tumn_valid[i], 0);
        }
    }

    if (!env->is_relay) {
        if (blk_type != HEAD_TYPE_SIMT) {
            ctx->t_idx &= 0x7;
            ctx->u_idx &= 0x7;
            tcg_gen_andi_i32(t_idx, t_idx, 0x7);
            tcg_gen_andi_i32(u_idx, u_idx, 0x7);
        }
    } else {
        tcg_gen_movi_i32(is_relay, 0);
    }

    if (!is_template_block(blk_type)) {
        bitmap_fill(ctx->reset_set_ggpr_mask, GPR_REG_SIZE);
    }

    ctx->bpc = ctx->base.pc_next;
    ctx->bnext = bnext_off;
    tcg_gen_movi_tl(bpc, ctx->base.pc_next);
    tcg_gen_movi_i64(bnext, bnext_off);
    gen_init_header_info(ctx, blk_type, brh_type);
}

#define GEN_LINX_INIT_HEAD(ctx, head_arg, branch_type, blk_type,               \
                                      bnext_off)                               \
do {                                                                           \
    gen_linx_init_head_common(ctx, branch_type, blk_type, bnext_off);          \
    ctx->carg_tgt = ctx->bpc + ctx->bnext;                                     \
    tcg_gen_movi_tl(carg_tgt, ctx->carg_tgt);                                  \
    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_SRCTYP_START,   \
                                 HEADER_INFO_SRCTYP_LEN, 0x1F);                \
    tcg_gen_movi_i64(header_info, ctx->header_info);                           \
} while (0)

static bool trans_blk_offset_btext(DisasContext *ctx, arg_blk_offset_btext *a)
{
    ctx->header_info = deposit64(ctx->header_info,
                                 HEADER_INFO_DECOUPLE_START,
                                 HEADER_INFO_DECOUPLE_LEN, 1);
    ctx->tpc1 = a->btext + ctx->base.pc_next;
    tcg_gen_movi_i64(header_info, ctx->header_info);
    tcg_gen_movi_tl(tpc1, ctx->tpc1);

    return true;
}

static bool trans_blk_memcopy(DisasContext *ctx, arg_blk_memcopy *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, HEAD_TYPE_MCOPY, 0);
    gen_helper_block_memcpy(cpu_env,
                            tcg_constant_i32(a->reg_src0),
                            tcg_constant_i32(a->reg_src1),
                            tcg_constant_i32(a->reg_src2));
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_memmove(DisasContext *ctx, arg_blk_memmove *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, HEAD_TYPE_MMOVE, 0);
    gen_helper_block_memmove(cpu_env,
                             tcg_constant_i32(a->reg_src0),
                             tcg_constant_i32(a->reg_src1),
                             tcg_constant_i32(a->reg_src2));
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_memset(DisasContext *ctx, arg_blk_memset *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, HEAD_TYPE_MSET, 0);
    gen_helper_block_memset(cpu_env,
                            tcg_constant_i32(a->reg_src0),
                            tcg_constant_i32(a->reg_src1),
                            tcg_constant_i32(a->reg_src2));
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_fentry(DisasContext *ctx, arg_blk_fentry *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, HEAD_TYPE_FENTRY, 0);
#ifndef CONFIG_USER_ONLY
    gen_helper_block_fentry(cpu_env, tcg_constant_i32(a->src_begin),
                            tcg_constant_i32(a->src_end),
                            tcg_constant_i32(a->imm));
#else
    TCGv target_addr = tcg_temp_new_i64();
    TCGv target_original = tcg_temp_new_i64();
    tcg_gen_mov_tl(target_addr, cpu_gpr[xSP]);
    tcg_gen_mov_tl(target_original, target_addr);

    if (a->src_begin > a->src_end) {
        gen_exception_illegal(ctx);
    }

    for (int i = a->src_begin; i <= a->src_end; i++) {
        tcg_gen_subi_tl(target_addr, target_addr, 8);
        tcg_gen_qemu_st_tl(cpu_gpr[i], target_addr, ctx->mem_idx, MO_TEQ);
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(ctx->base.pc_next)) {
            gen_helper_memprnt(target_addr, cpu_gpr[i], tcg_constant_tl(WRITE));
        }
    }

    tcg_gen_subi_tl(cpu_gpr[xSP], target_original, a->imm);
    tcg_temp_free(target_addr);
    tcg_temp_free(target_original);
#endif
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_fexit(DisasContext *ctx, arg_blk_fexit *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, HEAD_TYPE_FEXIT, 0);
#ifndef CONFIG_USER_ONLY
    gen_helper_block_fexit(cpu_env, tcg_constant_i32(a->dst_begin),
                           tcg_constant_i32(a->dst_end),
                           tcg_constant_i32(a->imm));
#else
    TCGv target_addr = tcg_temp_new_i64();
    tcg_gen_addi_tl(cpu_gpr[xSP], cpu_gpr[xSP], a->imm);
    tcg_gen_mov_tl(target_addr, cpu_gpr[xSP]);

    if (a->dst_begin > a->dst_end) {
        gen_exception_illegal(ctx);
    }

    for (int i = a->dst_begin; i <= a->dst_end; i++) {
        tcg_gen_subi_tl(target_addr, target_addr, 8);
        tcg_gen_qemu_ld_tl(cpu_gpr[i], target_addr, ctx->mem_idx, MO_TEQ);
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(ctx->base.pc_next)) {
            gen_helper_memprnt(target_addr, cpu_gpr[i], tcg_constant_tl(READ));
        }
    }
    tcg_temp_free(target_addr);
#endif
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_fret_ra(DisasContext *ctx, arg_blk_fret_ra *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_RET, HEAD_TYPE_FRET_RA, 0);
#ifndef CONFIG_USER_ONLY
    gen_helper_block_fret_ra(cpu_env, tcg_constant_i32(a->dst_begin),
                             tcg_constant_i32(a->dst_end),
                             tcg_constant_i32(a->imm));
#else
    TCGv target_addr = tcg_temp_new_i64();

    tcg_gen_mov_tl(carg_tgt, cpu_gpr[xRA]);

    tcg_gen_addi_tl(cpu_gpr[xSP], cpu_gpr[xSP], a->imm);
    tcg_gen_mov_tl(target_addr, cpu_gpr[xSP]);

    if (a->dst_begin > a->dst_end) {
        gen_exception_illegal(ctx);
    }

    for (int i = a->dst_begin; i <= a->dst_end; i++) {
        tcg_gen_subi_tl(target_addr, target_addr, 8);
        tcg_gen_qemu_ld_tl(cpu_gpr[i], target_addr, ctx->mem_idx, MO_TEQ);
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(ctx->base.pc_next)) {
            gen_helper_memprnt(target_addr, cpu_gpr[i], tcg_constant_tl(READ));
        }
    }
    tcg_temp_free(target_addr);
#endif
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_fret_stk(DisasContext *ctx, arg_blk_fret_stk *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_RET, HEAD_TYPE_FRET_STK, 0);
#ifndef CONFIG_USER_ONLY
    gen_helper_block_fret_stk(cpu_env, tcg_constant_i32(a->dst_begin),
                              tcg_constant_i32(a->dst_end),
                              tcg_constant_i32(a->imm));
#else
    TCGv target_addr = tcg_temp_new_i64();
    tcg_gen_addi_tl(cpu_gpr[xSP], cpu_gpr[xSP], a->imm);
    tcg_gen_mov_tl(target_addr, cpu_gpr[xSP]);

    if (a->dst_begin > a->dst_end) {
        gen_exception_illegal(ctx);
    }

    for (int i = a->dst_begin; i <= a->dst_end; i++) {
        tcg_gen_subi_tl(target_addr, target_addr, 8);
        tcg_gen_qemu_ld_tl(cpu_gpr[i], target_addr, ctx->mem_idx, MO_TEQ);
        if (i == a->dst_begin) {
            tcg_gen_mov_tl(carg_tgt, cpu_gpr[i]);
        }
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(ctx->base.pc_next)) {
            gen_helper_memprnt(target_addr, cpu_gpr[i], tcg_constant_tl(READ));
        }
    }
    tcg_temp_free(target_addr);
#endif
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_start_simt(DisasContext *ctx, arg_blk_start_simt *a)
{
    switch (a->para_mode) {
    case 0: /* vector mode */
    case 1: /* simt mode */
        tcg_gen_movi_tl(lane_num, CPU_NB_LANE_NUM);
        break;
    case 2:
    case 3:
        break;
    default:
        gen_exception_illegal(ctx);
        break;
    }
    if (a->para_op == 0x03) {
        ctx->tileop_type = (a->para_mode << T_MD_SFT) |
                           (a->para_op << T_OP_SFT)   | a->para_func;
    } else {
        ctx->tileop_type = (a->para_op << T_OP_SFT) | a->para_func;
    }
    tcg_gen_movi_tl(tileop_type, ctx->tileop_type);
    tcg_gen_movi_tl(tileop_datatype, a->para_datatype);
    gen_linx_init_head_common(ctx, BRANCH_FALL, HEAD_TYPE_SIMT, 0);

    return true;
}

static bool trans_blk_start_fall(DisasContext *ctx, arg_blk_start_fall *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, a->blk_type,
                               a->fixup_label);
    ctx->header_info |= HEADER_INFO_FIXUP_MASK;
    tcg_gen_movi_i64(header_info, ctx->header_info);

    return true;
}

static bool trans_blk_start_direct(DisasContext *ctx, arg_blk_start_direct *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_DIRECT_LINK, a->blk_type,
                               a->bnext_offset);

    return true;
}

static bool trans_blk_start_cond(DisasContext *ctx, arg_blk_start_cond *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_CONDITIONAL, a->blk_type,
                                a->bnext_offset);

    return true;
}

static bool trans_blk_start_call(DisasContext *ctx, arg_blk_start_call *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_CALL, a->blk_type, a->bnext_offset);

    return true;
}

static bool trans_blk_start_ind(DisasContext *ctx, arg_blk_start_ind *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_IND, a->blk_type, 0);

    return true;
}

static bool trans_blk_start_icall(DisasContext *ctx, arg_blk_start_icall *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_INDCALL, a->blk_type, 0);

    return true;
}

static bool trans_blk_start_ret(DisasContext *ctx, arg_blk_start_ret *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_RET, a->blk_type, 0);
    /*
     * Generic RET headers often rely on a later c.setc.tgt ra in a following
     * tiny block. If we seed carg_tgt with bpc+0 here, split return helpers
     * livelock on their own header before reaching that later target write.
     * Default to the next block header and let an explicit setc.tgt override.
     */
    ctx->carg_tgt = ctx->pc_succ_insn;
    tcg_gen_movi_tl(carg_tgt, ctx->carg_tgt);

    return true;
}

static bool trans_blk_offset_cond(DisasContext *ctx, arg_blk_offset_cond *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_CONDITIONAL, HEAD_TYPE_STD,
                                  a->bnext_low);
    return true;
}

static bool trans_blk_offset_direct_call(DisasContext *ctx,
                                         arg_blk_offset_direct_call *a)
{
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_CALL, HEAD_TYPE_STD, a->bnext_low);
    return true;
}

static bool trans_blk_biod(DisasContext *ctx, arg_blk_biod *a)
{
    /* do nothing */
    return true;
}

static bool trans_blk_xb(DisasContext *ctx, arg_blk_xb *a)
{
    return false;
}

static bool trans_blk_bior(DisasContext *ctx, arg_blk_bior *a)
{
    if (a->reg_src0) {
        tcg_gen_movi_i32(blk_ri[ctx->ri_idx++], a->reg_src0);
    }
    if (a->reg_src1) {
        tcg_gen_movi_i32(blk_ri[ctx->ri_idx++], a->reg_src1);
    }
    if (a->reg_src2) {
        tcg_gen_movi_i32(blk_ri[ctx->ri_idx++], a->reg_src2);
    }
    tcg_gen_movi_i32(ri_idx, ctx->ri_idx);

    if (a->reg_dst) {
        tcg_gen_movi_i32(blk_ro[ctx->ro_idx++], a->reg_dst);
        tcg_gen_movi_i32(ro_idx, ctx->ro_idx);
    }

    if (ctx->ri_idx > RI_SIZE || ctx->ro_idx > RO_SIZE) {
        qemu_log("ri_idx(%d)/ro_idx(%d) is too much, pc: 0x%lx\n",
            ctx->ri_idx, ctx->ro_idx, ctx->base.pc_next);
        gen_exception_illegal(ctx);
    }
    return true;
}

#define BIOT_IO_MODE0  0
#define BIOT_IO_MODE1  1
#define BIOT_IO_MODE2  2

static void gen_blk_biot(DisasContext *ctx, arg_blk_biot1 *a, int para)
{

    if (para >= 1) {
        gen_helper_biot(cpu_env,
                        tcg_constant_i32(ctx->tile_reg_src_num),
                        tcg_constant_i32(a->srctile0),
                        tcg_constant_i32(a->imm),
                        tcg_constant_i32(TILE_REG_SRC));
        ctx->tile_reg_src_num++;
    }

    if (para >= 2) {
        gen_helper_biot(cpu_env,
                        tcg_constant_i32(ctx->tile_reg_src_num),
                        tcg_constant_i32(a->srctile1),
                        tcg_constant_i32(a->imm),
                        tcg_constant_i32(TILE_REG_SRC));
        ctx->tile_reg_src_num++;
    }

    if (a->dsttile != 0b111) {
        gen_helper_biot(cpu_env,
                    tcg_constant_i32(ctx->tile_reg_dst_num),
                    tcg_constant_i32(a->dsttile),
                    tcg_constant_i32(a->imm),
                    tcg_constant_i32(TILE_REG_DST));
        ctx->tile_reg_dst_num++;
    }

    if (ctx->tile_reg_dst_num > MAX_DST_TILE_NUM ||
        ctx->tile_reg_src_num > MAX_SRC_TILE_NUM) {
        qemu_log("tile reg src num:(%d)/max(%d), dst num:(%d)/max(%d)"
                 "is too much, pc: 0x%lx\n", ctx->tile_reg_src_num,
                 MAX_SRC_TILE_NUM, ctx->tile_reg_dst_num, MAX_DST_TILE_NUM,
                 ctx->base.pc_next);
        gen_exception_illegal(ctx);
    }
}

static bool trans_blk_biot1(DisasContext *ctx, arg_blk_biot1 *a)
{
    gen_blk_biot(ctx, a, BIOT_IO_MODE2);
    return true;
}

static bool trans_blk_biot2(DisasContext *ctx, arg_blk_biot2 *a)
{
    assert(a->srctile1 == 0);
    gen_blk_biot(ctx, a, BIOT_IO_MODE1);
    return true;
}

static bool trans_blk_biot3(DisasContext *ctx, arg_blk_biot3 *a)
{
    assert(a->srctile0 == 0 && a->srctile1 == 0);
    gen_blk_biot(ctx, a, BIOT_IO_MODE0);
    return true;
}

static bool trans_blk_catr(DisasContext *ctx, arg_blk_catr *a)
{
    uint64_t header = (a->far << 7) | (a->atom << 6) | (a->aq << 5) |
                      (a->rl << 4) | (a->dr << 2) | (a->trap);


    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_BATTR_START,
                                 HEADER_INFO_BATTR_LEN, header);

    tcg_gen_movi_i64(header_info, ctx->header_info);
    gen_memory_barrier(get_blk_atomic(ctx->header_info));

    return true;
}

static bool trans_blk_datr(DisasContext *ctx, arg_blk_datr *a)
{
    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_FORMAT_START,
                                 HEADER_INFO_FORMAT_LEN, a->layout);
    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_SRCTYP_START,
                                 HEADER_INFO_SRCTYP_LEN, a->dat_typ);
    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_PAD_START,
                                 HEADER_INFO_PAD_LEN, a->pad);
    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_CM_START,
                                 HEADER_INFO_CM_LEN, a->cm);
    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_CANON_START,
                                 HEADER_INFO_CANON_LEN, a->c);
    ctx->header_info = deposit64(ctx->header_info, HEADER_INFO_SAT_START,
                                 HEADER_INFO_SAT_LEN, a->sat);
    assert(a->sat == 0);
    tcg_gen_movi_i64(header_info, ctx->header_info);
    gen_memory_barrier(get_blk_atomic(ctx->header_info));

    return true;
}

static bool trans_blk_hint(DisasContext *ctx, arg_blk_hint *a)
{

    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, HEAD_TYPE_STD, 0);
    return true;
}

static bool trans_blk_hint_trace(DisasContext *ctx, arg_blk_hint_trace *a)
{
    /* b.hint is no-op in QEMU */
    GEN_LINX_INIT_HEAD(ctx, a, BRANCH_FALL, HEAD_TYPE_STD, 0);
    return true;
}

static bool trans_blk_bdim(DisasContext *ctx, arg_blk_bdim *a)
{
    TCGv tmp = temp_new(ctx);
    tcg_gen_addi_tl(tmp, cpu_gpr[a->RegSrc], a->imm);
    switch (a->loopnest) {
    case 0:
        tcg_gen_mov_tl(csr_lb[0], tmp);
        break;
    case 1:
        tcg_gen_mov_tl(csr_lb[1], tmp);
        break;
    case 2:
        tcg_gen_mov_tl(csr_lb[2], tmp);
        break;
    default:
        gen_exception_illegal(ctx);
            break;
    }

    return true;
}

static bool trans_blk_start_stop(DisasContext *ctx, arg_blk_start_stop *a)
{
    ctx->block_commit = true;
    return true;
}

static bool trans_blk_short_head(DisasContext *ctx, arg_blk_short_head *a)
{
    /* branch type takes only the lower three bits */
    a->br_type &= 0x7;
    GEN_LINX_INIT_HEAD(ctx, a, a->br_type, a->blk_type, 0);
    if (a->br_type == BRANCH_RET) {
        /*
         * Short RET headers are used as split helpers in early kernel code.
         * They may reach c.setc.tgt ra only in a following tiny block, so a
         * self-targeted default livelocks at the header. Fall through first.
         */
        ctx->carg_tgt = ctx->pc_succ_insn;
        tcg_gen_movi_tl(carg_tgt, ctx->carg_tgt);
    }
    return true;
}

static bool trans_blk_c_bdimi(DisasContext *ctx, arg_blk_c_bdimi *a)
{
    switch (a->loopnest) {
    case 0:
        tcg_gen_movi_tl(csr_lb[0], a->imm);
        break;
    case 1:
        tcg_gen_movi_tl(csr_lb[1], a->imm);
        break;
    case 2:
        tcg_gen_movi_tl(csr_lb[2], a->imm);
        break;
    default:
        gen_exception_illegal(ctx);
        break;
    }

    return true;
}

static bool trans_blk_c_bdim(DisasContext *ctx, arg_blk_c_bdim *a)
{
    switch (a->loopnest) {
    case 0:
        tcg_gen_mov_tl(csr_lb[0], cpu_gpr[a->RegSrc]);
        break;
    case 1:
        tcg_gen_mov_tl(csr_lb[1], cpu_gpr[a->RegSrc]);
        break;
    case 2:
        tcg_gen_mov_tl(csr_lb[2], cpu_gpr[a->RegSrc]);
        break;
    default:
        gen_exception_illegal(ctx);
        break;
    }

    return true;
}
