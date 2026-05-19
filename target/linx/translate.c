/*
 * LINX emulation for qemu: main translation routines.
 *
 * Copyright (c) 2022 HiSilicon Technologies.
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

#include "linx_block_def.h"
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "disas/disas.h"
#include "exec/cpu_ldst.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "sysemu/tcg.h"
#include "linx_ldst.h"

#include "exec/translator.h"
#include "exec/log.h"

#include "trace.h"
#include <stdint.h>

/* global register indices */
static TCGv cpu_pc, bpc, tpc, next_bpc;
static TCGv cpu_gpr[GPR_REG_SIZE], blk_t[T_REG_SIZE], blk_u[U_REG_SIZE];
static TCGv_i32 blk_ri[RI_SIZE], blk_ro[RO_SIZE];
static TCGv predm, lane_num;
static TCGv_i32 t_idx, u_idx, is_relay, in_body;
static TCGv_i32 ri_idx, ro_idx;
static TCGv fvec_tumn_width[CPU_NB_LANE_NUM];
static TCGv fvec_tumn_valid[CPU_NB_LANE_NUM];
static TCGv csr_lc[3], csr_lb[3], tm_ext;
static TCGv csr_lc_sum, csr_lb_sum, enable_lane_num;
static TCGv ta, tb, tc, td, te, tf, tg, th, to, to1, to2, to3;
/* header_info include bhyp, blktype, brhtype, brhtype_extend */
static TCGv_i32 tile_reg_dst_num, tile_reg_src_num;
static TCGv_i64 header_info, need_combine_lbref;
static TCGv_i64 tpc1, bnext;
static TCGv linx_load_res;
static TCGv linx_load_val;
static TCGv csr_tp, csr_gp;
static TCGv_i32 scall_arg;
static TCGv carg_flag, carg_tgt;
static TCGv tileop_type, tileop_datatype;

#include "exec/gen-icount.h"

bool enable_force_tb_chained = false;

/* Load/Store whether to save addr */
#define IS_ADDR 1
#define NO_ADDR 0

/* Load/Store LC0 flag */
#define NO_LC0  0
#define HAS_LC0 1

/* Load/Store Scaled Offset */
#define UNSCALED 0
#define OFFSET_LB_SB    0
#define OFFSET_LH_SH    1
#define OFFSET_LW_SW    2
#define OFFSET_LD_SD    3

/* Load/Store Bytes */
#define BYTES_LB_SB 1
#define BYTES_LH_SH 2
#define BYTES_LW_SW 4
#define BYTES_LD_SD 8

/*
 * If an operation is being performed on less than TARGET_LONG_BITS,
 * it may require the inputs to be sign- or zero-extended; which will
 * depend on the exact operation being performed.
 */
typedef enum {
    EXT_NONE,
    EXT_SIGN,
    EXT_ZERO,
} DisasExtend;

typedef enum {
    EXT_ZERO_SIMT,
    EXT_SIGN_SIMT,
    EXT_NONE_SIMT,
} DisasExtend_SIMT;

typedef enum {
    REG_UTMN,
    REG_GPR,
    REG_P,
    REG_OTHER,
} DisasOutputType;

typedef struct DisasContext {
    DisasContextBase base;
    /* pc_succ_insn points to the instruction following base.pc_next */
    target_ulong pc_succ_insn;
    uint32_t opcode;
    uint32_t mem_idx;
    uint8_t ntemp;
    CPUState *cs;
    TCGv zero;
    /* Space for 3 operands plus 1 extra for address computation(4*2), and
       1 for simt lane execution flag, 1 for simt dest, 1 for temporary */
    TCGv temp[12];
    uint32_t u_idx;
    uint32_t t_idx;
    uint32_t ri_idx;
    uint32_t ro_idx;
    uint32_t fvec_t_idx[CPU_NB_LANE_NUM], fvec_u_idx[CPU_NB_LANE_NUM],
             fvec_m_idx[CPU_NB_LANE_NUM], fvec_n_idx[CPU_NB_LANE_NUM];
    /*
     * This variable records the width of the tumn register on each lane.
     * The bit is: lane_num * 4 reg_size * 4 reg_type * 2 width_bit = 32bit
     *  31      24 23      16 15       8 7        0
     * +----------+----------+----------+----------+
     * | ........ | ........ | ........ | ........ |
     * +----------+----------+----------+----------+
     * |  fvec_n  |  fvec_n  |  fvec_u  |  fvec_t  |
     * +----------+----------+----------+----------+
     * [1:0] == 0b00: 64 Bytes
     * [1:0] == 0b01: 32 Bytes
     * [1:0] == 0b10: 16 Bytes
     * [1:0] == 0b11: 8  Bytes
     * fvec_tumn_width[0] => lane[0] t/u/m/n reg_width_bit information
     * fvec_tumn_width[1] => lane[1] t/u/m/n reg_width_bit information
     * ...
     * fvec_tumn_width[7] => lane[7] t/u/m/n reg_width_bit information
     */
    target_long fvec_tumn_width[CPU_NB_LANE_NUM];

    /*
     * This variable are used to records whether the tumn register is valid
     * on each lane.
     * The bit is: lane_num * 4 reg_size * 4 reg_type * 4 valid_bit = 64bit
     * [0]: valid bit
     * [1]: set the bit when load bridge insn
     * [3:2]: the mem width of load bridge insn
     * [3:2] == 0: 64 Bytes
     * [3:2] == 1: 32 Bytes
     * [3:2] == 2: 16 Bytes
     * [3:2] == 3: 8  Bytes
     * fvec_tumn_valid[0] => lane[0] t/u/m/n reg_valid_bit information
     * fvec_tumn_valid[2] => lane[1] t/u/m/n reg_valid_bit information
     * ...
     * fvec_tumn_valid[7] => lane[7] t/u/m/n reg_valid_bit information
     */
    target_long fvec_tumn_valid[CPU_NB_LANE_NUM];
    bool block_commit;
    bool need_combine_lbref;
    target_long bpc, tpc1, bnext, carg_tgt;
    uint64_t header_info;
    uint32_t in_body;
    /* Lower 1 bit of the instruction, which is used to determine whether
        the instruction is a 16-bit or 32-bit instruction
    */
    uint32_t size, layer, blk_type, brh_type;
    /* Microinstruction length and block header length */
    int insn_size, head_size;
    #define CTX_INSTR_ATTR_CMP_MASK  0b1
    /*
     * Used for recording instruction attributes during translation
     * [0:1]: indicate cmp instruction
     */
    uint32_t instr_attr;
    int tile_reg_src_num, tile_reg_dst_num;
    uint32_t tileop_type;
    uint64_t predm;
    /*
     * used for constraint check, unified parameters
     */
    uint32_t c_args[4];

    /* mask check */
    DECLARE_BITMAP(total_get_ggpr_mask, GPR_REG_SIZE);
    DECLARE_BITMAP(reset_get_ggpr_mask, GPR_REG_SIZE);
    DECLARE_BITMAP(total_set_ggpr_mask, GPR_REG_SIZE);
    DECLARE_BITMAP(reset_set_ggpr_mask, GPR_REG_SIZE);

} DisasContext;

