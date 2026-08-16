/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "target/linx/cpu.h"
#include "target/linx/tile_cube_058.h"
#include "target/linx/tile_numeric_058.h"
#include <fenv.h>

static unsigned dtype_bytes(unsigned dtype)
{
    switch (dtype) {
    case 0:
    case 16:
    case 24:
        return 8;
    case 1:
    case 2:
    case 3:
    case 17:
    case 25:
        return 4;
    case 4:
    case 5:
    case 18:
    case 26:
        return 2;
    default:
        return 1;
    }
}

static void set_tile(CPULinxState *env, unsigned tile, unsigned dtype,
                     unsigned rows, unsigned cols, const void *data,
                     unsigned bytes)
{
    memset(env->tile_reg[tile], 0, sizeof(env->tile_reg[tile]));
    memcpy(env->tile_reg[tile], data, bytes);
    env->tile_reg_capacity[tile] = MAX(bytes, 16u);
    env->tile_reg_bytes[tile] = MAX(bytes, 4u);
    env->tile_reg_elem_bytes[tile] = dtype_bytes(dtype);
    env->tile_reg_dtype[tile] = dtype;
    env->tile_reg_valid_cols[tile] = cols;
    env->tile_reg_valid_rows[tile] = rows;
    env->tile_reg_cols[tile] = cols;
    env->tile_reg_rows[tile] = rows;
}

static void set_fp64_acc(CPULinxState *env, const double *values, unsigned rows,
                         unsigned cols)
{
    unsigned bytes = rows * cols * sizeof(double);
    memset(env->tile_acc, 0, sizeof(env->tile_acc));
    memcpy(env->tile_acc, values, bytes);
    env->tile_acc_bytes = MAX(bytes, 16u);
    env->tile_acc_dtype = LINX_TILE_ACC_FP64;
    env->tile_acc_valid = 1;
    env->tile_acc_rows = rows;
    env->tile_acc_cols = cols;
}

static uint64_t run_accumulator_convert(CPULinxState *env, unsigned dtype, double value,
                           unsigned rmode, bool sat)
{
    uint64_t raw = 0;
    set_fp64_acc(env, &value, 1, 1);
    env->tile_dtype = dtype;
    env->tile_attr_raw = (rmode << 25) | ((unsigned)sat << 28);
    env->tile_reg_capacity[31] = 16;
    g_assert_true(linx_tile_accumulator_convert(env, 31, 0));
    memcpy(&raw, env->tile_reg[31], dtype_bytes(dtype));
    g_assert_cmpuint(env->tile_reg_valid_cols[31], ==, 1);
    g_assert_cmpuint(env->tile_reg_valid_rows[31], ==, 1);
    g_assert_cmpuint(env->tile_reg_cols[31], ==, 1);
    g_assert_cmpuint(env->tile_reg_rows[31], ==, 1);
    return raw;
}

static uint64_t raw_one(unsigned dtype)
{
    switch (dtype) {
    case 0:
        return UINT64_C(0x3ff0000000000000);
    case 1:
    case 2:
    case 3:
        return 0x3f800000;
    case 4:
        return 0x3c00;
    case 5:
        return 0x3f80;
    case 6:
        return 0x08;
    case 7:
        return 0x38;
    case 8:
        return 0x3c;
    case 9:
        return 0x0c;
    case 10:
        return 0x08;
    case 11:
        return 0x02;
    case 12:
    case 14:
        return 0x04;
    default:
        return 1;
    }
}

