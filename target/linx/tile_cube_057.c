/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "tile_cube_057.h"
#include "tile_numeric_057.h"

static unsigned cube_dtype_bytes(uint32_t dtype)
{
    switch (dtype & 31u) {
    case 0u:
    case 16u:
    case 24u:
        return 8u;
    case 1u:
    case 2u:
    case 3u:
    case 17u:
    case 25u:
        return 4u;
    case 4u:
    case 5u:
    case 18u:
    case 26u:
        return 2u;
    case 6u:
    case 7u:
    case 8u:
    case 9u:
    case 10u:
    case 11u:
    case 12u:
    case 13u:
    case 14u:
    case 19u:
    case 20u:
    case 27u:
    case 28u:
        return 1u;
    default:
        return 0u;
    }
}

static unsigned cube_dim(uint32_t value) { return value ? value : 8u; }

static bool cube_operand_legal(const CPULinxState *env, unsigned tile,
                               uint32_t dtype, unsigned rows, unsigned cols)
{
    unsigned elem_bytes = cube_dtype_bytes(dtype);
    uint64_t row_bytes, required;

    if (tile >= LINX_TILE_SLOT_COUNT || elem_bytes == 0u ||
        env->tile_reg_dtype[tile] != (dtype & 31u) ||
        env->tile_reg_elem_bytes[tile] != elem_bytes ||
        env->tile_reg_valid_rows[tile] < rows ||
        env->tile_reg_valid_cols[tile] < cols ||
        env->tile_reg_rows[tile] < rows || env->tile_reg_cols[tile] < cols) {
        return false;
    }
    row_bytes = linx_tile_numeric_is_packed(dtype)
                    ? (env->tile_reg_cols[tile] + 1u) / 2u
                    : (uint64_t)env->tile_reg_cols[tile] * elem_bytes;
    required =
        (uint64_t)(rows - 1u) * row_bytes + (linx_tile_numeric_is_packed(dtype)
                                                 ? (cols + 1u) / 2u
                                                 : (uint64_t)cols * elem_bytes);
    return env->tile_reg_bytes[tile] >= required;
}

static bool cube_read_raw(const CPULinxState *env, unsigned tile, unsigned row,
                          unsigned column, uint64_t *raw)
{
    uint32_t dtype = env->tile_reg_dtype[tile] & 31u;
    unsigned elem_bytes = cube_dtype_bytes(dtype);
    unsigned stride = env->tile_reg_cols[tile];
    uint64_t lane, offset;

    if (tile >= LINX_TILE_SLOT_COUNT || elem_bytes == 0u ||
        row >= env->tile_reg_rows[tile] ||
        column >= stride) {
        return false;
    }
    lane = linx_tile_numeric_is_packed(dtype)
               ? (uint64_t)row * ((stride + 1u) / 2u) + column / 2u
               : (uint64_t)row * stride + column;
    offset = lane * elem_bytes;
    if (offset + elem_bytes > env->tile_reg_bytes[tile]) {
        return false;
    }
    *raw = 0u;
    memcpy(raw, (const uint8_t *)env->tile_reg[tile] + offset, elem_bytes);
    return true;
}

static bool cube_buffer_operand_legal(const uint8_t *data, uint32_t bytes,
                                      uint32_t dtype, unsigned rows,
                                      unsigned cols)
{
    unsigned elem_bytes = cube_dtype_bytes(dtype);
    uint64_t row_bytes;

    if (data == NULL || elem_bytes == 0u || rows == 0u || cols == 0u) {
        return false;
    }
    row_bytes = linx_tile_numeric_is_packed(dtype) ? (cols + 1u) / 2u
                                                    : (uint64_t)cols * elem_bytes;
    return (uint64_t)rows * row_bytes <= bytes;
}

static bool cube_read_buffer_raw(const uint8_t *data, uint32_t bytes,
                                 uint32_t dtype, unsigned stride,
                                 unsigned row, unsigned column, uint64_t *raw)
{
    unsigned elem_bytes = cube_dtype_bytes(dtype);
    uint64_t lane;
    uint64_t offset;

    if (data == NULL || elem_bytes == 0u || column >= stride) {
        return false;
    }
    lane = linx_tile_numeric_is_packed(dtype)
               ? (uint64_t)row * ((stride + 1u) / 2u) + column / 2u
               : (uint64_t)row * stride + column;
    offset = lane * elem_bytes;
    if (offset + elem_bytes > bytes) {
        return false;
    }
    *raw = 0u;
    memcpy(raw, data + offset, elem_bytes);
    return true;
}