static void generate_exception(DisasContext *ctx, int excp)
{
    tcg_gen_movi_tl(cpu_pc, ctx->base.pc_next);
    gen_helper_raise_exception(cpu_env, tcg_constant_i32(excp));
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void gen_exception_illegal(DisasContext *ctx)
{
    tcg_gen_st_i32(tcg_constant_i32(ctx->opcode), cpu_env,
                   offsetof(CPULINXState, bins));
    generate_exception(ctx, LINX_EXCP_INSN_ILLEGAL);
}

__attribute__((unused))
static void gen_exception_illegal_fixup(DisasContext *ctx)
{
    generate_exception(ctx, LINX_EXCP_BLK_IVLD_FIXUP);
}

static bool linx_use_goto_tb(DisasContextBase *db, target_ulong dest)
{
#ifdef CONFIG_USER_ONLY
    if (enable_force_tb_chained) {
        if ((tb_cflags(db->tb) & CF_NO_GOTO_TB)) {
            return false;
        } else {
            return true;
        }
    }
#endif
    return translator_use_goto_tb(db, dest);
}

static void gen_goto_tb(DisasContext *ctx, int n, target_ulong dest)
{
    if (linx_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(n);
        tcg_gen_movi_tl(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, n);
    } else {
        tcg_gen_movi_tl(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
}

/*
 * Wrappers for getting reg values.
 *
 * The $zero register does not have cpu_gpr[0] allocated -- we supply the
 * constant zero as a source, and an uninitialized sink as destination.
 *
 * Further, we may provide an extension for word operations.
 */
static TCGv temp_new(DisasContext *ctx)
{
    assert(ctx->ntemp < ARRAY_SIZE(ctx->temp));
    return ctx->temp[ctx->ntemp++] = tcg_temp_new();
}

static void temp_free(DisasContext *ctx)
{
    for (int i = ctx->ntemp - 1; i >= 0; --i) {
        tcg_temp_free(ctx->temp[i]);
        ctx->temp[i] = NULL;
    }
    ctx->ntemp = 0;
}

static void linx_block_constraint_check(DisasContext *ctx, LINXConstraint c_id)
{
    CPULINXState *env = ctx->cs->env_ptr;
    switch (c_id) {
    case CONSTRAINT_A6_4: {
        if (get_blktype(ctx->header_info) == HEAD_TYPE_SIMT) {
            printf("tpc:0x%lx\n", ctx->base.pc_next);
            printf("load or store instr should not be in para block!\n");
            gen_exception_illegal(ctx);
        }
    } break;
    case CONSTRAINT_A6_8: {
        for (int i = 0; i < ctx->ri_idx; ++i) {
            if (ctx->c_args[0] != env->blk_ri[i]) {
                continue;
            }
            printf("tpc:0x%lx\n", ctx->base.pc_next);
            printf("rd instr output reg cannot be in"
                   "block input register list\n");
            gen_exception_illegal(ctx);
        }
    } break;
    default:
        assert(0);
    }
    return;
}

static void log_exec(const char *str, TCGv val)
{
    gen_helper_log_str(tcg_constant_tl((target_ulong)str));
    gen_helper_log(cpu_env, val);
}

/* Determine whether the scalar block is a decouple block */
static inline bool is_decouple_with_bio(DisasContext *ctx)
{
    return (get_blktype(ctx->header_info) != HEAD_TYPE_SIMT &&
            get_blkdcp(ctx->header_info));
}

static TCGv get_src_regx(DisasContext *ctx, int src)
{
    int idx = 0;
    uint32_t offset = src & 0x3;
    if (src == 0) {
        return ctx->zero;
    } else if (src > 0 && src < 24) {
        return cpu_gpr[src];
    } else if (src >= 24 && src < 28) {
        /* t register */
        idx = (ctx->t_idx - offset - 1 + T_REG_SIZE) % T_REG_SIZE;
        if (idx < 0) {
            g_assert_not_reached();
        }
        return blk_t[idx];
    } else if (src >= 28 && src < 32) {
        /* u register */
        idx = (ctx->u_idx - offset - 1 + U_REG_SIZE) % U_REG_SIZE;
        if (idx < 0) {
            g_assert_not_reached();
        }
        return blk_u[idx];
    }
    g_assert_not_reached();
}

static TCGv get_src_regx_fvec_extx(DisasContext *ctx, TCGv src, int src_width,
                                   DisasExtend_SIMT ext)
{
    TCGv t = temp_new(ctx);
    tcg_gen_mov_tl(t, src);
    switch (src_width) {
    case WIDTH_BYTE:
        if (ext == EXT_SIGN_SIMT) {
            tcg_gen_ext8s_tl(t, t);
        } else if (ext == EXT_ZERO_SIMT) {
            tcg_gen_ext8u_tl(t, t);
        } else {
            g_assert_not_reached();
        }
        break;
    case WIDTH_HALF:
        if (ext == EXT_SIGN_SIMT) {
            tcg_gen_ext16s_tl(t, t);
        } else if (ext == EXT_ZERO_SIMT) {
            tcg_gen_ext16u_tl(t, t);
        } else {
            g_assert_not_reached();
        }
        break;
    case WIDTH_WORD:
        if (ext == EXT_SIGN_SIMT) {
            tcg_gen_ext32s_tl(t, t);
        } else if (ext == EXT_ZERO_SIMT) {
            tcg_gen_ext32u_tl(t, t);
        } else {
            g_assert_not_reached();
        }
        break;
    case WIDTH_DOUBLE:
        break;
    default:
        g_assert_not_reached();
    }
    return t;
}

/* vector register offset from env */
#define VREG_OFS(func, base_addr)                   \
static uint32_t func(int lane_id, int offset)       \
{                                                   \
    return offsetof(CPULINXState, base_addr) +      \
           (lane_id * FVEC_REG_SIZE + offset) * 8;  \
}

VREG_OFS(vtreg_ofs, fvec_t[0][0])
VREG_OFS(vureg_ofs, fvec_u[0][0])
VREG_OFS(vmreg_ofs, fvec_m[0][0])
VREG_OFS(vnreg_ofs, fvec_n[0][0])

static TCGv get_src_regx_fvec(DisasContext *ctx, int src_code, int lane_id)
{
    TCGv tmp;
    int src = extract16(src_code, 0, 7);
    int offset = src & FVEC_REG_IDX_MASK;
    if ((src >= SRC_FVEC_VT_1 && src <= SRC_FVEC_VT_4) ||
        (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4)) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vtreg_ofs(lane_id, offset));
        return tmp;
    } else if ((src >= SRC_FVEC_VU_1 && src <= SRC_FVEC_VU_4) ||
        (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4)) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vureg_ofs(lane_id, offset));
        return tmp;
    } else if ((src >= SRC_FVEC_VM_1 && src <= SRC_FVEC_VM_4) ||
        (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4)) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vmreg_ofs(lane_id, offset));
        return tmp;
    } else if ((src >= SRC_FVEC_VN_1 && src <= SRC_FVEC_VN_4) ||
        (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4)) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vnreg_ofs(lane_id, offset));
        return tmp;
    } else if (src >= SRC_FVEC_RI0 && src <= SRC_FVEC_RI11) {
        tmp = temp_new(ctx);
        TCGv_i32 ri_idx = tcg_temp_new_i32();
        tcg_gen_movi_i32(ri_idx, src - SRC_FVEC_RI0);
        gen_helper_get_ri_gpr(tmp, cpu_env, ri_idx);
        tcg_temp_free_i32(ri_idx);
        return tmp;
    } else if (src >= SRC_FVEC_T_1 && src <= SRC_FVEC_U_4) {
        return get_src_regx(ctx, src & SRC_FVRC_REG_MASK);
    } else {
        switch (src) {
        case SRC_FVEC_LC0:
            gen_helper_update_lcreg(cpu_env, tcg_constant_i32(lane_id));
            return csr_lc[0];
        case SRC_FVEC_LC1:
            gen_helper_update_lcreg(cpu_env, tcg_constant_i32(lane_id));
            return csr_lc[1];
        case SRC_FVEC_LC2:
            gen_helper_update_lcreg(cpu_env, tcg_constant_i32(lane_id));
            return csr_lc[2];
        case SRC_FVEC_LB0:
            return csr_lb[0];
        case SRC_FVEC_LB1:
            return csr_lb[1];
        case SRC_FVEC_LB2:
            return csr_lb[2];
        case SRC_FVEC_TA:
            return ta;
        case SRC_FVEC_TB:
            return tb;
        case SRC_FVEC_TC:
            return tc;
        case SRC_FVEC_TD:
            return td;
        case SRC_FVEC_TE:
            return te;
        case SRC_FVEC_TF:
            return tf;
        case SRC_FVEC_TG:
            return tg;
        case SRC_FVEC_TH:
            return th;
        case SRC_FVEC_TO:
            return to;
        case SRC_FVEC_TO1:
            return to1;
        case SRC_FVEC_TO2:
            return to2;
        case SRC_FVEC_TO3:
            return to3;
        case SRC_FVEC_ZERO:
            return ctx->zero;
        case SRC_FVEC_PRED:
            return predm;
        default:
            g_assert_not_reached();
            break;
        }
    }
}