static void test_all_production_type_rows(void)
{
    static const unsigned ordinary[] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
        12, 14, 16, 17, 18, 19, 20, 24, 25, 26, 27, 28,
    };
    static const unsigned mx[][2] = {
        {7, 7}, {7, 8}, {8, 7}, {8, 8}, {11, 11}, {11, 14}, {14, 11}, {14, 14},
    };
    CPULinxState *env = g_new0(CPULinxState, 1);
    uint8_t scale = 0x7f;

    env->lb[0] = env->lb[1] = env->lb[2] = 1;
    for (unsigned i = 0; i < G_N_ELEMENTS(ordinary); i++) {
        uint64_t one = raw_one(ordinary[i]);
        env->tile_dtype = ordinary[i];
        set_tile(env, 0, ordinary[i], 1, 1, &one, dtype_bytes(ordinary[i]));
        set_tile(env, 1, ordinary[i], 1, 1, &one, dtype_bytes(ordinary[i]));
        g_assert_true(linx_tile_cube_compute_058(env, 0, 1, 0, 0, 0, 0, false,
                                                 false, false));
        if (env->tile_acc_dtype == LINX_TILE_ACC_FP32) {
            float result;
            memcpy(&result, env->tile_acc, sizeof(result));
            g_assert_cmpfloat(result, ==, 1.0);
        } else if (env->tile_acc_dtype == LINX_TILE_ACC_FP64) {
            double result;
            memcpy(&result, env->tile_acc, sizeof(result));
            g_assert_cmpfloat(result, ==, 1.0);
        } else {
            uint64_t result;
            memcpy(&result, env->tile_acc, sizeof(result));
            g_assert_cmpuint(result, ==, 1);
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(mx); i++) {
        uint64_t left = raw_one(mx[i][0]), right = raw_one(mx[i][1]);
        float result;
        env->tile_dtype = 1;
        set_tile(env, 0, mx[i][0], 1, 1, &left, 1);
        set_tile(env, 1, mx[i][1], 1, 1, &right, 1);
        set_tile(env, 2, 13, 1, 1, &scale, 1);
        set_tile(env, 3, 13, 1, 1, &scale, 1);
        g_assert_true(linx_tile_cube_compute_058(env, 0, 1, 2, 3, 0, 0, true,
                                                 false, false));
        memcpy(&result, env->tile_acc, sizeof(result));
        g_assert_cmpfloat(result, ==, 1.0);
    }
    env->tile_dtype = 15;
    g_assert_false(
        linx_tile_cube_compute_058(env, 0, 1, 0, 0, 0, 0, false, false, false));
    g_free(env);
}

static void test_ordinary_bias_and_acc(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    float left[] = {1, 2, 3, 4};
    float right[] = {5, 6, 7, 8};
    float bias[] = {1, -2};
    float result[4];

    env->lb[0] = env->lb[1] = env->lb[2] = 2;
    env->tile_dtype = 1;
    set_tile(env, 0, 1, 2, 2, left, sizeof(left));
    set_tile(env, 1, 1, 2, 2, right, sizeof(right));
    set_tile(env, 2, 1, 1, 2, bias, sizeof(bias));
    g_assert_true(
        linx_tile_cube_compute_058(env, 0, 1, 0, 0, 2, 2, false, true, false));
    memcpy(result, env->tile_acc, sizeof(result));
    g_assert_cmpfloat(result[0], ==, 20);
    g_assert_cmpfloat(result[1], ==, 20);
    g_assert_cmpfloat(result[2], ==, 44);
    g_assert_cmpfloat(result[3], ==, 48);
    g_assert_cmpuint(env->tile_acc_dtype, ==, LINX_TILE_ACC_FP32);
    g_assert_cmpuint(env->tile_acc_rows, ==, 2);
    g_assert_cmpuint(env->tile_acc_cols, ==, 2);

    g_assert_true(
        linx_tile_cube_compute_058(env, 0, 1, 0, 0, 0, 2, false, false, true));
    memcpy(result, env->tile_acc, sizeof(result));
    g_assert_cmpfloat(result[0], ==, 39);
    g_assert_cmpfloat(result[1], ==, 42);
    g_assert_cmpfloat(result[2], ==, 87);
    g_assert_cmpfloat(result[3], ==, 98);
    g_free(env);
}

