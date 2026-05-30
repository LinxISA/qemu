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
#include "exec/address-spaces.h"
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
    bool next_bpc_explicit;
    target_ulong next_bpc_target;
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

static bool is_fused_call_setret_bundle(uint64_t opcode)
{
    uint16_t tail16 = opcode >> 48;

    /*
     * LLVM emits direct-call bundles as a 48-bit HL.BSTART.* CALL header
     * immediately followed by a 16-bit C.SETRET. That is not a real SIMT
     * 64-bit instruction and must not go through the private-fvec decoder.
     */
    return (tail16 & 0xf83f) == 0x5016;
}

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

static bool decode_regid10_queue_source(int src_code, int *legacy_src,
                                        int *width_offset)
{
    unsigned full = (unsigned)src_code & 0x3ffu;
    unsigned reg_class = (full >> 5) & 0x1fu;
    unsigned reg_index = full & 0x1fu;
    unsigned queue = 0;

    if (reg_class < 4u || reg_class > 7u || reg_index == 0 ||
        reg_index > FVEC_REG_SIZE) {
        return false;
    }

    queue = reg_class - 4u;
    *legacy_src = (int)(queue * 8u + (reg_index - 1u));
    if (reg_index <= FVEC_WIDTH_TRACKED_SIZE) {
        *width_offset = (int)((queue * FVEC_WIDTH_TRACKED_SIZE *
                               FVEC_REG_WIDTH_BITNUM) +
                              ((reg_index - 1u) * FVEC_REG_WIDTH_BITNUM));
    } else {
        *width_offset = -1;
    }
    return true;
}

static bool decode_regid10_queue_dest(int dst_code, int *queue_class,
                                      int *slot_index, int *width_offset)
{
    unsigned full = (unsigned)dst_code & 0x3ffu;
    unsigned reg_class = (full >> 5) & 0x1fu;
    unsigned reg_index = full & 0x1fu;

    if (reg_class < 4u || reg_class > 7u || reg_index == 0 ||
        reg_index > FVEC_REG_SIZE) {
        return false;
    }

    *queue_class = (int)(reg_class - 4u);
    *slot_index = (int)(reg_index - 1u);
    if (reg_index <= FVEC_WIDTH_TRACKED_SIZE) {
        *width_offset = (int)((*queue_class * FVEC_WIDTH_TRACKED_SIZE *
                               FVEC_REG_WIDTH_BITNUM) +
                              (*slot_index * FVEC_REG_WIDTH_BITNUM));
    } else {
        *width_offset = -1;
    }
    return true;
}

static bool decode_regid10_queue_push_dest(int dst_code, int *queue_class)
{
    unsigned full = (unsigned)dst_code & 0x3ffu;
    unsigned reg_class = (full >> 5) & 0x1fu;
    unsigned reg_index = full & 0x1fu;

    if (reg_class < 4u || reg_class > 7u || reg_index != 0) {
        return false;
    }

    *queue_class = (int)(reg_class - 4u);
    return true;
}

static bool decode_regid10_special_source(int src_code, int *reg_class,
                                          int *reg_index)
{
    unsigned full = (unsigned)src_code & 0x3ffu;
    unsigned klass = (full >> 5) & 0x1fu;
    unsigned index = full & 0x1fu;

    if (full == 92u) { /* predicate */
        *reg_class = 2;
        *reg_index = 28;
        return true;
    }

    switch (klass) {
    case 1: /* ri* */
    case 3: /* lc* */
    case 8: /* tile bases */
        *reg_class = (int)klass;
        *reg_index = (int)index;
        return true;
    default:
        return false;
    }
}