static void src_dst_reg_width_check(DisasContext *ctx,
                                    int lane_id, int src_code)
{
    int dst_width = 0;
    int src = extract16(src_code, 0, 7);
    int src_width = extract16(src_code, 7, FVEC_REG_WIDTH_BITNUM);
    int offset = src & FVEC_REG_IDX_MASK;
    bool reuse = extract16(src_code, 5, 2);
    if ((src >= SRC_FVEC_VT_1 && src <= SRC_FVEC_VT_4) ||
        (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4)) {
        offset = (DST_FVEC_VT * FVEC_REG_SIZE * FVEC_REG_WIDTH_BITNUM +
                  offset * FVEC_REG_WIDTH_BITNUM) % 64;
    } else if ((src >= SRC_FVEC_VU_1 && src <= SRC_FVEC_VU_4) ||
        (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4)) {
        offset = (DST_FVEC_VU * FVEC_REG_SIZE * FVEC_REG_WIDTH_BITNUM +
                  offset * FVEC_REG_WIDTH_BITNUM) % 64;
    } else if ((src >= SRC_FVEC_VM_1 && src <= SRC_FVEC_VM_4) ||
        (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4)) {
        offset = (DST_FVEC_VM * FVEC_REG_SIZE * FVEC_REG_WIDTH_BITNUM +
                  offset * FVEC_REG_WIDTH_BITNUM) % 64;
    } else if ((src >= SRC_FVEC_VN_1 && src <= SRC_FVEC_VN_4) ||
        (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4)) {
        offset = (DST_FVEC_VN * FVEC_REG_SIZE * FVEC_REG_WIDTH_BITNUM +
                  offset * FVEC_REG_WIDTH_BITNUM) % 64;
    } else {
        return;
    }

    /* todo: reg valid check */
    int is_valid = 1;
    if (!is_valid) {
        gen_helper_dynamic_reg_valid_check(cpu_env,
            tcg_constant_tl(ctx->base.pc_next), tcg_constant_i32(lane_id));
    }
    if (!reuse) {
        /* todo: reg valid set to zero */
    }

    dst_width = extract64(ctx->fvec_tumn_width[lane_id], offset,
                          FVEC_REG_WIDTH_BITNUM);

    if ((ctx->predm & 1ULL << lane_id) && src_width != dst_width) {
        log_exec("vt/vu/vw/vn width check in lane:", tcg_constant_tl(lane_id));
        gen_helper_dynamic_reg_width_check(cpu_env,
            tcg_constant_tl(ctx->base.pc_next), tcg_constant_i32(lane_id));
    }
}

static TCGv get_src_regx_fvec_with_width(DisasContext *ctx, int src_code,
                                         int lane_id)
{
    int src_width = extract16(src_code, 7, FVEC_REG_WIDTH_BITNUM);
    int ext = extract16(src_code, 9, 1);
    src_dst_reg_width_check(ctx, lane_id, src_code);
    TCGv src_reg = get_src_regx_fvec(ctx, src_code, lane_id);
    return get_src_regx_fvec_extx(ctx, src_reg, src_width, ext);
}

static inline void depos_dst_width(DisasContext *ctx, int lane_id, int offset,
                                int dst_width)
{
    int mask_length = FVEC_REG_SIZE * FVEC_REG_WIDTH_BITNUM;
    uint16_t field_val = extract64(ctx->fvec_tumn_width[lane_id],
                                   offset, mask_length);
    field_val = field_val << FVEC_REG_WIDTH_BITNUM;
    field_val = deposit64(field_val, 0, FVEC_REG_WIDTH_BITNUM, dst_width);
    ctx->fvec_tumn_width[lane_id] = deposit64(ctx->fvec_tumn_width[lane_id],
                                        offset, mask_length, field_val);
    tcg_gen_movi_tl(fvec_tumn_width[lane_id], ctx->fvec_tumn_width[lane_id]);
}