static void test_upper_physical_slots(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    float left = 2.0f;
    float right = 3.0f;
    float result;

    env->lb[0] = env->lb[1] = env->lb[2] = 1;
    env->tile_dtype = 1;
    set_tile(env, 32, 1, 1, 1, &left, sizeof(left));
    set_tile(env, 47, 1, 1, 1, &right, sizeof(right));
    g_assert_true(linx_tile_cube_compute_058(env, 32, 47, 0, 0, 0, 0,
                                             false, false, false));
    memcpy(&result, env->tile_acc, sizeof(result));
    g_assert_cmpfloat(result, ==, 6.0f);

    env->tile_reg_capacity[48] = 16;
    g_assert_true(linx_tile_accumulator_convert(env, 48, 0));
    memcpy(&result, env->tile_reg[48], sizeof(result));
    g_assert_cmpfloat(result, ==, 6.0f);

    env->tile_reg_capacity[63] = 16;
    g_assert_true(linx_tile_accumulator_convert(env, 63, 0));
    memcpy(&result, env->tile_reg[63], sizeof(result));
    g_assert_cmpfloat(result, ==, 6.0f);
    g_free(env);
}

static void test_non_square_loop_bound_mapping(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    uint8_t left[2 * 64], right[64 * 4];

    memset(left, 1, sizeof(left));
    memset(right, 1, sizeof(right));
    env->lb[0] = 4;
    env->lb[1] = 2;
    env->lb[2] = 64;
    env->tile_dtype = 27;
    set_tile(env, 0, 27, 2, 64, left, sizeof(left));
    set_tile(env, 1, 27, 64, 4, right, sizeof(right));

    LinxTileCubeDimensions dims = linx_tile_cube_dimensions_058(env);
    g_assert_cmpuint(dims.m, ==, 2);
    g_assert_cmpuint(dims.n, ==, 4);
    g_assert_cmpuint(dims.k, ==, 64);
    g_assert_true(linx_tile_cube_primary_legal_058(env, 0, 1, false, false));

    env->lb[0] = 2;
    env->lb[1] = 4;
    g_assert_false(linx_tile_cube_primary_legal_058(env, 0, 1, false, false));
    g_free(env);
}

static void test_group_profile_dimension_contract(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);

    env->lb[0] = 32;
    env->lb[1] = 32;
    env->lb[2] = 32;
    g_assert_true(linx_tile_cube_group_dimensions_legal_058(env));

    env->lb[1] = 8;
    g_assert_false(linx_tile_cube_group_dimensions_legal_058(env));
    g_free(env);
}

static void test_mx_k64(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    uint8_t left[64], right[64], scale_a[] = {0x7f, 0x80};
    uint8_t scale_b[] = {0x7f, 0x81};
    float bias = 2, result, prior = 3;

    memset(left, 0x38, sizeof(left));
    memset(right, 0x3c, sizeof(right));
    env->lb[0] = 1;
    env->lb[1] = 1;
    env->lb[2] = 64;
    env->tile_dtype = 1;
    set_tile(env, 0, 7, 1, 64, left, sizeof(left));
    set_tile(env, 1, 8, 64, 1, right, sizeof(right));
    set_tile(env, 2, 13, 1, 2, scale_a, sizeof(scale_a));
    set_tile(env, 3, 13, 2, 1, scale_b, sizeof(scale_b));
    set_tile(env, 4, 1, 1, 1, &bias, sizeof(bias));
    g_assert_true(
        linx_tile_cube_compute_058(env, 0, 1, 2, 3, 4, 0, true, true, false));
    memcpy(&result, env->tile_acc, sizeof(result));
    g_assert_cmpfloat(result, ==, 290);

    memcpy(env->tile_acc, &prior, sizeof(prior));
    env->tile_acc_bytes = 16;
    env->tile_acc_dtype = LINX_TILE_ACC_FP32;
    env->tile_acc_valid = 1;
    env->tile_acc_rows = env->tile_acc_cols = 1;
    g_assert_true(
        linx_tile_cube_compute_058(env, 0, 1, 2, 3, 0, 0, true, false, true));
    memcpy(&result, env->tile_acc, sizeof(result));
    g_assert_cmpfloat(result, ==, 291);
    g_free(env);
}