static int64_t cube_signed(uint32_t dtype, uint64_t raw, unsigned logical_lane)
{
    switch (dtype & 31u) {
    case 16u:
        return (int64_t)raw;
    case 17u:
        return (int32_t)raw;
    case 18u:
        return (int16_t)raw;
    case 19u:
        return (int8_t)raw;
    case 20u: {
        uint8_t lane = linx_tile_numeric_nibble(raw, logical_lane);
        return (int8_t)((lane ^ 8u) - 8u);
    }
    default:
        return 0;
    }
}

static bool linx_tile_cube_compute_common_057(
    CPULinxState *env, unsigned src_a, unsigned src_b,
    const uint8_t *shared_b, uint32_t shared_b_bytes,
    uint32_t shared_b_dtype, unsigned row_scale, unsigned column_scale,
    unsigned bias, unsigned size_code, bool mx, bool with_bias,
    bool accumulate, unsigned a_row_base)
{
    unsigned m = cube_dim(env->lb[0]);
    unsigned n = cube_dim(env->lb[1]);
    unsigned kdim = cube_dim(env->lb[2]);
    uint32_t left_dtype =
        src_a < LINX_TILE_SLOT_COUNT
            ? env->tile_reg_dtype[src_a] & 31u
            : UINT32_MAX;
    uint32_t right_dtype = shared_b != NULL
                               ? shared_b_dtype & 31u
                               : src_b < LINX_TILE_SLOT_COUNT
                                     ? env->tile_reg_dtype[src_b] & 31u
                                     : UINT32_MAX;
    uint8_t acc_dtype =
        mx ? LINX_TILE_ACC_FP32 : linx_tile_numeric_acc_dtype(env->tile_dtype);
    unsigned acc_bytes = 4u;
    uint64_t allocated = size_code < 60u ? UINT64_C(1) << (size_code + 4u) : 0u;
    uint64_t required = (uint64_t)m * n * acc_bytes;
    unsigned groups = (kdim + 31u) / 32u;
    float_status fp_status = {0};
    uint8_t *next;

    if (src_a >= LINX_TILE_SLOT_COUNT ||
        (shared_b == NULL && src_b >= LINX_TILE_SLOT_COUNT) ||
        m > UINT16_MAX || n > UINT16_MAX ||
        acc_dtype == UINT8_MAX || allocated == 0u ||
        allocated > LINX_TILE_MAX_BYTES || required == 0u ||
        required > allocated ||
        (mx ? ((env->tile_dtype & 31u) != 1u ||
               !linx_tile_numeric_mx_pair(left_dtype, right_dtype))
            : (!linx_tile_numeric_ordinary(env->tile_dtype) ||
               left_dtype != (env->tile_dtype & 31u) ||
               right_dtype != (env->tile_dtype & 31u))) ||
        !cube_operand_legal(env, src_a, left_dtype, a_row_base + m, kdim) ||
        (shared_b != NULL
             ? !cube_buffer_operand_legal(shared_b, shared_b_bytes,
                                          right_dtype, kdim, n)
             : !cube_operand_legal(env, src_b, right_dtype, kdim, n)) ||
        (mx && (!cube_operand_legal(env, row_scale, 13u, m, groups) ||
                !cube_operand_legal(env, column_scale, 13u, groups, n))) ||
        (with_bias &&
         !cube_operand_legal(env, bias, mx ? 1u : env->tile_dtype, 1u, n)) ||
        (accumulate &&
         (!env->tile_acc_valid || env->tile_acc_dtype != acc_dtype ||
          env->tile_acc_rows != m || env->tile_acc_cols != n ||
          env->tile_acc_bytes < required))) {
        return false;
    }

    set_float_rounding_mode(float_round_nearest_even, &fp_status);
    set_default_nan_mode(true, &fp_status);
    set_float_default_nan_pattern(0b01000000, &fp_status);
    next = g_malloc0(allocated);
    if (accumulate) {
        memcpy(next, env->tile_acc, required);
    }
    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            uint64_t index = (uint64_t)i * n + j;
            if (acc_dtype == LINX_TILE_ACC_FP32) {
                uint32_t acc = 0u;
                if (accumulate) {
                    memcpy(&acc, next + index * 4u, 4u);
                }
                for (unsigned k = 0; k < kdim; k++) {
                    uint64_t ar, br;
                    uint32_t av, bv;
                    if (!cube_read_raw(env, src_a, a_row_base + i, k, &ar) ||
                        (shared_b != NULL
                             ? !cube_read_buffer_raw(shared_b, shared_b_bytes,
                                                     right_dtype, n, k, j,
                                                     &br)
                             : !cube_read_raw(env, src_b, k, j, &br))) {
                        goto fail;
                    }
                    av = linx_tile_numeric_f32_raw(
                        linx_tile_numeric_decode(left_dtype, ar, k));
                    bv = linx_tile_numeric_f32_raw(
                        linx_tile_numeric_decode(right_dtype, br, j));
                    if (mx) {
                        uint64_t sar, sbr;
                        uint32_t scale_a, scale_b;
                        if (!cube_read_raw(env, row_scale, i, k / 32u, &sar) ||
                            !cube_read_raw(env, column_scale, k / 32u, j,
                                           &sbr)) {
                            goto fail;
                        }
                        scale_a = linx_tile_numeric_f32_raw(
                            linx_tile_numeric_decode(13u, sar, 0));
                        scale_b = linx_tile_numeric_f32_raw(
                            linx_tile_numeric_decode(13u, sbr, 0));
                        av = float32_mul(av, scale_a, &fp_status);
                        bv = float32_mul(bv, scale_b, &fp_status);
                    }
                    acc = float32_muladd(av, bv, acc, 0, &fp_status);
                }
                if (with_bias) {
                    uint64_t raw;
                    uint32_t bias_raw;
                    if (!cube_read_raw(env, bias, 0, j, &raw)) {
                        goto fail;
                    }
                    bias_raw =
                        linx_tile_numeric_f32_raw(linx_tile_numeric_decode(
                            mx ? 1u : env->tile_dtype, raw, j));
                    acc = float32_add(acc, bias_raw, &fp_status);
                }
                memcpy(next + index * 4u, &acc, 4u);
            } else {
                uint32_t acc = 0u;
                if (accumulate) {
                    memcpy(&acc, next + index * 4u, 4u);
                }
                for (unsigned k = 0; k < kdim; k++) {
                    uint64_t ar, br;
                    if (!cube_read_raw(env, src_a, a_row_base + i, k, &ar) ||
                        (shared_b != NULL
                             ? !cube_read_buffer_raw(shared_b, shared_b_bytes,
                                                     right_dtype, n, k, j,
                                                     &br)
                             : !cube_read_raw(env, src_b, k, j, &br))) {
                        goto fail;
                    }
                    acc += (uint32_t)(cube_signed(left_dtype, ar, k) *
                                      cube_signed(right_dtype, br, j));
                }
                if (with_bias) {
                    uint64_t raw;
                    if (!cube_read_raw(env, bias, 0, j, &raw)) {
                        goto fail;
                    }
                    acc += (uint32_t)cube_signed(env->tile_dtype, raw, j);
                }
                memcpy(next + index * 4u, &acc, 4u);
            }
        }
    }
    memset(env->tile_acc, 0, sizeof(env->tile_acc));
    memcpy(env->tile_acc, next, allocated);
    env->tile_acc_bytes = allocated;
    env->tile_acc_dtype = acc_dtype;
    env->tile_acc_valid = 1u;
    env->tile_acc_cols = n;
    env->tile_acc_rows = m;
    g_free(next);
    return true;