static void set_dst_width(DisasContext *ctx, int dst, int lane_id,
                          int dst_width)
{
    int offset = (dst * FVEC_REG_SIZE * FVEC_REG_WIDTH_BITNUM) % 64;
    if (dst >= DST_FVEC_VT && dst <= DST_FVEC_VN) {
        depos_dst_width(ctx, lane_id, offset, dst_width);
    } else {
        g_assert_not_reached();
    }
}

static TCGv set_dst_regx_fvec_ext(DisasContext *ctx, TCGv dst, int dst_width)
{
    TCGv t = temp_new(ctx);
    tcg_gen_mov_tl(t, dst);
    switch (dst_width) {
    case WIDTH_BYTE:
        tcg_gen_ext8u_tl(t, dst);
        break;
    case WIDTH_HALF:
        tcg_gen_ext16u_tl(t, dst);
        break;
    case WIDTH_WORD:
        tcg_gen_ext32u_tl(t, dst);
        break;
    case WIDTH_DOUBLE:
        break;
    default:
        g_assert_not_reached();
    }

    return t;
}

static void set_dst_regx(DisasContext *ctx, TCGv t, int dst)
{
    if (dst > 0 && dst < 24) {
        tcg_gen_mov_tl(cpu_gpr[dst], t);
        return;
    }

    if (dst == TARGET_REG_T) {
        /* push to T register */
        tcg_gen_mov_tl(blk_t[ctx->t_idx % T_REG_SIZE], t);
        tcg_gen_addi_i32(t_idx, t_idx, 1);
        ctx->t_idx++;
        return;
    } else if (dst == TARGET_REG_U) {
        /* push to U register */
        tcg_gen_mov_tl(blk_u[ctx->u_idx % U_REG_SIZE], t);
        tcg_gen_addi_i32(u_idx, u_idx, 1);
        ctx->u_idx++;
        return;
    }
    g_assert_not_reached();
    return;
}

#define SHIFT_RIGHT_3REGS_AND_SETDST(reg_ofs)               \
do {                                                        \
    tmp = tcg_temp_new();                                   \
    tcg_gen_ld_i64(tmp, cpu_env, reg_ofs(lane_id, 6));      \
    tcg_gen_st_i64(tmp, cpu_env, reg_ofs(lane_id, 7));      \
    tcg_gen_ld_i64(tmp, cpu_env, reg_ofs(lane_id, 5));      \
    tcg_gen_st_i64(tmp, cpu_env, reg_ofs(lane_id, 6));      \
    tcg_gen_ld_i64(tmp, cpu_env, reg_ofs(lane_id, 4));      \
    tcg_gen_st_i64(tmp, cpu_env, reg_ofs(lane_id, 5));      \
    tcg_gen_ld_i64(tmp, cpu_env, reg_ofs(lane_id, 3));      \
    tcg_gen_st_i64(tmp, cpu_env, reg_ofs(lane_id, 4));      \
    tcg_gen_ld_i64(tmp, cpu_env, reg_ofs(lane_id, 2));      \
    tcg_gen_st_i64(tmp, cpu_env, reg_ofs(lane_id, 3));      \
    tcg_gen_ld_i64(tmp, cpu_env, reg_ofs(lane_id, 1));      \
    tcg_gen_st_i64(tmp, cpu_env, reg_ofs(lane_id, 2));      \
    tcg_gen_ld_i64(tmp, cpu_env, reg_ofs(lane_id, 0));      \
    tcg_gen_st_i64(tmp, cpu_env, reg_ofs(lane_id, 1));      \
    tcg_gen_st_i64(t, cpu_env, reg_ofs(lane_id, 0));        \
    tcg_temp_free(tmp);                                     \
} while (0)

static void set_dst_regx_fvec(DisasContext *ctx, TCGv t, int dst_code,
                              int lane_id, DisasOutputType type)
{
    int dst = extract16(dst_code, 0, 7);
    int dst_width = extract16(dst_code, 7, FVEC_REG_WIDTH_BITNUM);
    TCGv tmp;

    if (type == REG_UTMN) {
        if (dst < SRC_FVEC_VT_1 || dst > SRC_FVEC_VN_4) {
            type = REG_OTHER;
        }
        set_dst_width(ctx, dst, lane_id, dst_width);
    }

    if (dst == DST_FVEC_VT) {
        /* push to T register */
        assert(type == REG_UTMN);
        SHIFT_RIGHT_3REGS_AND_SETDST(vtreg_ofs);
    } else if (dst == DST_FVEC_VU) {
        /* push to U register */
        assert(type == REG_UTMN);
        SHIFT_RIGHT_3REGS_AND_SETDST(vureg_ofs);
    } else if (dst == DST_FVEC_VM) {
        /* push to M register */
        assert(type == REG_UTMN);
        SHIFT_RIGHT_3REGS_AND_SETDST(vmreg_ofs);
    } else if (dst == DST_FVEC_VN) {
        /* push to N register */
        assert(type == REG_UTMN);
        SHIFT_RIGHT_3REGS_AND_SETDST(vnreg_ofs);
    } else if (dst == DST_FVEC_ZERO) {
        return;
    } else if (dst >= DST_FVEC_RO0 && dst <= DST_FVEC_RO3) {
        assert(type == REG_GPR);
        /* FIXME: The reduce command may cause errors in register output check. */
        TCGv_i32 ro_idx = tcg_temp_new_i32();
        tcg_gen_movi_i32(ro_idx, dst - DST_FVEC_RO0);
        gen_helper_set_ro_gpr(cpu_env, t, ro_idx);
        tcg_temp_free_i32(ro_idx);
    } else if (dst == DST_FVEC_T || dst == DST_FVEC_U) {
        set_dst_regx(ctx, t, (dst & SRC_FVRC_REG_MASK));
    } else if (dst == DST_FVEC_PRED) {
        if (ctx->instr_attr & CTX_INSTR_ATTR_CMP_MASK) {
            tmp = tcg_temp_new();
            tcg_gen_andi_tl(predm, predm,
                        (MAX_NUM_UNSIGNED_LONG - (1ull << lane_id)));
            tcg_gen_andi_tl(tmp, t, 1);
            tcg_gen_shli_tl(tmp, tmp, lane_id);
            tcg_gen_or_tl(predm, predm, tmp);
            tcg_temp_free(tmp);
        } else {
            tcg_gen_mov_tl(predm, t);
        }
    } else {
        g_assert_not_reached();
    }
}