static void test_reject_non_power_of_two_k(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    uint8_t left[33], right[33], scale_a[] = {0x7f, 0x80};
    uint8_t scale_b[] = {0x7f, 0x81};
    uint32_t before_acc[LINX_TILE_MAX_WORDS];
    uint32_t before_acc_bytes;
    uint16_t before_acc_cols, before_acc_rows;
    uint8_t before_acc_dtype, before_acc_valid;

    memset(left, 0x38, sizeof(left));
    memset(right, 0x3c, sizeof(right));
    env->lb[0] = 1;
    env->lb[1] = 1;
    env->lb[2] = 33;
    env->tile_dtype = 1;
    set_tile(env, 0, 7, 1, 33, left, sizeof(left));
    set_tile(env, 1, 8, 33, 1, right, sizeof(right));
    set_tile(env, 2, 13, 1, 2, scale_a, sizeof(scale_a));
    set_tile(env, 3, 13, 2, 1, scale_b, sizeof(scale_b));
    memset(env->tile_acc, 0xa5, sizeof(env->tile_acc));
    env->tile_acc_bytes = 16;
    env->tile_acc_dtype = LINX_TILE_ACC_FP64;
    env->tile_acc_valid = 1;
    env->tile_acc_cols = 7;
    env->tile_acc_rows = 9;
    memcpy(before_acc, env->tile_acc, sizeof(before_acc));
    before_acc_bytes = env->tile_acc_bytes;
    before_acc_dtype = env->tile_acc_dtype;
    before_acc_valid = env->tile_acc_valid;
    before_acc_cols = env->tile_acc_cols;
    before_acc_rows = env->tile_acc_rows;

    g_assert_false(
        linx_tile_cube_compute_058(env, 0, 1, 2, 3, 0, 0, true, false, false));
    g_assert_cmpmem(env->tile_acc, sizeof(before_acc), before_acc,
                    sizeof(before_acc));
    g_assert_cmpuint(env->tile_acc_bytes, ==, before_acc_bytes);
    g_assert_cmpuint(env->tile_acc_dtype, ==, before_acc_dtype);
    g_assert_cmpuint(env->tile_acc_valid, ==, before_acc_valid);
    g_assert_cmpuint(env->tile_acc_cols, ==, before_acc_cols);
    g_assert_cmpuint(env->tile_acc_rows, ==, before_acc_rows);
    g_free(env);
}

static void test_reject_undersized_accumulator(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    CPULinxState *before;
    float left[] = {1, 2, 3, 4};
    float right[] = {5, 6, 7, 8};
    float prior = 3;

    env->lb[0] = env->lb[1] = env->lb[2] = 2;
    env->tile_dtype = 1;
    set_tile(env, 0, 1, 2, 2, left, sizeof(left));
    set_tile(env, 1, 1, 2, 2, right, sizeof(right));
    memset(env->tile_acc, 0xa5, sizeof(env->tile_acc));
    memcpy(env->tile_acc, &prior, sizeof(prior));
    env->tile_acc_bytes = sizeof(prior);
    env->tile_acc_dtype = LINX_TILE_ACC_FP32;
    env->tile_acc_valid = 1;
    env->tile_acc_rows = 2;
    env->tile_acc_cols = 2;
    before = g_memdup2(env, sizeof(*env));

    g_assert_false(
        linx_tile_cube_compute_058(env, 0, 1, 0, 0, 0, 0, false, false, true));
    g_assert_cmpmem(env, sizeof(*env), before, sizeof(*before));
    g_free(before);
    g_free(env);
}