static TCGv get_src_regx_fvec(DisasContext *ctx, int src_code, int lane_id)
{
    TCGv tmp;
    int src = extract16(src_code, 0, 7);
    int offset = src & FVEC_REG_IDX_MASK;
    int width_offset = 0;
    int special_class = 0;
    int special_index = 0;

    if (decode_regid10_queue_source(src_code, &src, &width_offset)) {
        int queue_class = ((unsigned)src_code >> 5) - 4u;
        offset = (unsigned)src_code & 0x1fu;
        offset = offset ? offset - 1 : 0;
        tmp = temp_new(ctx);
        switch (queue_class) {
        case 0:
            tcg_gen_ld_i64(tmp, cpu_env, vtreg_ofs(lane_id, offset));
            return tmp;
        case 1:
            tcg_gen_ld_i64(tmp, cpu_env, vureg_ofs(lane_id, offset));
            return tmp;
        case 2:
            tcg_gen_ld_i64(tmp, cpu_env, vmreg_ofs(lane_id, offset));
            return tmp;
        case 3:
            tcg_gen_ld_i64(tmp, cpu_env, vnreg_ofs(lane_id, offset));
            return tmp;
        default:
            g_assert_not_reached();
        }
    }
    if (decode_regid10_special_source(src_code, &special_class, &special_index)) {
        switch (special_class) {
        case 1: /* ri* */
            if (special_index > SRC_FVEC_RI11 - SRC_FVEC_RI0) {
                g_assert_not_reached();
            }
            tmp = temp_new(ctx);
            TCGv_i32 ri_idx = tcg_temp_new_i32();
            tcg_gen_movi_i32(ri_idx, special_index);
            gen_helper_get_ri_gpr(tmp, cpu_env, ri_idx);
            tcg_temp_free_i32(ri_idx);
            return tmp;
        case 2: /* predicate */
            return predm;
        case 3: /* lc* */
            if (special_index < 0 || special_index > 2) {
                g_assert_not_reached();
            }
            gen_helper_update_lcreg(cpu_env, tcg_constant_i32(lane_id));
            return csr_lc[special_index];
        case 8: /* tile bases */
            switch (special_index) {
            case 0: return ta;
            case 1: return tb;
            case 2: return tc;
            case 3: return td;
            case 4: return to;
            case 5: return to1;
            default:
                g_assert_not_reached();
            }
        default:
            g_assert_not_reached();
        }
    }
    if (src < 32) {
        if (g_getenv("LINX_LOG_SIMT_ADDR") && ctx->base.pc_next == 0x1582a) {
            fprintf(stderr,
                    "LINX_SIMT_SRC_GPR pc=0x%llx src_code=0x%x src=%d\n",
                    (unsigned long long)ctx->base.pc_next, src_code, src);
        }
        return get_src_regx(ctx, src);
    }
    if (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vtreg_ofs(lane_id, offset));
        return tmp;
    } else if (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vureg_ofs(lane_id, offset));
        return tmp;
    } else if (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vmreg_ofs(lane_id, offset));
        return tmp;
    } else if (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4) {
        tmp = temp_new(ctx);
        tcg_gen_ld_i64(tmp, cpu_env, vnreg_ofs(lane_id, offset));
        return tmp;
    } else if (src >= SRC_FVEC_T_1 && src <= SRC_FVEC_U_4) {
        return get_src_regx(ctx, src & SRC_FVRC_REG_MASK);
    } else {
        switch (src) {
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
            fprintf(stderr,
                    "get_src_regx_fvec bad src_code=0x%x low7=0x%x lane=%d pc=0x%llx\n",
                    src_code, extract16(src_code, 0, 7), lane_id,
                    (unsigned long long)ctx->base.pc_next);
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
    int width_offset = 0;
    bool canonical_queue = decode_regid10_queue_source(src_code, &src,
                                                       &width_offset);
    bool queue_source = canonical_queue;
    int special_class = 0;
    int special_index = 0;
    if (canonical_queue) {
        offset = width_offset;
    } else if (decode_regid10_special_source(src_code, &special_class,
                                             &special_index)) {
        return;
    } else if (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4) {
        queue_source = true;
        offset = (DST_FVEC_VT * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
                  offset * FVEC_REG_WIDTH_BITNUM) % 64;
    } else if (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4) {
        queue_source = true;
        offset = (DST_FVEC_VU * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
                  offset * FVEC_REG_WIDTH_BITNUM) % 64;
    } else if (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4) {
        queue_source = true;
        offset = (DST_FVEC_VM * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
                  offset * FVEC_REG_WIDTH_BITNUM) % 64;
    } else if (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4) {
        queue_source = true;
        offset = (DST_FVEC_VN * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
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

    dst_width = WIDTH_WORD;
    if (offset >= 0) {
        dst_width = extract64(ctx->fvec_tumn_width[lane_id], offset,
                              FVEC_REG_WIDTH_BITNUM);
    }
    if ((canonical_queue && offset >= 0) ||
        (queue_source && src_width == WIDTH_DOUBLE && offset >= 0)) {
        src_width = dst_width;
    }

    if ((ctx->predm & 1ULL << lane_id) && src_width != dst_width) {
        fprintf(stderr,
                "linx-width-mismatch src_code=0x%x src=0x%x src_width=%d "
                "dst_width=%d reuse=%d offset=%d pc=0x%llx lane=%d\n",
                src_code, src, src_width, dst_width, reuse, offset,
                (unsigned long long)ctx->base.pc_next, lane_id);
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
    int legacy_src = 0;
    int width_offset = 0;
    int special_class = 0;
    int special_index = 0;

    if (decode_regid10_queue_source(src_code, &legacy_src, &width_offset)) {
        src_width = WIDTH_WORD;
        if (width_offset >= 0) {
            src_width = extract64(ctx->fvec_tumn_width[lane_id], width_offset,
                                  FVEC_REG_WIDTH_BITNUM);
        }
        ext = 0;
    } else if (decode_regid10_special_source(src_code, &special_class,
                                             &special_index)) {
        src_width = WIDTH_WORD;
        ext = 0;
    } else {
        int src = extract16(src_code, 0, 7);
        int offset = src & FVEC_REG_IDX_MASK;

        if (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4) {
            offset = (DST_FVEC_VT * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
                      offset * FVEC_REG_WIDTH_BITNUM) % 64;
        } else if (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4) {
            offset = (DST_FVEC_VU * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
                      offset * FVEC_REG_WIDTH_BITNUM) % 64;
        } else if (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4) {
            offset = (DST_FVEC_VM * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
                      offset * FVEC_REG_WIDTH_BITNUM) % 64;
        } else if (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4) {
            offset = (DST_FVEC_VN * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM +
                      offset * FVEC_REG_WIDTH_BITNUM) % 64;
        } else {
            offset = -1;
        }

        if (offset >= 0 && src_width == WIDTH_DOUBLE) {
            src_width = extract64(ctx->fvec_tumn_width[lane_id], offset,
                                  FVEC_REG_WIDTH_BITNUM);
        }
    }
    if (g_getenv("LINX_LOG_SIMT_ADDR") && ctx->base.pc_next == 0x1582a) {
        fprintf(stderr,
                "LINX_SIMT_SRC_WIDTH pc=0x%llx src_code=0x%x width=%d ext=%d lane=%d\n",
                (unsigned long long)ctx->base.pc_next, src_code, src_width,
                ext, lane_id);
    }
    src_dst_reg_width_check(ctx, lane_id, src_code);
    TCGv src_reg = get_src_regx_fvec(ctx, src_code, lane_id);
    return get_src_regx_fvec_extx(ctx, src_reg, src_width, ext);
}

static inline void depos_dst_width(DisasContext *ctx, int lane_id, int offset,
                                int dst_width)
{
    int mask_length = FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM;
    uint16_t field_val = extract64(ctx->fvec_tumn_width[lane_id],
                                   offset, mask_length);
    field_val = field_val << FVEC_REG_WIDTH_BITNUM;
    field_val = deposit64(field_val, 0, FVEC_REG_WIDTH_BITNUM, dst_width);
    ctx->fvec_tumn_width[lane_id] = deposit64(ctx->fvec_tumn_width[lane_id],
                                        offset, mask_length, field_val);
    tcg_gen_movi_tl(fvec_tumn_width[lane_id], ctx->fvec_tumn_width[lane_id]);
}

static void set_queue_slot_width(DisasContext *ctx, int queue_class, int lane_id,
                                 int slot_index, int dst_width)
{
    if (slot_index >= FVEC_WIDTH_TRACKED_SIZE) {
        return;
    }
    int width_offset =
        (queue_class * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM) +
        (slot_index * FVEC_REG_WIDTH_BITNUM);
    ctx->fvec_tumn_width[lane_id] = deposit64(ctx->fvec_tumn_width[lane_id],
                                              width_offset,
                                              FVEC_REG_WIDTH_BITNUM,
                                              dst_width);
    tcg_gen_movi_tl(fvec_tumn_width[lane_id], ctx->fvec_tumn_width[lane_id]);
}

static void set_dst_width(DisasContext *ctx, int dst, int lane_id,
                          int dst_width)
{
    int offset = (dst * FVEC_WIDTH_TRACKED_SIZE * FVEC_REG_WIDTH_BITNUM) % 64;
    if (dst >= DST_FVEC_VT && dst <= DST_FVEC_VN) {
        depos_dst_width(ctx, lane_id, offset, dst_width);
    } else {
        g_assert_not_reached();
    }
}

static int reserve_canonical_queue_push_slot(DisasContext *ctx, int queue_class,
                                             int lane_id)
{
    uint32_t *slot_index;

    switch (queue_class) {
    case 0:
        slot_index = &ctx->fvec_t_idx[lane_id];
        break;
    case 1:
        slot_index = &ctx->fvec_u_idx[lane_id];
        break;
    case 2:
        slot_index = &ctx->fvec_m_idx[lane_id];
        break;
    case 3:
        slot_index = &ctx->fvec_n_idx[lane_id];
        break;
    default:
        g_assert_not_reached();
    }

    if (*slot_index >= FVEC_REG_SIZE) {
        fprintf(stderr,
                "canonical fvec queue overflow queue=%d lane=%d slot=%u pc=0x%llx\n",
                queue_class, lane_id, *slot_index,
                (unsigned long long)ctx->base.pc_next);
        g_assert_not_reached();
    }

    return (*slot_index)++;
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
    if (dst == 0) {
        /*
         * Some helper/padding sequences materialize a RegDst encoding of 0,
         * which acts as a discard rather than a real architectural writeback.
         */
        return;
    }

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
    qemu_log_mask(LOG_GUEST_ERROR,
                  "set_dst_regx invalid dst=%d pc=0x%lx insn_size=%d "
                  "blk_type=%d brh_type=%d t_idx=%d u_idx=%d\n",
                  dst, ctx->base.pc_next, ctx->insn_size, ctx->blk_type,
                  ctx->brh_type, ctx->t_idx, ctx->u_idx);
    fprintf(stderr,
            "set_dst_regx invalid dst=%d pc=0x%llx insn_size=%d "
            "blk_type=%d brh_type=%d t_idx=%d u_idx=%d\n",
            dst, (unsigned long long)ctx->base.pc_next, ctx->insn_size,
            ctx->blk_type, ctx->brh_type, ctx->t_idx, ctx->u_idx);
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
    int queue_class = 0;
    int slot_index = 0;
    int width_offset = 0;
    int push_queue_class = 0;
    bool canonical_queue =
        decode_regid10_queue_dest(dst_code, &queue_class, &slot_index,
                                  &width_offset);
    bool canonical_push =
        decode_regid10_queue_push_dest(dst_code, &push_queue_class);
    TCGv tmp;

    if (type == REG_UTMN) {
        if (canonical_queue) {
            set_queue_slot_width(ctx, queue_class, lane_id, slot_index,
                                 WIDTH_WORD);
        } else if (canonical_push) {
            dst_width = WIDTH_WORD;
        } else if (dst < SRC_FVEC_VT_1 || dst > SRC_FVEC_VN_4) {
            type = REG_OTHER;
        } else {
            set_dst_width(ctx, dst, lane_id, dst_width);
        }
    }

    if (canonical_queue) {
        switch (queue_class) {
        case 0:
            tcg_gen_st_i64(t, cpu_env, vtreg_ofs(lane_id, slot_index));
            return;
        case 1:
            tcg_gen_st_i64(t, cpu_env, vureg_ofs(lane_id, slot_index));
            return;
        case 2:
            tcg_gen_st_i64(t, cpu_env, vmreg_ofs(lane_id, slot_index));
            return;
        case 3:
            tcg_gen_st_i64(t, cpu_env, vnreg_ofs(lane_id, slot_index));
            return;
        default:
            g_assert_not_reached();
        }
    } else if (canonical_push) {
        slot_index =
            reserve_canonical_queue_push_slot(ctx, push_queue_class, lane_id);
        set_queue_slot_width(ctx, push_queue_class, lane_id, slot_index,
                             dst_width);
        switch (push_queue_class) {
        case 0:
            tcg_gen_st_i64(t, cpu_env, vtreg_ofs(lane_id, slot_index));
            return;
        case 1:
            tcg_gen_st_i64(t, cpu_env, vureg_ofs(lane_id, slot_index));
            return;
        case 2:
            tcg_gen_st_i64(t, cpu_env, vmreg_ofs(lane_id, slot_index));
            return;
        case 3:
            tcg_gen_st_i64(t, cpu_env, vnreg_ofs(lane_id, slot_index));
            return;
        default:
            g_assert_not_reached();
        }
    } else if (dst == DST_FVEC_VT) {
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
    int queue_class = 0;
    int slot_index = 0;
    int width_offset = 0;
    DisasOutputType type = REG_OTHER;
    if (decode_regid10_queue_dest(dst_id, &queue_class, &slot_index,
                                  &width_offset) ||
        decode_regid10_queue_push_dest(dst_id, &queue_class) ||
        (dst_code >= DST_FVEC_VT && dst_code <= DST_FVEC_VN)) {
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
    int queue_class = 0;
    int slot_index = 0;
    int width_offset = 0;

    if (decode_regid10_queue_dest(dst_code, &queue_class, &slot_index,
                                  &width_offset) ||
        decode_regid10_queue_push_dest(dst_code, &queue_class)) {
        return tcg_temp_local_new();
    }
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
    fprintf(stderr,
            "get_dst_regx_fvec bad dst_code=0x%x low7=0x%x lane=%d pc=0x%llx\n",
            dst_code, dst, lane_id, (unsigned long long)ctx->base.pc_next);
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
        } else if (flag == AU_NEG) {
            tcg_gen_neg_tl(t, SrcReg);
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

static inline bool linx_use_atomic_store_buf(DisasContext *ctx)
{
    return linx_is_atomic_blk(get_blk_atomic(ctx->header_info));
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

    if (ctx->next_bpc_explicit && ctx->pc_succ_insn < ctx->next_bpc_target) {
        return false;
    }
/* todo: other cpu_load_code() alse need bswap if bigendian */
#ifdef TARGET_WORDS_BIGENDIAN
    inst = bswap16((uint16_t)inst);
#endif
    int insn_size = get_inst_len(inst);
    uint32_t next32;
    int bstop_op = 0x1;
    int c_bstop_op = 0x0;
    int c_bdim_op = 0b11110;
    uint64_t l_bstop_op = 0b1ull << 32 | 0b1111;

    if (is_head_block(ctx, inst)) {
        if (ctx->header_info != 0 && insn_size == 4) {
            next32 = linx_ldl_code(env, &ctx->base, ctx->pc_succ_insn + 4);
            if (((uint32_t)inst == 0x00001001 ||
                 (uint32_t)inst == 0x00001081) &&
                next32 != 0x40000023) {
                return false;
            }
        }
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

/*
 * Writes that change the active address context must end the current TB so the
 * next fetch is translated under the new MMU/privilege state.
 */
static inline bool csr_write_needs_tb_restart(int ssrid)
{
    switch (ssrid) {
    case CSTATE:
    case A0_MMTBASE:
    case A0_MMCONFIG:
    case A1_MMTBASE:
    case A1_MMCONFIG:
        return true;
    default:
        return false;
    }
}

static void gen_csrw(DisasContext *ctx, int ssrid, TCGv src)
{
    target_ulong restart_pc = ctx->pc_succ_insn;
    target_ulong linear_succ = ctx->base.pc_next + ctx->insn_size;

    if (linear_succ > restart_pc) {
        restart_pc = linear_succ;
    }

    if (g_getenv("LINX_LOG_MMU_CSR_TB") &&
        (ssrid == A1_MMTBASE || ssrid == A1_MMCONFIG ||
         ssrid == A0_MMTBASE || ssrid == A0_MMCONFIG)) {
        fprintf(stderr,
                "LINX_GEN_CSRW pc=%#" PRIx64 " succ=%#" PRIx64
                " linear=%#" PRIx64 " restart=%#" PRIx64
                " ssrid=%#x insn_size=%d\n",
                (uint64_t)ctx->base.pc_next, (uint64_t)ctx->pc_succ_insn,
                (uint64_t)linear_succ, (uint64_t)restart_pc,
                ssrid, ctx->insn_size);
    }
    gen_helper_csrw(cpu_env, tcg_constant_i32(ssrid), src);

    if (csr_write_needs_tb_restart(ssrid)) {
        tcg_gen_movi_tl(cpu_pc, restart_pc);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
    }
}

/*
 * Kernel-generated split helper blocks can legitimately arrive at setc
 * writers after branch-type bookkeeping has been flattened across tiny block
 * fragments. Preserve carg generation instead of treating those helpers as
 * architecturally illegal.
 */
static inline void allow_setc_writer(DisasContext *ctx)
{
    (void)ctx;
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
        ctx->fvec_t_idx[i] = 0;
        ctx->fvec_u_idx[i] = 0;
        ctx->fvec_m_idx[i] = 0;
        ctx->fvec_n_idx[i] = 0;
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

    if (ctx->base.pc_first == 0xffffffff80924724ULL ||
        ctx->base.pc_first == 0x80924724ULL) {
        fprintf(stderr,
                "LINX_INIT_4724 first=0x%llx hdr=0x%llx in_body=%d tpc1=0x%llx cond=%d\n",
                (unsigned long long)ctx->base.pc_first,
                (unsigned long long)ctx->header_info, ctx->in_body,
                (unsigned long long)ctx->tpc1,
                (ctx->header_info == 0 && ctx->in_body &&
                 ctx->tpc1 != LINX_ILLEGAL_INSTR_ADDR));
    }

    if (ctx->header_info == 0 && ctx->in_body &&
        ctx->tpc1 != LINX_ILLEGAL_INSTR_ADDR) {
        uint64_t hi = 0;

        hi = set_field(hi, HEADER_INFO_BRHTYPE_MASK, BRANCH_FALL);
        hi = set_field(hi, HEADER_INFO_BLKTYPE_MASK, HEAD_TYPE_STD);
        hi = deposit64(hi, HEADER_INFO_SRCTYP_START,
                       HEADER_INFO_SRCTYP_LEN, 0x1f);
        ctx->header_info = hi;
        ctx->bpc = ctx->tpc1;
        if (ctx->tpc1 == 0xffffffff80924724ULL ||
            ctx->tpc1 == 0x80924724ULL) {
            fprintf(stderr,
                    "LINX_SYNTH_4724 bpc=0x%llx hdr=0x%llx\n",
                    (unsigned long long)ctx->bpc,
                    (unsigned long long)ctx->header_info);
        }
        tcg_gen_movi_i64(header_info, hi);
        tcg_gen_movi_tl(bpc, ctx->bpc);
    }

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
    if (linx_use_atomic_store_buf(ctx)) {
        gen_helper_write_store_buf_to_mem(cpu_env);
        ctx->base.is_jmp = DISAS_NORETURN;
    }

    gen_dym_linx_blk_do_jump(env, ctx);
}

static void linx_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULINXState *env = cpu->env_ptr;
    uint8_t low_page_before[8];
    bool log_low_before = false;

    if (g_getenv("LINX_LOG_LOW_PAGE_BEFORE_FETCH") &&
        ctx->base.pc_next == 0x40) {
        log_low_before = true;
        address_space_read(&address_space_memory, 0x5c,
                           MEMTXATTRS_UNSPECIFIED, low_page_before,
                           sizeof(low_page_before));
        fprintf(stderr,
                "LINX_LOW_PAGE_BEFORE_FETCH bytes_5c=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                low_page_before[0], low_page_before[1], low_page_before[2],
                low_page_before[3], low_page_before[4], low_page_before[5],
                low_page_before[6], low_page_before[7]);
    }
    uint64_t opcode = cpu_lduw_code(env, ctx->base.pc_next);
    uint64_t opcode64 = 0;
    bool fused_call_setret = false;
    bool is_head;
    bool nested_explicit_head = false;

    if (g_getenv("LINX_LOG_LOW_PAGE_STATE") &&
        (ctx->base.pc_next == 0x40 || ctx->base.pc_next == 0x54)) {
        static bool logged_40;
        static bool logged_54;
        bool *logged = ctx->base.pc_next == 0x40 ? &logged_40 : &logged_54;
        void *base = tlb_vaddr_to_host(env, 0, MMU_INST_FETCH,
                                       cpu_mmu_index(env, true));

        if (base && !*logged) {
            uint8_t *p = (uint8_t *)base + 0x5c;
            *logged = true;
            fprintf(stderr,
                    "LINX_LOW_PAGE_STATE@0x%llx bytes_5c=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    (unsigned long long)ctx->base.pc_next,
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }

    if (log_low_before) {
        uint8_t low_page_after[8];

        address_space_read(&address_space_memory, 0x5c,
                           MEMTXATTRS_UNSPECIFIED, low_page_after,
                           sizeof(low_page_after));
        fprintf(stderr,
                "LINX_LOW_PAGE_AFTER_FETCH bytes_5c=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                low_page_after[0], low_page_after[1], low_page_after[2],
                low_page_after[3], low_page_after[4], low_page_after[5],
                low_page_after[6], low_page_after[7]);
    }

    ctx->instr_attr = 0;
    ctx->insn_size = get_inst_len(opcode);
    if (ctx->insn_size == 8) {
        opcode64 = linx_ldq_code(env, &ctx->base, ctx->base.pc_next);
        fused_call_setret = is_fused_call_setret_bundle(opcode64);
    }
    is_head = fused_call_setret ? true : is_head_block(ctx, opcode);
    if (is_head && ctx->next_bpc_explicit &&
        ctx->base.pc_next < ctx->next_bpc_target) {
        if ((ctx->insn_size == 4 &&
             linx_ldl_code(env, &ctx->base, ctx->base.pc_next) == 0x00001001) ||
            (ctx->insn_size == 2 &&
             linx_lduw_code(env, &ctx->base, ctx->base.pc_next) == 0x0800)) {
            nested_explicit_head = true;
            is_head = false;
        }
    } else if (is_head && ctx->header_info != 0 && ctx->insn_size == 4) {
        uint32_t head32 = linx_ldl_code(env, &ctx->base, ctx->base.pc_next);
        uint32_t next32 = linx_ldl_code(env, &ctx->base, ctx->base.pc_next + 4);

        /*
         * Kernel code also emits inner BSTART.STD/SYS markers as local
         * metadata inside an already-active outer block. Preserve the outer
         * block state for those non-atomic inner markers; leave real atomic
         * subblocks (identified by a following B.CATR) on the normal path.
         */
        if (head32 == 0x00001001 &&
            next32 != 0x40000023) {
            nested_explicit_head = true;
            is_head = false;
        }
    }

    if (!is_head) {
        if (!ctx->next_bpc_explicit &&
            get_blktype(ctx->header_info) != HEAD_TYPE_SIMT) {
            /*
             * Keep the architectural fall-through continuation aligned with
             * the current in-block instruction, not just the opening header.
             * Break/BUG traps can be taken from tiny one-instruction bodies;
             * when Linux elects to skip that instruction it resumes from
             * EBPCN, so EBPCN must already point at this instruction's
             * successor before the trap is raised.
             */
            tcg_gen_movi_tl(next_bpc, ctx->pc_succ_insn);
        }
        if (!is_valid_linx_addr(ctx->tpc1)) {
            ctx->tpc1 = ctx->base.pc_next;
            tcg_gen_movi_i64(tpc1, ctx->base.pc_next);
        }
        if (!ctx->in_body) {
            ctx->in_body = true;
            tcg_gen_movi_i32(in_body, true);
            if (ctx->header_info == 0) {
                uint64_t hi = 0;

                hi = set_field(hi, HEADER_INFO_BRHTYPE_MASK, BRANCH_FALL);
                hi = set_field(hi, HEADER_INFO_BLKTYPE_MASK, HEAD_TYPE_STD);
                hi = deposit64(hi, HEADER_INFO_SRCTYP_START,
                               HEADER_INFO_SRCTYP_LEN, 0x1f);
                ctx->header_info = hi;
                ctx->bpc = ctx->tpc1;
                ctx->blk_type = HEAD_TYPE_STD;
                ctx->brh_type = BRANCH_FALL;
                if (ctx->tpc1 == 0xffffffff80924724ULL ||
                    ctx->tpc1 == 0x80924724ULL) {
                    fprintf(stderr,
                            "LINX_SYNTH_BODY_4724 bpc=0x%llx hdr=0x%llx\n",
                            (unsigned long long)ctx->bpc,
                            (unsigned long long)ctx->header_info);
                }
                tcg_gen_movi_i64(header_info, hi);
                tcg_gen_movi_tl(bpc, ctx->bpc);
            }
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

    if (g_getenv("LINX_LOG_HEAD_TRACE") &&
        (ctx->base.pc_next == 0x10f6e ||
         ctx->base.pc_next == 0x10f80 ||
         ctx->base.pc_next == 0x10f88 ||
         ctx->base.pc_next == 0xffffffff80010f6eULL ||
         ctx->base.pc_next == 0xffffffff80010f80ULL ||
         ctx->base.pc_next == 0xffffffff80010f88ULL ||
         ctx->base.pc_next == 0xffffffff80445e8cULL ||
         ctx->base.pc_next == 0xffffffff80445e96ULL ||
         ctx->base.pc_next == 0xffffffff80445e9aULL)) {
        fprintf(stderr,
                "LINX_HEAD_TRACE pc=%#" PRIx64 " is_head=%d nested=%d size=%u hdr=%#" PRIx64
                " blk=%u br=%u in_body=%d next_explicit=%d next_target=%#" PRIx64 "\n",
                (uint64_t)ctx->base.pc_next, is_head, nested_explicit_head,
                ctx->insn_size, (uint64_t)ctx->header_info, ctx->blk_type,
                ctx->brh_type, ctx->in_body, ctx->next_bpc_explicit,
                (uint64_t)ctx->next_bpc_target);
    }

    trace_linx_mini(ctx->base.pc_next);
    if ((ctx->base.pc_next == 0x10f6e ||
         ctx->base.pc_next == 0x10f72 ||
         ctx->base.pc_next == 0x10f78 ||
         ctx->base.pc_next == 0x10f7c ||
         ctx->base.pc_next == 0x10f80 ||
         ctx->base.pc_next == 0x10f84 ||
         ctx->base.pc_next == 0x10f88 ||
         ctx->base.pc_next == 0x10f8c ||
         ctx->base.pc_next == 0xffffffff8001221aULL ||
         ctx->base.pc_next == 0xffffffff8001227cULL ||
         ctx->base.pc_next == 0xffffffff8001233aULL ||
         ctx->base.pc_next == 0xffffffff8001240cULL ||
         ctx->base.pc_next == 0xffffffff80012426ULL ||
         ctx->base.pc_next == 0xffffffff80011094ULL ||
         ctx->base.pc_next == 0xffffffff800110d8ULL ||
         ctx->base.pc_next == 0xffffffff8004b1d4ULL ||
         ctx->base.pc_next == 0xffffffff8004b360ULL ||
         ctx->base.pc_next == 0xffffffff800480ecULL ||
         ctx->base.pc_next == 0xffffffff80926accULL ||
         ctx->base.pc_next == 0xffffffff80926ed2ULL ||
         ctx->base.pc_next == 0xffffffff80926f16ULL ||
         ctx->base.pc_next == 0xffffffff8004cfbcULL ||
         ctx->base.pc_next == 0xffffffff8004cfbeULL ||
         ctx->base.pc_next == 0xffffffff8004cfc2ULL ||
         ctx->base.pc_next == 0xffffffff80010f6eULL ||
         ctx->base.pc_next == 0xffffffff80010f72ULL ||
         ctx->base.pc_next == 0xffffffff80010f78ULL ||
         ctx->base.pc_next == 0xffffffff80010f7cULL ||
         ctx->base.pc_next == 0xffffffff80010f80ULL ||
         ctx->base.pc_next == 0xffffffff80010f84ULL ||
         ctx->base.pc_next == 0xffffffff80010f88ULL ||
         ctx->base.pc_next == 0xffffffff80010f8cULL) &&
        g_getenv("LINX_LOG_TRACE_PC")) {
        gen_helper_trace_pc(cpu_env, tcg_constant_tl(ctx->base.pc_next));
    }
    ctx->pc_succ_insn = ctx->base.pc_next + ctx->insn_size;

    if (!is_head &&
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

    if (ctx->base.pc_next == 0xffffffff80a1ca6eULL ||
        ctx->base.pc_next == 0x80a1ca6eULL) {
        fprintf(stderr,
                "LINX_TPCA6E predecode pc=0x%llx insn_size=%d is_head=%d fused=%d hdr=0x%llx\n",
                (unsigned long long)ctx->base.pc_next, ctx->insn_size, is_head,
                fused_call_setret, (unsigned long long)ctx->header_info);
    }
    if (ctx->base.pc_next == 0xffffffff801d2208ULL ||
        ctx->base.pc_next == 0x801d2208ULL) {
        fprintf(stderr,
                "LINX_TPC2208 predecode pc=0x%llx insn_size=%d is_head=%d fused=%d hdr=0x%llx next_explicit=%d\n",
                (unsigned long long)ctx->base.pc_next, ctx->insn_size, is_head,
                fused_call_setret, (unsigned long long)ctx->header_info,
                ctx->next_bpc_explicit);
    }
    if (ctx->base.pc_next == 0xffffffff8092473aULL ||
        ctx->base.pc_next == 0x8092473aULL) {
        fprintf(stderr,
                "LINX_TPC473A predecode pc=0x%llx first=0x%llx insn_size=%d is_head=%d hdr=0x%llx in_body=%d tpc1=0x%llx\n",
                (unsigned long long)ctx->base.pc_next,
                (unsigned long long)ctx->base.pc_first, ctx->insn_size, is_head,
                (unsigned long long)ctx->header_info, ctx->in_body,
                (unsigned long long)ctx->tpc1);
    }

    if (nested_explicit_head) {
        /* Nested fall-through headers inside a CALL setup window are metadata. */
    } else if (fused_call_setret) {
        target_ulong bundle_pc = ctx->base.pc_next;
        uint64_t head48 = opcode64 & 0x0000ffffffffffffULL;
        uint16_t tail16 = opcode64 >> 48;

        ctx->insn_size = 6;
        ctx->pc_succ_insn = bundle_pc + 6;
        if (!decode_block48(ctx, head48)) {
            gen_exception_illegal(ctx);
        }

        ctx->base.pc_next = bundle_pc + 6;
        ctx->insn_size = 2;
        ctx->pc_succ_insn = bundle_pc + 8;
        if (!decode_block16(ctx, tail16)) {
            gen_exception_illegal(ctx);
        }
    } else if (ctx->insn_size == 2) {
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
        if (ctx->base.pc_next == 0xffffffff80a1ca6eULL ||
            ctx->base.pc_next == 0x80a1ca6eULL) {
            fprintf(stderr,
                    "LINX_TPCA6E decode48 pc=0x%llx opcode48=0x%012llx\n",
                    (unsigned long long)ctx->base.pc_next,
                    (unsigned long long)(opcode & 0x0000ffffffffffffULL));
        }
        if (!decode_block48(ctx, opcode)) {
            if (ctx->base.pc_next == 0xffffffff80a1ca6eULL ||
                ctx->base.pc_next == 0x80a1ca6eULL) {
                fprintf(stderr,
                        "LINX_TPCA6E decode48_fail pc=0x%llx opcode48=0x%012llx\n",
                        (unsigned long long)ctx->base.pc_next,
                        (unsigned long long)(opcode & 0x0000ffffffffffffULL));
            }
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
        if (linx_use_atomic_store_buf(ctx)) {
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