static void set_dst_regx_fvec_with_width(DisasContext *ctx, TCGv t, int dst_id,
                                         int lane_id)
{
    int dst_width = extract16(dst_id, 7, FVEC_REG_WIDTH_BITNUM);
    int dst_code = extract16(dst_id, 0, 7);
    DisasOutputType type = REG_OTHER;
    if (dst_code >= SRC_FVEC_VT_1 && dst_code <= SRC_FVEC_VN_4) {
        type = REG_UTMN;
    }

    if (type == REG_UTMN) {
        /* When the output is GPR, the width does not need to be processed. */
        t = set_dst_regx_fvec_ext(ctx, t, dst_width);
    }
    set_dst_regx_fvec(ctx, t, dst_id, lane_id, type);
}

static TCGv get_dst_regx_fvec(DisasContext *ctx, int dst_code, int lane_id)
{
    int dst = extract16(dst_code, 0, 7);
    switch (dst) {
    case DST_FVEC_VT:
    case DST_FVEC_VU:
    case DST_FVEC_VM:
    case DST_FVEC_VN:
    case DST_FVEC_ZERO:
    case DST_FVEC_PRED:
    case DST_FVEC_RO0:
    case DST_FVEC_RO1:
    case DST_FVEC_RO2:
    case DST_FVEC_RO3:
        return tcg_temp_local_new();
    case DST_FVEC_T:
    case DST_FVEC_U:
        printf("simt instructions do not support writing to"
                "scalar registers.\n");
        printf("pc:0x%lx\n", ctx->base.pc_next);
        g_assert_not_reached();
    default:
        break;
    };
    g_assert_not_reached();
}

/*
 * This function gets the destination register of the current instruction
 * and increments the index of the u/t register. Therefore, this function
 * must be called after the set_dst_regx function.
 */
static TCGv get_dst_regx(DisasContext *ctx, int dst)
{
    if (dst == 0) { return temp_new(ctx); }
    if (dst > 0 && dst < 24) {
        return cpu_gpr[dst];
    }
    if (dst == TARGET_REG_T) {
        // return T register
        return blk_t[ctx->t_idx % T_REG_SIZE];
    }
    if (dst == TARGET_REG_U) {
        // return U register
        return blk_u[ctx->u_idx % U_REG_SIZE];
    }
    g_assert_not_reached();
}

static TCGv get_src_reg_extx(DisasContext *ctx, int src_enc,
                            DisasExtend ext)
{
    TCGv t = tcg_temp_local_new();
    TCGv tmp = get_src_regx(ctx, src_enc);
    tcg_gen_mov_tl(t, tmp);
    switch (ext) {
    case EXT_NONE:
        break;
    case EXT_SIGN:
        tcg_gen_ext32s_tl(t, t);
        break;
    case EXT_ZERO:
        tcg_gen_ext32u_tl(t, t);
        break;
    default:
        g_assert_not_reached();
    }
    return t;
}

static TCGv conver_src_value(DisasContext *ctx, TCGv SrcReg, int SrcType, int flag)
{
    TCGv t = temp_new(ctx);

    if (SrcType == INSTR_TYPE_CMP_SETC_LD_ST) {
        if (flag == AU_NONE) {
            tcg_gen_mov_tl(t, SrcReg);
        } else if (flag == AU_SW) {
            tcg_gen_ext32s_tl(t, SrcReg);
        } else if (flag == AU_UW) {
            tcg_gen_ext32u_tl(t, SrcReg);
        } else {
            fprintf(stderr, "error SrcRType:%d in INSTR_TYPE_AU_LD_ST\n", flag);
            g_assert_not_reached();
        }
    } else if (SrcType == INSTR_TYPE_AU) {
        if (flag == AU_NONE) {
            tcg_gen_mov_tl(t, SrcReg);
        } else if (flag == AU_SW) {
            tcg_gen_ext32s_tl(t, SrcReg);
        } else if (flag == AU_UW) {
            tcg_gen_ext32u_tl(t, SrcReg);
        } else if (flag == AU_NEG) {
            tcg_gen_neg_tl(t, SrcReg);
        } else {
            fprintf(stderr, "error SrcRType:%d in INSTR_TYPE_AU\n", flag);
            g_assert_not_reached();
        }
    } else if (SrcType == INSTR_TYPE_LU) {
        if (flag == REG_EXT_NONE) {
            tcg_gen_mov_tl(t, SrcReg);
        } else if (flag == REG_EXT_SW) {
            tcg_gen_ext32s_tl(t, SrcReg);
        } else if (flag == REG_EXT_UW) {
            tcg_gen_ext32u_tl(t, SrcReg);
        } else if (flag == REG_EXT_NOT) {
            tcg_gen_not_tl(t, SrcReg);
        } else {
            fprintf(stderr, "error SrcRType:%d in INSTR_TYPE_LU\n", flag);
            g_assert_not_reached();
        }
    } else {
        fprintf(stderr, "error SrcType:%d\n", SrcType);
        g_assert_not_reached();
    }

    return t;
}

#define TCG_GEN_ROTRI(ret, arg1, arg2, width, ext)  \
do {                                                \
    int len = arg2 >= width ? 64 : width;           \
    if (arg2 == 0) {                                \
        tcg_gen_mov_i64(ret, arg1);                 \
    } else {                                        \
        TCGv_i64 t0, t1;                            \
        t0 = tcg_temp_new_i64();                    \
        t1 = tcg_temp_new_i64();                    \
        ext(t1, arg1);                              \
        tcg_gen_shli_i64(t0, t1, len - arg2);       \
        tcg_gen_shri_i64(t1, t1, arg2);             \
        tcg_gen_or_i64(ret, t0, t1);                \
        tcg_temp_free_i64(t0);                      \
        tcg_temp_free_i64(t1);                      \
    }                                               \
} while (0)

static void tcg_gen_rotri(TCGv ret, TCGv arg1, int64_t arg2, int width)
{
    switch (width) {
    case 64:
        tcg_gen_rotri_tl(ret, arg1, arg2);
        break;
    case 32:
        TCG_GEN_ROTRI(ret, arg1, arg2, width, tcg_gen_ext32u_tl);
        break;
    case 16:
        TCG_GEN_ROTRI(ret, arg1, arg2, width, tcg_gen_ext16u_tl);
        break;
    case 8:
        TCG_GEN_ROTRI(ret, arg1, arg2, width, tcg_gen_ext8u_tl);
        break;
    default:
        tcg_gen_rotri_tl(ret, arg1, arg2);
        break;
    }
}

static TCGv add_offset_lc0(TCGv addr, int lane_id, int scaled_lc0, bool has_lc0)
{
    if (has_lc0) {
        TCGv tmp = tcg_temp_new();
        gen_helper_update_lcreg(cpu_env, tcg_constant_i32(lane_id));
        tcg_gen_shli_tl(tmp, csr_lc[0], scaled_lc0);
        tcg_gen_add_tl(addr, addr, tmp);
        tcg_temp_free(tmp);
    }
    return addr;
}