static void test_accumulator_convert_all_rounding_modes(void)
{
    static const struct {
        unsigned dtype;
        double halfway;
        uint64_t lower;
        uint64_t upper;
    } formats[] = {
        {1, 1.000000059604644775390625, 0x3f800000, 0x3f800001},
        {2, 1.00048828125, 0x3f800000, 0x3f802000},
        {3, 1.000244140625, 0x3f800000, 0x3f801000},
        {4, 1.00048828125, 0x3c00, 0x3c01},
        {5, 1.00390625, 0x3f80, 0x3f81},
        {6, 1.0625, 0x08, 0x09},
        {7, 1.0625, 0x38, 0x39},
        {8, 1.125, 0x3c, 0x3d},
        {9, 1.125, 0x0c, 0x0d},
        {10, 1.0625, 0x08, 0x09},
        {11, 1.25, 0x2, 0x3},
        {12, .375, 0x1, 0x2},
        {14, .375, 0x1, 0x2},
    };
    static const uint16_t positive[] = {
        0x3c00, 0x3c00, 0x3c00, 0x3c00, 0x3c01, 0x3c01, 0x3c01, 0x3c01,
    };
    static const uint8_t packed[] = {
        0x2, 0x2, 0x1, 0x1, 0x2, 0x2, 0x1, 0x2,
    };
    CPULinxState *env = g_new0(CPULinxState, 1);
    double half_fp16 = 1.0 + ldexp(1.0, -11);

    for (unsigned mode = 0; mode < 8; mode++) {
        g_assert_cmphex(run_accumulator_convert(env, 4, half_fp16, mode, false), ==,
                        positive[mode]);
        g_assert_cmphex(run_accumulator_convert(env, 12, .375, mode, false) & 0xfu, ==,
                        packed[mode]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(formats); i++) {
        for (unsigned mode = 0; mode < 8; mode++) {
            bool upper = mode == 4 || mode == 5 || mode == 7 ||
                         (mode == 6 && (formats[i].lower & 1u) == 0u) ||
                         ((mode == 0 || mode == 1) && (formats[i].lower & 1u));
            uint64_t expected = upper ? formats[i].upper : formats[i].lower;
            g_assert_cmphex(run_accumulator_convert(env, formats[i].dtype,
                                       formats[i].halfway, mode, false),
                            ==, expected);
        }
    }
    for (unsigned mode = 0; mode < 8; mode++) {
        g_assert_cmphex(run_accumulator_convert(env, 0, 1.0, mode, false), ==,
                        UINT64_C(0x3ff0000000000000));
    }
    g_assert_cmphex(run_accumulator_convert(env, 4, -half_fp16, 7, false), ==, 0xbc00);
    g_free(env);
}

static void test_accumulator_convert_is_host_fenv_independent(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    double value = 1.0 + ldexp(1.0, -11);
    uint32_t operand = 0x3f800001;
    uint32_t cube_expected;
    uint64_t expected;
    int original = fegetround();
    const int host_modes[] = {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};

    env->lb[0] = env->lb[1] = env->lb[2] = 1;
    env->tile_dtype = 1;
    set_tile(env, 0, 1, 1, 1, &operand, sizeof(operand));
    set_tile(env, 1, 1, 1, 1, &operand, sizeof(operand));
    fesetround(FE_TONEAREST);
    g_assert_true(
        linx_tile_cube_compute_058(env, 0, 1, 0, 0, 0, 0, false, false, false));
    memcpy(&cube_expected, env->tile_acc, sizeof(cube_expected));
    expected = run_accumulator_convert(env, 4, value, 1, false);
    for (unsigned i = 0; i < G_N_ELEMENTS(host_modes); i++) {
        fesetround(host_modes[i]);
        env->tile_dtype = 1;
        g_assert_true(linx_tile_cube_compute_058(env, 0, 1, 0, 0, 0, 0, false,
                                                 false, false));
        g_assert_cmphex(*(uint32_t *)env->tile_acc, ==, cube_expected);
        g_assert_cmphex(run_accumulator_convert(env, 4, value, 1, false), ==, expected);
    }
    fesetround(original);
    g_free(env);
}

static void test_accumulator_convert_requested_rounding_and_subnormal(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    const double fp32_min_subnormal = ldexp(1.0, -149);
    const double fp16_halfway = 1.0 + ldexp(1.0, -11);

    g_assert_cmphex(run_accumulator_convert(env, 1, fp32_min_subnormal, 1,
                                            false), ==, 0x00000001);
    g_assert_cmphex(run_accumulator_convert(env, 4, fp16_halfway, 1, false),
                    ==, 0x3c00);
    g_assert_cmphex(run_accumulator_convert(env, 4, fp16_halfway, 4, false),
                    ==, 0x3c01);
    g_free(env);
}

static void test_fpatr_relu_postprocess_and_fail_closed(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    float negative = -2.0f;
    uint32_t raw = UINT32_MAX;

    memset(env->tile_acc, 0, sizeof(env->tile_acc));
    memcpy(env->tile_acc, &negative, sizeof(negative));
    env->tile_acc_bytes = sizeof(negative);
    env->tile_acc_dtype = LINX_TILE_ACC_FP32;
    env->tile_acc_valid = 1;
    env->tile_acc_rows = 1;
    env->tile_acc_cols = 1;
    env->tile_dtype = 1;
    env->tile_reg_capacity[31] = 16;
    env->tile_fpatr_valid = 1;
    env->tile_fpatr_raw = 1u << 23; /* ReluMode=ReLU, PreQuant=None. */
    g_assert_true(linx_tile_accumulator_convert(env, 31, 0));
    memcpy(&raw, env->tile_reg[31], sizeof(raw));
    g_assert_cmphex(raw, ==, 0x00000000);

    memset(env->tile_reg[31], 0xa5, sizeof(env->tile_reg[31]));
    env->tile_fpatr_raw = 2u << 23; /* Parameterized LReLU is unavailable. */
    g_assert_false(linx_tile_accumulator_convert(env, 31, 0));
    for (unsigned i = 0; i < G_N_ELEMENTS(env->tile_reg[31]); i++) {
        g_assert_cmphex(env->tile_reg[31][i], ==, 0xa5a5a5a5);
    }
    g_free(env);
}

static void test_accumulator_convert_saturates_every_float_target(void)
{
    static const struct {
        unsigned dtype;
        uint64_t max;
    } cases[] = {
        {0, UINT64_C(0x7fefffffffffffff)},
        {1, 0x7f7fffff},
        {2, 0x7f7fe000},
        {3, 0x7f7ff000},
        {4, 0x7bff},
        {5, 0x7f7f},
        {6, 0x6e},
        {7, 0x7e},
        {8, 0x7b},
        {9, 0x1f},
        {10, 0x1f},
        {11, 0x7},
        {12, 0x7},
        {14, 0x7},
    };
    CPULinxState *env = g_new0(CPULinxState, 1);

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_cmphex(run_accumulator_convert(env, cases[i].dtype, INFINITY, 1, true), ==,
                        cases[i].max);
        if (cases[i].dtype != 0) {
            g_assert_cmphex(run_accumulator_convert(env, cases[i].dtype, 1e300, 1, true), ==,
                            cases[i].max);
        }
    }
    g_free(env);
}