fail:
    g_free(next);
    return false;
}

bool linx_tile_cube_compute_057(CPULinxState *env, unsigned src_a,
                                unsigned src_b, unsigned row_scale,
                                unsigned column_scale, unsigned bias,
                                unsigned size_code, bool mx, bool with_bias,
                                bool accumulate)
{
    return linx_tile_cube_compute_common_057(
        env, src_a, src_b, NULL, 0u, 0u, row_scale, column_scale, bias,
        size_code, mx, with_bias, accumulate, 0u);
}

bool linx_tile_cube_compute_shared_b_057(
    CPULinxState *env, unsigned src_a, const uint8_t *shared_b,
    uint32_t shared_b_bytes, uint32_t shared_b_dtype, unsigned size_code,
    bool accumulate)
{
    const uint32_t logical_m = env->lb[0];
    uint32_t shard_m;
    bool valid;

    if (logical_m == 0u || logical_m % LINX_CORE4_PE_COUNT != 0u) {
        return false;
    }
    shard_m = logical_m / LINX_CORE4_PE_COUNT;
    env->lb[0] = shard_m;
    valid = linx_tile_cube_compute_common_057(
        env, src_a, UINT_MAX, shared_b, shared_b_bytes, shared_b_dtype, 0u,
        0u, 0u, size_code, false, false, accumulate, 0u);
    env->lb[0] = logical_m;
    return valid;
}