static inline void static_blk_type_check(DisasContext *ctx, uint64_t blk_types)
{
    if (!((blk_types >> ctx->blk_type) & 1)) {
        fprintf(stderr, "\nblktype error, bpc: 0x%lx, tpc: 0x%lx\n",
                ctx->bpc, ctx->base.pc_next);
        gen_exception_illegal(ctx);
    }
}

static inline void static_brh_type_check(DisasContext *ctx, uint64_t brh_types)
{
    if (!((brh_types >> ctx->brh_type) & 1)) {
        fprintf(stderr, "\nbrh_type error, bpc: 0x%lx, tpc: 0x%lx\n",
                ctx->bpc, ctx->base.pc_next);
        gen_exception_illegal(ctx);
    }
}

static inline target_long get_block_para(DisasContext *ctx)
{
    uint32_t blk_type = extract32(ctx->header_info, HEADER_INFO_BLKTYPE_START,
                                  HEADER_INFO_BLKTYPE_LEN);
    uint32_t br_type = extract32(ctx->header_info, HEADER_INFO_BRHTYPE_START,
                                  HEADER_INFO_BRHTYPE_LEN);
    uint32_t trap = extract32(ctx->header_info, HEADER_INFO_BATTR_START, 1);

    if (br_type == BRANCH_FALL) {
        br_type = 0;
    } else if (br_type == BRANCH_CALL || br_type == BRANCH_DIRECT_LINK) {
        br_type = 1;
    } else if (br_type == BRANCH_CONDITIONAL) {
        br_type = 2;
    } else if (br_type == BRANCH_IND || br_type == BRANCH_INDCALL ||
               br_type == BRANCH_RET) {
        br_type = 3;
    } else {
        gen_exception_illegal(ctx);
    }
    uint32_t flag =  trap << 7 | br_type << 4 | blk_type;
    return flag;
}

#define EX_SH(amount) \
    static int ex_shift_##amount(DisasContext *ctx, int imm) \
    {                                         \
        return imm << amount;                 \
    }
EX_SH(1)
EX_SH(3)
EX_SH(12)

/* Determine the inst length */
static int get_inst_len(uint16_t opcode)
{
    int inst_size = 0;
    if (extract64(opcode, 0, 1) == 0) {
        if (extract16(opcode, 1, 3) == 0b111) {
            inst_size = 6;
        } else {
            inst_size = 2;
        }
    } else if (extract64(opcode, 0, 4) == 0b1111) {
        inst_size = 8;
    } else {
        inst_size = 4;
    }
    return inst_size;
}

/* Determine if it's the head command */
static bool is_head_block(DisasContext *ctx, uint64_t opcode)
{
    int insn_size = get_inst_len(opcode);

    if (insn_size == 6) { /*  48 bit 指令 */
        CPULINXState *env = ctx->cs->env_ptr;
        opcode = cpu_ldl_code(env, ctx->pc_succ_insn);
        if (extract64(opcode, 16, 12) == 0b000000000001 ||
            extract64(opcode, 16, 12) == 0b000010000001 ||
            extract64(opcode, 16, 12) == 0b000100000001) {
            return true;
        }
    } else if (insn_size == 2) {  /*  16 bit 指令 */
        if ((extract16(opcode, 1, 3) <= 0b010) ||
            (extract16(opcode, 1, 5) >= 0b11110)) { /* B.DIM/B.DIMI */
            return true;
        }
    } else if (insn_size == 4) {  /*  32 bit 指令 */
        if (extract16(opcode, 0, 4) <= 0b0011) {
            return true;
        }
    } else if (insn_size == 8) {  /*  64 bit 指令 */
        /* 64 bit 指令的低32： 00000 00 ..... ..... 000 ..... ... 111 1 */
        CPULINXState *env = ctx->cs->env_ptr;
        opcode = cpu_ldq_code(env, ctx->pc_succ_insn);
        if (opcode == 0x10000000f) { /* L.BSTOP */
            return false;
        } else if (extract64(opcode, 33, 3) == 0b000) {
            return true;
        }
    }
    return false;
}

/*
 * Determine if it's the last insn, include BSTART, Memory operation-related
 * Template Blocks such as MEMCOPY.
 * To ensure BSTOP insn can also be included in the TB, BSTOP would be treated
 * as the last insn, not considered a commit insn. Instead, when translating to
 * BSTOP, the commit flag would be set to true.
 */
static bool next_is_commit_inst(DisasContext *ctx)
{
    CPULINXState *env = ctx->cs->env_ptr;
    uint64_t inst = cpu_lduw_code(env, ctx->pc_succ_insn);
/* todo: other cpu_load_code() alse need bswap if bigendian */
#ifdef TARGET_WORDS_BIGENDIAN
    inst = bswap16((uint16_t)inst);
#endif
    int insn_size = get_inst_len(inst);
    int bstop_op = 0x1;
    int c_bstop_op = 0x0;
    int c_bdim_op = 0b11110;
    uint64_t l_bstop_op = 0b1ull << 32 | 0b1111;

    if (is_head_block(ctx, inst)) {
        switch (insn_size) {
        case 2:
            if ((uint16_t)inst == c_bstop_op ||
                extract16((uint16_t)inst, 1, 5) == c_bdim_op) {
                return false;
            }
            return true;
            break;
        case 4:
            inst = cpu_ldl_code(env, ctx->pc_succ_insn);
            if ((uint32_t)inst == bstop_op) {
                return false;
            }
            /* b.hint begin(0x1033) and b.hint end(0x9033) */
            if (inst == 0x9033 || inst == 0x1033) {
                return true;
            }
            return extract16(inst, 0, 4) != 0b0011;
            break;
        case 6:
            return true;
        case 8:
            inst = cpu_ldq_code(env, ctx->pc_succ_insn);
            if (inst == l_bstop_op) {
                return false;
            }
            return true;
            break;
        default:
            break;
        }
    }
    return false;
}

/* Include Linx Block mini code decoder */
#include "decode-block16.c.inc"
#include "decode-block32.c.inc"
#include "decode-block48.c.inc"
#include "decode-block32_private_fvec.c.inc"

#include "insn_trans/trans_block_header.c.inc"
#include "insn_trans/trans_block_32.c.inc"
#include "insn_trans/trans_block_16.c.inc"
#include "insn_trans/trans_block_48.c.inc"
#include "insn_trans/trans_block_32_private_fvec.c.inc"