static void test_accumulator_convert_preserves_signed_zero(void)
{
    static const struct {
        unsigned dtype;
        uint64_t zero;
    } cases[] = {
        {0, UINT64_C(0x8000000000000000)},
        {1, 0x80000000},
        {2, 0x80000000},
        {3, 0x80000000},
        {4, 0x8000},
        {5, 0x8000},
        {6, 0x00},
        {7, 0x80},
        {8, 0x80},
        {9, 0x20},
        {10, 0x20},
        {11, 0x8},
        {12, 0x8},
        {14, 0x8},
    };
    CPULinxState *env = g_new0(CPULinxState, 1);

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_cmphex(run_accumulator_convert(env, cases[i].dtype, -0.0, 1, false), ==,
                        cases[i].zero);
    }
    double packed_values[] = {.25, -.25};
    set_fp64_acc(env, packed_values, 1, 2);
    env->tile_dtype = 12;
    env->tile_attr_raw = 1u << 25;
    env->tile_reg_capacity[31] = 16;
    g_assert_true(linx_tile_accumulator_convert(env, 31, 0));
    g_assert_cmphex(((uint8_t *)env->tile_reg[31])[0], ==, 0x91);
    g_assert_cmpuint(env->tile_reg_cols[31], ==, 2);
    g_free(env);
}

