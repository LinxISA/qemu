/*
 * LINX FPU Emulation Helpers for QEMU.
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

#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/host-utils.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"
#include "internals.h"
#include <math.h>

void helper_set_rounding_mode(CPULINXState *env, uint32_t rm)
{
    set_float_rounding_mode(rm, &env->fp_status);
}

static target_ulong fclass_b(uint64_t frs1)
{
    float8 f = frs1;
    bool sign = float8_is_neg(f);

    if (float8_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float8_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float8_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float8_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float8_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

static target_ulong fclass_lb(uint64_t frs1)
{
    float8 f = frs1;
    bool sign = float8_1_is_neg(f);

    if (float8_1_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float8_1_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float8_1_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float8_1_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float8_1_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

static target_ulong fclass_h(uint64_t frs1)
{
    float16 f = frs1;
    bool sign = float16_is_neg(f);

    if (float16_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float16_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float16_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float16_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float16_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

static target_ulong fclass_s(uint64_t frs1)
{
    float32 f = frs1;
    bool sign = float32_is_neg(f);

    if (float32_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float32_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float32_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float32_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float32_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

static target_ulong fclass_d(uint64_t frs1)
{
    float64 f = frs1;
    bool sign = float64_is_neg(f);

    if (float64_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float64_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float64_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float64_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float64_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

static target_ulong fclass_bf(uint64_t frs1)
{
    float16 f = frs1;
    bool sign = bfloat16_is_neg(f);

    if (bfloat16_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (bfloat16_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (bfloat16_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (bfloat16_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return bfloat16_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

#define CONV_FP_TO_INT(name_int, name_fp, fsz, itype)                          \
target_ulong helper_fcvt_##name_int##_##name_fp(CPULINXState *env,             \
                                                target_ulong x)                \
{                                                                              \
    if (unlikely(float##fsz##_is_any_nan(x))) {                                \
        float_raise(float_flag_invalid, &env->fp_status);                      \
        return 0;                                                              \
    }                                                                          \
    return float##fsz##_to_##itype##_scalbn(x,                                 \
           env->fp_status.float_rounding_mode, 0, &env->fp_status);            \
}

CONV_FP_TO_INT(ud, d, 64, uint64)
CONV_FP_TO_INT(uw, d, 64, uint32)
CONV_FP_TO_INT(uh, d, 64, uint16)
CONV_FP_TO_INT(ub, d, 64, uint8)
CONV_FP_TO_INT(ud, s, 32, uint64)
CONV_FP_TO_INT(uw, s, 32, uint32)
CONV_FP_TO_INT(uh, s, 32, uint16)
CONV_FP_TO_INT(ub, s, 32, uint8)
CONV_FP_TO_INT(ud, h, 16, uint64)
CONV_FP_TO_INT(uw, h, 16, uint32)
CONV_FP_TO_INT(uh, h, 16, uint16)
CONV_FP_TO_INT(ub, h, 16, uint8)
CONV_FP_TO_INT(ud, b, 8, uint64)
CONV_FP_TO_INT(uw, b, 8, uint32)
CONV_FP_TO_INT(uh, b, 8, uint16)
CONV_FP_TO_INT(ub, b, 8, uint8)
CONV_FP_TO_INT(ud, bf, 16, int64)
CONV_FP_TO_INT(uw, bf, 16, int32)
CONV_FP_TO_INT(uh, bf, 16, int16)
CONV_FP_TO_INT(ub, bf, 16, int8)
CONV_FP_TO_INT(ud, lb, 8_1, uint64)
CONV_FP_TO_INT(uw, lb, 8_1, uint32)
CONV_FP_TO_INT(uh, lb, 8_1, uint16)
CONV_FP_TO_INT(ub, lb, 8_1, uint8)

CONV_FP_TO_INT(sd, d, 64, int64)
CONV_FP_TO_INT(sw, d, 64, int32)
CONV_FP_TO_INT(sh, d, 64, int16)
CONV_FP_TO_INT(sb, d, 64, int8)
CONV_FP_TO_INT(sd, s, 32, int64)
CONV_FP_TO_INT(sw, s, 32, int32)
CONV_FP_TO_INT(sh, s, 32, int16)
CONV_FP_TO_INT(sb, s, 32, int8)
CONV_FP_TO_INT(sd, h, 16, int64)
CONV_FP_TO_INT(sw, h, 16, int32)
CONV_FP_TO_INT(sh, h, 16, int16)
CONV_FP_TO_INT(sb, h, 16, int8)
CONV_FP_TO_INT(sd, b, 8, int64)
CONV_FP_TO_INT(sw, b, 8, int32)
CONV_FP_TO_INT(sh, b, 8, int16)
CONV_FP_TO_INT(sb, b, 8, int8)
CONV_FP_TO_INT(sd, bf, 16, uint64)
CONV_FP_TO_INT(sw, bf, 16, uint32)
CONV_FP_TO_INT(sh, bf, 16, uint16)
CONV_FP_TO_INT(sb, bf, 16, uint8)
CONV_FP_TO_INT(sd, lb, 8_1, int64)
CONV_FP_TO_INT(sw, lb, 8_1, int32)
CONV_FP_TO_INT(sh, lb, 8_1, int16)
CONV_FP_TO_INT(sb, lb, 8_1, int8)

#undef CONV_FP_TO_INT

static uint64_t do_fmadd_b(CPULINXState *env, uint64_t rs1, uint64_t rs2,
                           uint64_t rs3, int flags)
{
    return float8_muladd(rs1, rs2, rs3, flags, &env->fp_status);
}

static uint64_t do_fmadd_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2,
                           uint64_t rs3, int flags)
{
    return float8_1_muladd(rs1, rs2, rs3, flags, &env->fp_status);
}

static uint64_t do_fmadd_h(CPULINXState *env, uint64_t rs1, uint64_t rs2,
                           uint64_t rs3, int flags)
{
    return float16_muladd(rs1, rs2, rs3, flags, &env->fp_status);
}

static uint64_t do_fmadd_s(CPULINXState *env, uint64_t rs1, uint64_t rs2,
                           uint64_t rs3, int flags)
{
    return float32_muladd(rs1, rs2, rs3, flags, &env->fp_status);
}

static uint64_t do_fmadd_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2,
                           uint64_t rs3, int flags)
{
    return bfloat16_muladd(rs1, rs2, rs3, flags, &env->fp_status);
}

uint64_t helper_fmadd_b(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_b(env, frs1, frs2, frs3, 0);
}

uint64_t helper_fmadd_lb(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_lb(env, frs1, frs2, frs3, 0);
}

uint64_t helper_fmadd_h(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_h(env, frs1, frs2, frs3, 0);
}

uint64_t helper_fmadd_s(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_s(env, frs1, frs2, frs3, 0);
}

uint64_t helper_fmadd_d(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return float64_muladd(frs1, frs2, frs3, 0, &env->fp_status);
}

uint64_t helper_fmadd_bf(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_bf(env, frs1, frs2, frs3, 0);
}

uint64_t helper_fmsub_b(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_b(env, frs1, frs2, frs3, float_muladd_negate_c);
}

uint64_t helper_fmsub_lb(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_lb(env, frs1, frs2, frs3, float_muladd_negate_c);
}

uint64_t helper_fmsub_h(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_h(env, frs1, frs2, frs3, float_muladd_negate_c);
}

uint64_t helper_fmsub_s(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_s(env, frs1, frs2, frs3, float_muladd_negate_c);
}

uint64_t helper_fmsub_d(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return float64_muladd(frs1, frs2, frs3, float_muladd_negate_c,
                          &env->fp_status);
}

uint64_t helper_fmsub_bf(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                        uint64_t frs3)
{
    return do_fmadd_bf(env, frs1, frs2, frs3, float_muladd_negate_c);
}

uint64_t helper_fnmsub_b(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_b(env, frs1, frs2, frs3, float_muladd_negate_product);
}

uint64_t helper_fnmsub_lb(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_lb(env, frs1, frs2, frs3, float_muladd_negate_product);
}

uint64_t helper_fnmsub_h(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_h(env, frs1, frs2, frs3, float_muladd_negate_product);
}

uint64_t helper_fnmsub_s(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_s(env, frs1, frs2, frs3, float_muladd_negate_product);
}

uint64_t helper_fnmsub_d(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return float64_muladd(frs1, frs2, frs3, float_muladd_negate_product,
                          &env->fp_status);
}

uint64_t helper_fnmsub_bf(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_s(env, frs1, frs2, frs3, float_muladd_negate_product);
}

uint64_t helper_fnmadd_b(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_b(env, frs1, frs2, frs3,
                      float_muladd_negate_c | float_muladd_negate_product);
}

uint64_t helper_fnmadd_lb(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_lb(env, frs1, frs2, frs3,
                      float_muladd_negate_c | float_muladd_negate_product);
}

uint64_t helper_fnmadd_h(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_h(env, frs1, frs2, frs3,
                      float_muladd_negate_c | float_muladd_negate_product);
}

uint64_t helper_fnmadd_s(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_s(env, frs1, frs2, frs3,
                      float_muladd_negate_c | float_muladd_negate_product);
}

uint64_t helper_fnmadd_d(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return float64_muladd(frs1, frs2, frs3, float_muladd_negate_c |
                          float_muladd_negate_product, &env->fp_status);
}

uint64_t helper_fnmadd_bf(CPULINXState *env, uint64_t frs1, uint64_t frs2,
                         uint64_t frs3)
{
    return do_fmadd_bf(env, frs1, frs2, frs3,
                      float_muladd_negate_c | float_muladd_negate_product);
}

uint64_t helper_fadd_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_add(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsub_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_sub(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmul_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_mul(rs1, rs2, &env->fp_status);
}

uint64_t helper_fdiv_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_div(rs1, rs2, &env->fp_status);
}

target_ulong helper_feq_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_eq_quiet(rs1, rs2, &env->fp_status);
}

target_ulong helper_flt_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_lt(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsqrt_b(CPULINXState *env, uint64_t rs1)
{
    return float8_sqrt(rs1, &env->fp_status);
}

target_ulong helper_fclass_b(uint64_t rs1)
{
    return fclass_b(rs1);
}

uint64_t helper_fmax_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_maximum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmin_b(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_minimum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fadd_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_add(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsub_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_sub(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmul_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_mul(rs1, rs2, &env->fp_status);
}

uint64_t helper_fdiv_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_div(rs1, rs2, &env->fp_status);
}

target_ulong helper_feq_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_compare_quiet(rs1, rs2, &env->fp_status);
}

target_ulong helper_flt_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_lt(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsqrt_lb(CPULINXState *env, uint64_t rs1)
{
    return float8_1_sqrt(rs1, &env->fp_status);
}

uint64_t helper_fexp_b(CPULINXState *env, uint64_t rs1)
{
    float f;
    uint64_t result = helper_fcvt_s_b(env, rs1);
    memcpy(&f, &result, sizeof(f));

    float res = exp(f);
    memcpy(&result, &res, sizeof(res));
    result = helper_fcvt_b_s(env, result);
    return result;
}

target_ulong helper_fclass_lb(uint64_t rs1)
{
    return fclass_lb(rs1);
}

uint64_t helper_fmax_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_maximum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmin_lb(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float8_1_minimum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fadd_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_add(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsub_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_sub(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmul_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_mul(rs1, rs2, &env->fp_status);
}

uint64_t helper_fdiv_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_div(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmin_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_minimum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmax_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_maximum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsqrt_h(CPULINXState *env, uint64_t rs1)
{
    return float16_sqrt(rs1, &env->fp_status);
}

uint64_t helper_fexp_h(CPULINXState *env, uint64_t rs1)
{
    float f;
    uint64_t result = helper_fcvt_s_h(env, rs1);
    memcpy(&f, &result, sizeof(f));

    float res = exp(f);
    memcpy(&result, &res, sizeof(res));
    result = helper_fcvt_h_s(env, result);
    return result;
}

uint64_t helper_fexp_bf(CPULINXState *env, uint64_t rs1)
{
    float f;
    uint64_t result = helper_fcvt_s_bf(env, rs1);
    memcpy(&f, &result, sizeof(f));

    float res = exp(f);
    memcpy(&result, &res, sizeof(res));
    result = helper_fcvt_bf_s(env, result);
    return result;
}

target_ulong helper_fle_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_le(rs1, rs2, &env->fp_status);
}

target_ulong helper_flt_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_lt(rs1, rs2, &env->fp_status);
}

target_ulong helper_feq_h(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float16_eq_quiet(rs1, rs2, &env->fp_status);
}

target_ulong helper_fclass_h(uint64_t rs1)
{
    return fclass_h(rs1);
}

uint64_t helper_fadd_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_add(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsub_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_sub(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmul_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_mul(rs1, rs2, &env->fp_status);
}

uint64_t helper_fdiv_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_div(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmin_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_minimum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmax_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_maximum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsqrt_s(CPULINXState *env, uint64_t rs1)
{
    return float32_sqrt(rs1, &env->fp_status);
}

uint64_t helper_fexp_s(CPULINXState *env, uint64_t rs1)
{
    float f;
    uint64_t result;
    memcpy(&f, &rs1, sizeof(f));

    float res = exp(f);

    memcpy(&result, &res, sizeof(res));
    return result;
}

target_ulong helper_fle_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_le(rs1, rs2, &env->fp_status);
}

target_ulong helper_flt_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_lt(rs1, rs2, &env->fp_status);
}

target_ulong helper_feq_s(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return float32_eq_quiet(rs1, rs2, &env->fp_status);
}

target_ulong helper_fclass_s(uint64_t rs1)
{
    return fclass_s(rs1);
}

uint64_t helper_fadd_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_add(frs1, frs2, &env->fp_status);
}

uint64_t helper_fsub_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_sub(frs1, frs2, &env->fp_status);
}

uint64_t helper_fmul_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_mul(frs1, frs2, &env->fp_status);
}

uint64_t helper_fdiv_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_div(frs1, frs2, &env->fp_status);
}

uint64_t helper_fmin_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_minimum_number(frs1, frs2, &env->fp_status);
}

uint64_t helper_fmax_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_maximum_number(frs1, frs2, &env->fp_status);
}

uint64_t helper_fsqrt_d(CPULINXState *env, uint64_t frs1)
{
    return float64_sqrt(frs1, &env->fp_status);
}

uint64_t helper_fexp_d(CPULINXState *env, uint64_t frs1)
{
    double *ptr_b = (double *)&frs1;
    double res = exp(*ptr_b);
    uint64_t *ptr_u = (uint64_t *)&res;
    return *ptr_u;
}

target_ulong helper_fle_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_le(frs1, frs2, &env->fp_status);
}

target_ulong helper_flt_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_lt(frs1, frs2, &env->fp_status);
}

target_ulong helper_feq_d(CPULINXState *env, uint64_t frs1, uint64_t frs2)
{
    return float64_eq_quiet(frs1, frs2, &env->fp_status);
}

target_ulong helper_fclass_d(uint64_t frs1)
{
    return fclass_d(frs1);
}

uint64_t helper_fadd_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_add(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsub_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_sub(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmul_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_mul(rs1, rs2, &env->fp_status);
}

uint64_t helper_fdiv_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_div(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmin_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_minimum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fmax_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_maximum_number(rs1, rs2, &env->fp_status);
}

uint64_t helper_fsqrt_bf(CPULINXState *env, uint64_t rs1)
{
    return bfloat16_sqrt(rs1, &env->fp_status);
}

target_ulong helper_fle_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_le(rs1, rs2, &env->fp_status);
}

target_ulong helper_flt_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_lt(rs1, rs2, &env->fp_status);
}

target_ulong helper_feq_bf(CPULINXState *env, uint64_t rs1, uint64_t rs2)
{
    return bfloat16_eq_quiet(rs1, rs2, &env->fp_status);
}

target_ulong helper_fclass_bf(uint64_t rs1)
{
    return fclass_bf(rs1);
}

uint64_t helper_fcvt_s_sb(CPULINXState *env, target_ulong rs1)
{
    return int8_to_float32((int8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_ub(CPULINXState *env, target_ulong rs1)
{
    return uint8_to_float32((uint8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_sh(CPULINXState *env, target_ulong rs1)
{
    return int16_to_float32((int16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_uh(CPULINXState *env, target_ulong rs1)
{
    return uint16_to_float32((uint16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_sw(CPULINXState *env, target_ulong rs1)
{
    return int32_to_float32((int32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_uw(CPULINXState *env, target_ulong rs1)
{
    return uint32_to_float32((uint32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_sd(CPULINXState *env, target_ulong rs1)
{
    return int64_to_float32(rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_ud(CPULINXState *env, target_ulong rs1)
{
    return uint64_to_float32(rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_b(CPULINXState *env, uint64_t rs1)
{
    return float8_to_float32(rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_lb(CPULINXState *env, uint64_t rs1)
{
    return float8_1_to_float32(rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_h(CPULINXState *env, uint64_t rs1)
{
    return float16_to_float32(rs1, true, &env->fp_status);
}

uint64_t helper_fcvt_s_bf(CPULINXState *env, uint64_t rs1)
{
    return bfloat16_to_float32(rs1, &env->fp_status);
}

uint64_t helper_fcvt_s_d(CPULINXState *env, uint64_t rs1)
{
    return float64_to_float32(rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_sb(CPULINXState *env, target_ulong rs1)
{
    return int8_to_float64((int8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_ub(CPULINXState *env, target_ulong rs1)
{
    return uint8_to_float64((uint8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_sh(CPULINXState *env, target_ulong rs1)
{
    return int16_to_float64((int16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_uh(CPULINXState *env, target_ulong rs1)
{
    return uint16_to_float64((uint16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_sw(CPULINXState *env, target_ulong rs1)
{
    return int32_to_float64((int32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_uw(CPULINXState *env, target_ulong rs1)
{
    return uint32_to_float64((uint32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_sd(CPULINXState *env, target_ulong rs1)
{
    return int64_to_float64(rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_ud(CPULINXState *env, target_ulong rs1)
{
    return uint64_to_float64(rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_b(CPULINXState *env, uint64_t rs1)
{
    return float8_to_float64(rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_lb(CPULINXState *env, uint64_t rs1)
{
    return float8_1_to_float64(rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_h(CPULINXState *env, uint64_t rs1)
{
    return float16_to_float64(rs1, true, &env->fp_status);
}

uint64_t helper_fcvt_d_s(CPULINXState *env, uint64_t rs1)
{
    return float32_to_float64(rs1, &env->fp_status);
}

uint64_t helper_fcvt_d_bf(CPULINXState *env, uint64_t rs1)
{
    return bfloat16_to_float64(rs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_b(CPULINXState *env, uint64_t frs1)
{
    return float8_to_bfloat16(frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_lb(CPULINXState *env, uint64_t frs1)
{
    return float8_1_to_bfloat16(frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_h(CPULINXState *env, uint64_t frs1)
{
    return float16_to_bfloat16(frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_s(CPULINXState *env, uint64_t frs1)
{
    return float32_to_bfloat16(frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_d(CPULINXState *env, uint64_t frs1)
{
    return float64_to_bfloat16(frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_sb(CPULINXState *env, uint64_t frs1)
{
    return int8_to_bfloat16((int8_t)frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_ub(CPULINXState *env, uint64_t frs1)
{
    return uint8_to_bfloat16((uint8_t)frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_sh(CPULINXState *env, uint64_t frs1)
{
    return int16_to_bfloat16((int16_t)frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_uh(CPULINXState *env, uint64_t frs1)
{
    return uint16_to_bfloat16((uint16_t)frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_sw(CPULINXState *env, uint64_t frs1)
{
    return int32_to_bfloat16((int32_t)frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_uw(CPULINXState *env, uint64_t frs1)
{
    return uint32_to_bfloat16((uint32_t)frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_sd(CPULINXState *env, uint64_t frs1)
{
    return int64_to_bfloat16(frs1, &env->fp_status);
}

target_ulong helper_fcvt_bf_ud(CPULINXState *env, uint64_t frs1)
{
    return uint64_to_bfloat16(frs1, &env->fp_status);
}

uint64_t helper_fcvt_h_sb(CPULINXState *env, target_ulong rs1)
{
    return int8_to_float16((int8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_ub(CPULINXState *env, target_ulong rs1)
{
    return uint8_to_float16((uint8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_sh(CPULINXState *env, target_ulong rs1)
{
    return int16_to_float16((int16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_uh(CPULINXState *env, target_ulong rs1)
{
    return uint16_to_float16((uint16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_sw(CPULINXState *env, target_ulong rs1)
{
    return int32_to_float16((int32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_uw(CPULINXState *env, target_ulong rs1)
{
    return uint32_to_float16((uint32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_sd(CPULINXState *env, target_ulong rs1)
{
    return int64_to_float16(rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_ud(CPULINXState *env, target_ulong rs1)
{
    return uint64_to_float16(rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_b(CPULINXState *env, uint64_t rs1)
{
    return float8_to_float16(rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_lb(CPULINXState *env, uint64_t rs1)
{
    return float8_1_to_float16(rs1, &env->fp_status);
}

uint64_t helper_fcvt_h_s(CPULINXState *env, uint64_t rs1)
{
    return float32_to_float16(rs1, true, &env->fp_status);
}

uint64_t helper_fcvt_h_d(CPULINXState *env, uint64_t rs1)
{
    return float64_to_float16(rs1, true, &env->fp_status);
}

uint64_t helper_fcvt_h_bf(CPULINXState *env, uint64_t rs1)
{
    return bfloat16_to_float16(rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_sb(CPULINXState *env, target_ulong rs1)
{
    return int8_to_float8((int8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_ub(CPULINXState *env, target_ulong rs1)
{
    return uint8_to_float8((uint8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_sh(CPULINXState *env, target_ulong rs1)
{
    return int16_to_float8((int16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_uh(CPULINXState *env, target_ulong rs1)
{
    return uint16_to_float8((uint16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_sw(CPULINXState *env, target_ulong rs1)
{
    return int32_to_float8((int32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_uw(CPULINXState *env, target_ulong rs1)
{
    return uint32_to_float8((uint32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_sd(CPULINXState *env, target_ulong rs1)
{
    return int64_to_float8(rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_ud(CPULINXState *env, target_ulong rs1)
{
    return uint64_to_float8(rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_lb(CPULINXState *env, uint64_t rs1)
{
    return float8_1_to_float8(rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_h(CPULINXState *env, uint64_t rs1)
{
    return float16_to_float8(rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_s(CPULINXState *env, uint64_t rs1)
{
    return float32_to_float8(rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_d(CPULINXState *env, uint64_t rs1)
{
    return float64_to_float8(rs1, &env->fp_status);
}

uint64_t helper_fcvt_b_bf(CPULINXState *env, uint64_t rs1)
{
    return bfloat16_to_float8(rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_sb(CPULINXState *env, target_ulong rs1)
{
    return int8_to_float8_1((int8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_ub(CPULINXState *env, target_ulong rs1)
{
    return uint8_to_float8_1((uint8_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_sh(CPULINXState *env, target_ulong rs1)
{
    return int16_to_float8_1((int16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_uh(CPULINXState *env, target_ulong rs1)
{
    return uint16_to_float8_1((uint16_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_sw(CPULINXState *env, target_ulong rs1)
{
    return int32_to_float8_1((int32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_uw(CPULINXState *env, target_ulong rs1)
{
    return uint32_to_float8_1((uint32_t)rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_sd(CPULINXState *env, target_ulong rs1)
{
    return int64_to_float8_1(rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_ud(CPULINXState *env, target_ulong rs1)
{
    return uint64_to_float8_1(rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_b(CPULINXState *env, uint64_t rs1)
{
    return float8_to_float8_1(rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_h(CPULINXState *env, uint64_t rs1)
{
    return float16_to_float8_1(rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_s(CPULINXState *env, uint64_t rs1)
{
    return float32_to_float8_1(rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_d(CPULINXState *env, uint64_t rs1)
{
    return float64_to_float8_1(rs1, &env->fp_status);
}

uint64_t helper_fcvt_lb_bf(CPULINXState *env, uint64_t rs1)
{
    return bfloat16_to_float8_1(rs1, &env->fp_status);
}

uint64_t helper_flog_s(CPULINXState *env, uint64_t rs1)
{
    return float32_log2(rs1, &env->fp_status);
}

uint64_t helper_flog_d(CPULINXState *env, uint64_t rs1)
{
    return float64_log2(rs1, &env->fp_status);
}


#define HELPER_FLOAT_XN(instr, func)                                \
uint64_t helper_##instr##_xn(CPULINXState *env, uint64_t srcl,      \
    uint64_t srcr, uint64_t vlen, uint64_t sat, uint64_t rm)        \
{                                                                   \
    helper_set_rounding_mode(env, rm);                              \
    vlen = pow(2, vlen);                                            \
    uint64_t res = 0;                                               \
    for (int i = 0; i < vlen; ++i) {                                \
        uint16_t rs1 = srcl >> (i * 16) & 0xFFFF;                   \
        uint16_t rs2 = srcr >> (i * 16) & 0xFFFF;                   \
        res |= helper_##func(env, rs1, rs2) << (i * 16);          \
    }                                                               \
    helper_set_rounding_mode(env, float_round_nearest_even);        \
    return res;                                                     \
}

uint64_t helper_fadd_xn(CPULINXState *env, uint64_t srcl,
    uint64_t srcr, uint64_t vlen, uint64_t sat, uint64_t rm)
{
    assert(sat == 0);
    helper_set_rounding_mode(env, rm);
    vlen = pow(2, vlen);
    uint64_t res = 0;
    for (int i = 0; i < vlen; ++i) {
        uint16_t rs1 = srcl >> (i * 16) & 0xFFFF;
        uint16_t rs2 = srcr >> (i * 16) & 0xFFFF;
        res |= helper_fadd_bf(env, rs1, rs2) << (i * 16);
    }
    helper_set_rounding_mode(env, float_round_nearest_even);
    return res;
}

HELPER_FLOAT_XN(fsub, fsub_bf)
HELPER_FLOAT_XN(fmul, fmul_bf)
HELPER_FLOAT_XN(fdiv, fdiv_bf)


uint64_t helper_fmax_xn(CPULINXState *env, uint64_t srcl,
    uint64_t srcr, uint64_t vlen)
{
    vlen = pow(2, vlen);
    uint64_t res = 0;
    for (int i = 0; i < vlen; ++i) {
        uint16_t rs1 = srcl >> (i * 16) & 0xFFFF;
        uint16_t rs2 = srcr >> (i * 16) & 0xFFFF;
        res |= helper_fmax_bf(env, rs1, rs2) << (i * 16);
    }
    return res;
}

uint64_t helper_fmin_xn(CPULINXState *env, uint64_t srcl,
    uint64_t srcr, uint64_t vlen)
{
    vlen = pow(2, vlen);
    uint64_t res = 0;
    for (int i = 0; i < vlen; ++i) {
        uint16_t rs1 = srcl >> (i * 16) & 0xFFFF;
        uint16_t rs2 = srcr >> (i * 16) & 0xFFFF;
        res |= helper_fmin_bf(env, rs1, rs2) << (i * 16);
    }
    return res;
}

uint64_t helper_fexp_xn(CPULINXState *env, uint64_t srcl,
    uint64_t vlen, uint64_t sat, uint64_t rm)
{
    assert(sat == 0);
    helper_set_rounding_mode(env, rm);
    vlen = pow(2, vlen);
    uint64_t res = 0;
    for (int i = 0; i < vlen; ++i) {
        uint16_t rs1 = srcl >> (i * 16) & 0xFFFF;
        res |= helper_fexp_bf(env, rs1) << (i * 16);
    }
    helper_set_rounding_mode(env, float_round_nearest_even);
    return res;
}