static void linx_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    CPULINXState *env = cs->env_ptr;
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    uint32_t tb_flags = ctx->base.tb->flags;

    ctx->pc_succ_insn = ctx->base.pc_first;
    ctx->mem_idx = FIELD_EX32(tb_flags, TB_FLAGS, MEM_IDX);
    ctx->cs = cs;
    ctx->ntemp = 0;
    memset(ctx->temp, 0, sizeof(ctx->temp));
    ctx->t_idx = env->t_idx;
    ctx->u_idx = env->u_idx;
    ctx->ri_idx = env->ri_idx;
    ctx->ro_idx = env->ro_idx;
    for (int i = 0; i < CPU_NB_LANE_NUM; ++i) {
        ctx->fvec_tumn_width[i] = env->fvec_tumn_width[i];
        ctx->fvec_tumn_valid[i] = env->fvec_tumn_valid[i];
    }
    ctx->block_commit = false;

    ctx->tpc1 = env->tpc1;
    ctx->need_combine_lbref = env->need_combine_lbref;
    ctx->in_body = env->in_body;
    ctx->bpc = env->bpc;
    ctx->bnext = env->bnext;
    ctx->carg_tgt = env->carg_tgt;
    ctx->header_info = env->header_info;
    ctx->tile_reg_dst_num = env->tile_reg_dst_num;
    ctx->tile_reg_src_num = env->tile_reg_src_num;

    ctx->blk_type = get_blktype(ctx->header_info);
    ctx->brh_type = get_brhtype(ctx->header_info);
    ctx->tileop_type = env->tileop_info.tileop_type;
    ctx->predm = env->predm;
    ctx->zero = tcg_constant_tl(0);
}

static void linx_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
}

static void linx_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next);
}

static void linx_gen_blk_commit(CPULINXState *env, DisasContext *ctx)
{
    /*
     * If DISAS_NORETURN is set, other microinstructions will be redirected to.
     * We do not consider that the commit is required. Like j and so on.
     */
    if (ctx->base.is_jmp == DISAS_NORETURN) {
        return;
    }

    /* Note: this must be put before gpr update */
    if (tb_cflags(ctx->base.tb) & CF_ATOMIC) {
        gen_helper_write_store_buf_to_mem(cpu_env);
        ctx->base.is_jmp = DISAS_NORETURN;
    }

    gen_dym_linx_blk_do_jump(env, ctx);
}

static void linx_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULINXState *env = cpu->env_ptr;
    uint64_t opcode = cpu_lduw_code(env, ctx->base.pc_next);

    ctx->instr_attr = 0;
    ctx->insn_size = get_inst_len(opcode);

    if (!is_head_block(ctx, opcode)) {
        if (!is_valid_linx_addr(ctx->tpc1)) {
            ctx->tpc1 = ctx->base.pc_next;
            tcg_gen_movi_i64(tpc1, ctx->base.pc_next);
        }
        if (!ctx->in_body) {
            ctx->in_body = true;
            tcg_gen_movi_i32(in_body, true);
            if (linx_is_atomic_blk(get_blk_atomic(ctx->header_info)) &&
                (tb_cflags(ctx->base.tb) & CF_PARALLEL)) {
                tcg_gen_movi_tl(cpu_pc, ctx->tpc1);
                gen_helper_clear_store_buf(cpu_env);
                gen_helper_jump_to_atomic_context(cpu_env);
            }
        }
        if ((ctx->insn_size == 2 || ctx->insn_size == 6) &&
            get_blktype(ctx->header_info) == HEAD_TYPE_SIMT) {
            printf("warning pc:0x%lx, 16 bits & 48 bits instr should "
                    "not be in simt block!\n", ctx->base.pc_next);
        }
    }

    trace_linx_mini(ctx->base.pc_next);
    ctx->pc_succ_insn = ctx->base.pc_next + ctx->insn_size;

    if (!is_head_block(ctx, opcode) &&
        ctx->brh_type == BRANCH_RET &&
        ctx->carg_tgt == ctx->base.pc_next) {
        /*
         * Split RET helpers can span several tiny non-head blocks before a
         * later c.setc.tgt ra writes the real return address. Keep the
         * provisional target moving forward across those microblocks instead
         * of self-looping on the current PC.
         */
        ctx->carg_tgt = ctx->pc_succ_insn;
        tcg_gen_movi_tl(carg_tgt, ctx->carg_tgt);
    }

    if (ctx->insn_size == 2) {
        opcode = linx_lduw_code(env, &ctx->base, ctx->base.pc_next);
        if (!decode_block16(ctx, (uint16_t)opcode)) {
            gen_exception_illegal(ctx);
        }
    } else if (ctx->insn_size == 4) {
        opcode = linx_ldl_code(env, &ctx->base, ctx->base.pc_next);
        if (!decode_block32(ctx, (uint32_t)opcode)) {
            gen_exception_illegal(ctx);
        }
    } else if  (ctx->insn_size == 6) {
        opcode = linx_ldq_code(env, &ctx->base, ctx->base.pc_next);
        if (!decode_block48(ctx, opcode)) {
            gen_exception_illegal(ctx);
        }
    } else if (ctx->insn_size == 8) {
        opcode = linx_ldq_code(env, &ctx->base, ctx->base.pc_next);
        if (!decode_block32_private_fvec(ctx, opcode)) {
            gen_exception_illegal(ctx);
        }
    }

    /* Prefetch an instruction. If the instruction is battr block header,
    not need to be submitted. Another block, it must be submitted curr */
    if (ctx->block_commit || next_is_commit_inst(ctx)) {
        if (get_blkdcp(ctx->header_info) && !ctx->in_body) {
            gen_linx_decouple_head_jump_body(ctx);
        } else {
            linx_gen_blk_commit(env, ctx);
        }
    }

    ctx->base.pc_next = ctx->pc_succ_insn;

    for (int i = ctx->ntemp - 1; i >= 0; --i) {
        tcg_temp_free(ctx->temp[i]);
        ctx->temp[i] = NULL;
    }
    ctx->ntemp = 0;

    if (ctx->base.is_jmp == DISAS_NEXT) {
        target_ulong page_start;

        page_start = ctx->base.pc_first & TARGET_PAGE_MASK;
        if (ctx->base.pc_next - page_start >= TARGET_PAGE_SIZE) {
            ctx->base.is_jmp = DISAS_TOO_MANY;
        }
    }
}