static void test_accumulator_convert_canonicalizes_nan(void)
{
    static const struct {
        unsigned dtype;
        uint64_t canonical_nan;
    } cases[] = {
        {0, UINT64_C(0x7ff8000000000000)},
        {1, 0x7fc00000},
        {2, 0x7fc00000},
        {3, 0x7fc00000},
        {4, 0x7e00},
        {5, 0x7fc0},
        {6, 0x80},
        {7, 0x7f},
        {8, 0x7e},
    };
    CPULinxState *env = g_new0(CPULinxState, 1);
    const double positive_nan = NAN;
    const double negative_nan = copysign(NAN, -1.0);

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_assert_cmphex(run_accumulator_convert(env, cases[i].dtype, positive_nan, 1, false),
                        ==, cases[i].canonical_nan);
        g_assert_cmphex(run_accumulator_convert(env, cases[i].dtype, negative_nan, 1, false),
                        ==, cases[i].canonical_nan);
    }
    g_free(env);
}

static void test_failures_preserve_acc_and_destination(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    double value = 1.25;
    uint32_t before_tile[LINX_TILE_MAX_WORDS];
    uint32_t before_acc[LINX_TILE_MAX_WORDS];
    uint32_t before_tile_capacity, before_tile_bytes, before_acc_bytes;
    uint16_t before_tile_valid_cols, before_tile_valid_rows;
    uint16_t before_tile_cols, before_tile_rows;
    uint16_t before_acc_cols, before_acc_rows;
    uint8_t before_tile_elem_bytes, before_tile_dtype;
    uint8_t before_acc_dtype, before_acc_valid;

    set_fp64_acc(env, &value, 1, 1);
    env->tile_dtype = 4;
    env->tile_reg_capacity[31] = 8;
    env->tile_reg[31][0] = 0xdeadbeef;
    memcpy(before_tile, env->tile_reg[31], sizeof(before_tile));
    memcpy(before_acc, env->tile_acc, sizeof(before_acc));
    before_tile_capacity = env->tile_reg_capacity[31];
    before_tile_bytes = env->tile_reg_bytes[31];
    before_tile_elem_bytes = env->tile_reg_elem_bytes[31];
    before_tile_dtype = env->tile_reg_dtype[31];
    before_tile_valid_cols = env->tile_reg_valid_cols[31];
    before_tile_valid_rows = env->tile_reg_valid_rows[31];
    before_tile_cols = env->tile_reg_cols[31];
    before_tile_rows = env->tile_reg_rows[31];
    before_acc_bytes = env->tile_acc_bytes;
    before_acc_dtype = env->tile_acc_dtype;
    before_acc_valid = env->tile_acc_valid;
    before_acc_cols = env->tile_acc_cols;
    before_acc_rows = env->tile_acc_rows;
    g_assert_false(linx_tile_accumulator_convert(env, 31, 0));
    g_assert_cmpmem(env->tile_reg[31], sizeof(before_tile), before_tile,
                    sizeof(before_tile));
    g_assert_cmpmem(env->tile_acc, sizeof(before_acc), before_acc,
                    sizeof(before_acc));
    g_assert_cmpuint(env->tile_reg_capacity[31], ==, before_tile_capacity);
    g_assert_cmpuint(env->tile_reg_bytes[31], ==, before_tile_bytes);
    g_assert_cmpuint(env->tile_reg_elem_bytes[31], ==, before_tile_elem_bytes);
    g_assert_cmpuint(env->tile_reg_dtype[31], ==, before_tile_dtype);
    g_assert_cmpuint(env->tile_reg_valid_cols[31], ==, before_tile_valid_cols);
    g_assert_cmpuint(env->tile_reg_valid_rows[31], ==, before_tile_valid_rows);
    g_assert_cmpuint(env->tile_reg_cols[31], ==, before_tile_cols);
    g_assert_cmpuint(env->tile_reg_rows[31], ==, before_tile_rows);
    g_assert_cmpuint(env->tile_acc_bytes, ==, before_acc_bytes);
    g_assert_cmpuint(env->tile_acc_dtype, ==, before_acc_dtype);
    g_assert_cmpuint(env->tile_acc_valid, ==, before_acc_valid);
    g_assert_cmpuint(env->tile_acc_cols, ==, before_acc_cols);
    g_assert_cmpuint(env->tile_acc_rows, ==, before_acc_rows);

    env->lb[0] = env->lb[1] = 1;
    env->lb[2] = 33;
    env->tile_dtype = 1;
    uint8_t left[33] = {0}, right[33] = {0}, bad_scale[2] = {0};
    set_tile(env, 0, 7, 1, 33, left, sizeof(left));
    set_tile(env, 1, 8, 33, 1, right, sizeof(right));
    set_tile(env, 2, 19, 1, 2, bad_scale, sizeof(bad_scale));
    set_tile(env, 3, 13, 2, 1, bad_scale, sizeof(bad_scale));
    memcpy(before_acc, env->tile_acc, sizeof(before_acc));
    before_acc_bytes = env->tile_acc_bytes;
    before_acc_dtype = env->tile_acc_dtype;
    before_acc_valid = env->tile_acc_valid;
    before_acc_cols = env->tile_acc_cols;
    before_acc_rows = env->tile_acc_rows;
    g_assert_false(
        linx_tile_cube_compute_058(env, 0, 1, 2, 3, 0, 0, true, false, false));
    g_assert_cmpmem(env->tile_acc, sizeof(before_acc), before_acc,
                    sizeof(before_acc));
    g_assert_cmpuint(env->tile_acc_bytes, ==, before_acc_bytes);
    g_assert_cmpuint(env->tile_acc_dtype, ==, before_acc_dtype);
    g_assert_cmpuint(env->tile_acc_valid, ==, before_acc_valid);
    g_assert_cmpuint(env->tile_acc_cols, ==, before_acc_cols);
    g_assert_cmpuint(env->tile_acc_rows, ==, before_acc_rows);
    g_free(env);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/cube/type-matrix", test_all_production_type_rows);
    g_test_add_func("/linx/cube/ordinary-bias-acc", test_ordinary_bias_and_acc);
    g_test_add_func("/linx/cube/upper-physical-slots",
                    test_upper_physical_slots);
    g_test_add_func("/linx/cube/non-square-loop-bounds",
                    test_non_square_loop_bound_mapping);
    g_test_add_func("/linx/cube/group-profile-dimensions",
                    test_group_profile_dimension_contract);
    g_test_add_func("/linx/cube/mx-k64", test_mx_k64);
    g_test_add_func("/linx/cube/reject-non-power-of-two-k",
                    test_reject_non_power_of_two_k);
    g_test_add_func("/linx/cube/reject-undersized-accumulator",
                    test_reject_undersized_accumulator);
    g_test_add_func("/linx/cube/accumulator_convert-rounding",
                    test_accumulator_convert_all_rounding_modes);
    g_test_add_func("/linx/cube/accumulator_convert-host-fenv",
                    test_accumulator_convert_is_host_fenv_independent);
    g_test_add_func("/linx/cube/accumulator_convert-requested-rounding-subnormal",
                    test_accumulator_convert_requested_rounding_and_subnormal);
    g_test_add_func("/linx/cube/fpatr-postprocess",
                    test_fpatr_relu_postprocess_and_fail_closed);
    g_test_add_func("/linx/cube/accumulator_convert-saturation",
                    test_accumulator_convert_saturates_every_float_target);
    g_test_add_func("/linx/cube/accumulator_convert-signed-zero",
                    test_accumulator_convert_preserves_signed_zero);
    g_test_add_func("/linx/cube/accumulator_convert-canonical-nan",
                    test_accumulator_convert_canonicalizes_nan);
    g_test_add_func("/linx/cube/fault-preservation",
                    test_failures_preserve_acc_and_destination);
    return g_test_run();
}