static void linx_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULINXState *env = cpu->env_ptr;

    switch (ctx->base.is_jmp) {
    case DISAS_TOO_MANY:
        if (env->tpc1 != LINX_ILLEGAL_INSTR_ADDR && enable_delay_block_intr) {
            gen_helper_set_next_flags(cpu_env, tcg_constant_i32(CF_NOIRQ));
        }
        /*
         * set CF_ATOMIC to the cflags of next tb so that it can continue to
         * execute atomically.
         */
        if (tb_cflags(ctx->base.tb) & CF_ATOMIC) {
            gen_helper_set_next_flags(cpu_env, tcg_constant_i32(CF_ATOMIC));
        }

        gen_goto_tb(ctx, 0, ctx->base.pc_next);
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static void linx_tr_disas_log(const DisasContextBase *dcbase, CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    LINXCPU *linxcpu = LINX_CPU(cpu);
    CPULINXState *env = &linxcpu->env;
#endif

    qemu_log("IN: %s\n", lookup_symbol(dcbase->pc_first));
#ifndef CONFIG_USER_ONLY
    qemu_log("Priv: "TARGET_FMT_ld"\n", env->priv);
#endif

#define ADDR_FMT "%08" PRIx64
    log_target_disas(cpu, dcbase->pc_first, dcbase->tb->size);
}

static const TranslatorOps linx_tr_ops = {
    .init_disas_context = linx_tr_init_disas_context,
    .tb_start           = linx_tr_tb_start,
    .insn_start         = linx_tr_insn_start,
    .translate_insn     = linx_tr_translate_insn,
    .tb_stop            = linx_tr_tb_stop,
    .disas_log          = linx_tr_disas_log,
};

void gen_intermediate_code(CPUState *cs, TranslationBlock *tb, int max_insns)
{
    DisasContext ctx;

    translator_loop(&linx_tr_ops, &ctx.base, cs, tb, max_insns);
}

void linx_translate_init(void)
{
    int i;

    /* In riscv,gpr[0] is zero,not writable;In blockISA, gpr[0] is available */
    for (i = 0; i < GPR_REG_SIZE; i++) {
        cpu_gpr[i] = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, gpr[i]), linx_int_regnames[i]);
    }

    for (i = 0; i < RI_SIZE; ++i) {
        blk_ri[i] =  tcg_global_mem_new_i32(cpu_env,
            offsetof(CPULINXState, blk_ri[i]), blk_ri_regnames[i]);
    }

    for (i = 0; i < RO_SIZE; ++i) {
        blk_ro[i] =  tcg_global_mem_new_i32(cpu_env,
            offsetof(CPULINXState, blk_ro[i]), blk_ro_regnames[i]);
    }
    for (i = 0; i < 3; ++i) {
        csr_lc[i] = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, csr_lc[i]), linx_lcnames[i]);
        csr_lb[i] = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, csr_lb[i]), linx_lbnames[i]);
    }
    csr_lc_sum = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, csr_lc_sum), "csr_lc_sum");
    csr_lb_sum = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, csr_lb_sum), "csr_lb_sum");
    enable_lane_num = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, enable_lane_num), "enable_lane_num");

    tm_ext = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, tm_ext), "tm_ext");

    for (i = 0; i < T_REG_SIZE; i++) {
        blk_t[i] = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, blk_t[i]), linx_int_blk_tnames[i]);
    }

    for (i = 0; i < U_REG_SIZE; i++) {
        blk_u[i] = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, blk_u[i]), linx_int_blk_unames[i]);
    }

    for (i = 0; i < CPU_NB_LANE_NUM; i++) {
        fvec_tumn_width[i] = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, fvec_tumn_width[i]), "fvec_tumn_width");
        fvec_tumn_valid[i] = tcg_global_mem_new(cpu_env,
            offsetof(CPULINXState, fvec_tumn_valid[i]), "fvec_tumn_valid");
    }

    lane_num = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, csr_lanenum),
                                  "lane_num");
    predm = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, predm), "predm");
    ta = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, ta), "ta");
    tb = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, tb), "tb");
    tc = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, tc), "tc");
    td = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, td), "td");
    te = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, te), "te");
    tf = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, tf), "tf");
    tg = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, tg), "tg");
    th = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, th), "th");
    to = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, to), "to");
    to1 = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, to1), "to1");
    to2 = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, to2), "to2");
    to3 = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, to3), "to3");
    tpc = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, tpc), "tpc");
    bpc = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, bpc), "bpc");
    next_bpc = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, next_bpc), "next_bpc");
    is_relay = tcg_global_mem_new_i32(cpu_env, offsetof(CPULINXState, is_relay), "is_relay");
    need_combine_lbref = tcg_global_mem_new(cpu_env,
        offsetof(CPULINXState, need_combine_lbref), "need_combine_lbref");
    bnext = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, bnext), "bnext");
    t_idx = tcg_global_mem_new_i32(cpu_env, offsetof(CPULINXState, t_idx), "t_idx");
    u_idx = tcg_global_mem_new_i32(cpu_env, offsetof(CPULINXState, u_idx), "u_idx");
    ri_idx = tcg_global_mem_new_i32(cpu_env, offsetof(CPULINXState, ri_idx),
        "ri_idx");
    ro_idx = tcg_global_mem_new_i32(cpu_env, offsetof(CPULINXState, ro_idx),
        "ro_idx");
    header_info = tcg_global_mem_new_i64(cpu_env,
        offsetof(CPULINXState, header_info), "header_info");
    tile_reg_dst_num = tcg_global_mem_new_i32(cpu_env,
        offsetof(CPULINXState, tile_reg_dst_num), "tile_reg_dst_num");
    tile_reg_src_num = tcg_global_mem_new_i32(cpu_env,
        offsetof(CPULINXState, tile_reg_src_num), "tile_reg_src_num");
    tileop_type = tcg_global_mem_new(cpu_env, offsetof(CPULINXState,
        tileop_info.tileop_type), "tileop_type");
    tileop_datatype = tcg_global_mem_new(cpu_env, offsetof(CPULINXState,
        tileop_info.tileop_datatype), "tileop_datatype");
    tpc1 = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, tpc1), "tpc1");
    in_body = tcg_global_mem_new_i32(cpu_env, offsetof(CPULINXState, in_body),
                                     "in_body");
    cpu_pc = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, pc), "pc");
    linx_load_res = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, linx_load_res),
                             "linx_load_res");
    linx_load_val = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, linx_load_val),
                             "linx_load_val");
    carg_flag = tcg_global_mem_new(cpu_env, offsetof(CPULINXState,
                                   carg_flag), "carg_flag");
    carg_tgt = tcg_global_mem_new(cpu_env, offsetof(CPULINXState,
                                   carg_tgt), "carg_tgt");
    csr_tp = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, csr_tp),
                                "tp");
    csr_gp = tcg_global_mem_new(cpu_env, offsetof(CPULINXState, csr_gp),
                                "gp");
    scall_arg = tcg_global_mem_new_i32(cpu_env,
        offsetof(CPULINXState, scall_arg), "scall_arg");
}
