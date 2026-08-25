/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "tile_cube_058.h"
#include "tile_numeric_058.h"

static unsigned cube_dimension(uint32_t value)
{
    return value ? value : 8u;
}

static bool cube_is_tmatmul_family(const CPULinxState *env)
{
    switch (env->tile_func & 0x1fu) {
    case 0u:  /* TMATMUL */
    case 1u:  /* TMATMUL.BIAS */
    case 2u:  /* TMATMUL.ACC */
    case 4u:  /* TMATMUL.MX */
    case 5u:  /* TMATMUL.MX.BIAS */
    case 6u:  /* TMATMUL.MX.ACC */
        return true;
    default:
        return false;
    }
}

static unsigned cube_dtype_bytes(uint32_t dtype);

static unsigned cube_dtype_bits(uint32_t dtype)
{
    return linx_tile_numeric_is_packed(dtype) ? 4u
                                               : cube_dtype_bytes(dtype) * 8u;
}

static bool cube_layout(uint32_t layout)
{
    return layout == LINX_TILE_LAYOUT_CUBE_M16 ||
           layout == LINX_TILE_LAYOUT_CUBE_M32 ||
           layout == LINX_TILE_LAYOUT_CUBE_N8;
}

static bool cube_cell_geometry(uint32_t layout, uint32_t dtype,
                               uint32_t *cell_rows, uint32_t *cell_cols)
{
    const unsigned bits = cube_dtype_bits(dtype);

    if (!cube_layout(layout) || bits == 0u || bits == 64u || dtype == 14u) {
        return false;
    }
    if (layout == LINX_TILE_LAYOUT_CUBE_M16) {
        *cell_rows = 16u;
        *cell_cols = 64u / bits;
    } else if (layout == LINX_TILE_LAYOUT_CUBE_M32) {
        *cell_rows = 32u;
        *cell_cols = 32u / bits;
    } else {
        *cell_rows = 128u / bits;
        *cell_cols = 8u;
    }
    return *cell_rows != 0u && *cell_cols != 0u;
}

bool linx_tile_cube_descriptor_058(CPULinxState *env, unsigned tile,
                                   uint32_t layout, uint32_t dtype,
                                   uint32_t valid_rows, uint32_t valid_cols,
                                   uint32_t capacity_bytes)
{
    uint32_t cell_rows, cell_cols, storage_rows, storage_cols;
    uint32_t k_repeat, n_repeat, cell_count, storage_bytes;

    if (tile >= LINX_TILE_SLOT_COUNT || valid_rows == 0u ||
        valid_cols == 0u || !cube_cell_geometry(layout, dtype, &cell_rows,
                                                &cell_cols)) {
        return false;
    }
    if (layout == LINX_TILE_LAYOUT_CUBE_N8) {
        storage_rows = ROUND_UP(valid_rows, cell_rows);
    } else {
        if (valid_rows > cell_rows) {
            return false;
        }
        storage_rows = cell_rows;
    }
    storage_cols = ROUND_UP(valid_cols, cell_cols);
    k_repeat = layout == LINX_TILE_LAYOUT_CUBE_N8
                   ? storage_rows / cell_rows
                   : storage_cols / cell_cols;
    n_repeat = layout == LINX_TILE_LAYOUT_CUBE_N8
                   ? storage_cols / 8u : 1u;
    cell_count = k_repeat * n_repeat;
    storage_bytes = cell_count * LINX_TILE_CELL_BYTES;
    if (cell_count == 0u || cell_count > UINT16_MAX ||
        storage_bytes > capacity_bytes ||
        capacity_bytes > LINX_TILE_PE_CAPACITY_BYTES ||
        storage_rows > UINT16_MAX || storage_cols > UINT16_MAX) {
        return false;
    }
    env->tile_reg_layout[tile] = layout;
    env->tile_reg_capacity[tile] = capacity_bytes;
    env->tile_reg_bytes[tile] = capacity_bytes;
    env->tile_reg_dtype[tile] = dtype;
    env->tile_reg_elem_bytes[tile] = cube_dtype_bytes(dtype);
    env->tile_reg_valid_rows[tile] = valid_rows;
    env->tile_reg_valid_cols[tile] = valid_cols;
    env->tile_reg_rows[tile] = storage_rows;
    env->tile_reg_cols[tile] = storage_cols;
    env->tile_reg_cube_k_repeat[tile] = k_repeat;
    env->tile_reg_cube_n_repeat[tile] = n_repeat;
    env->tile_reg_cube_cell_count[tile] = cell_count;
    env->tile_reg_cube_storage_bytes[tile] = storage_bytes;
    return true;
}

bool linx_tile_cube_payload_index_058(const CPULinxState *env, unsigned tile,
                                      unsigned row, unsigned column,
                                      uint32_t *index)
{
    uint32_t cell_rows, cell_cols, cell_index, inner_row, inner_col;
    uint32_t local;
    const uint32_t layout = tile < LINX_TILE_SLOT_COUNT
                                ? env->tile_reg_layout[tile] : UINT32_MAX;
    const uint32_t dtype = tile < LINX_TILE_SLOT_COUNT
                               ? env->tile_reg_dtype[tile] : UINT32_MAX;

    if (index == NULL || tile >= LINX_TILE_SLOT_COUNT ||
        row >= env->tile_reg_rows[tile] ||
        column >= env->tile_reg_cols[tile] ||
        !cube_cell_geometry(layout, dtype, &cell_rows, &cell_cols)) {
        return false;
    }
    if (layout == LINX_TILE_LAYOUT_CUBE_N8) {
        const uint32_t cell_k = row / cell_rows;
        const uint32_t cell_n = column / cell_cols;
        cell_index = cell_n * env->tile_reg_cube_k_repeat[tile] + cell_k;
        inner_row = row % cell_rows;
        inner_col = column % cell_cols;
        local = inner_col * cell_rows + inner_row;
    } else {
        cell_index = column / cell_cols;
        inner_row = row;
        inner_col = column % cell_cols;
        if (layout == LINX_TILE_LAYOUT_CUBE_M16 &&
            cube_dtype_bits(dtype) == 4u) {
            inner_col = inner_col < 4u ? inner_col
                        : inner_col < 8u ? inner_col + 4u
                        : inner_col < 12u ? inner_col - 4u : inner_col;
        }
        local = inner_row * cell_cols + inner_col;
    }
    *index = cell_index * cell_rows * cell_cols + local;
    return *index < (LINX_TILE_PE_CAPACITY_BYTES * 8u) /
                         cube_dtype_bits(dtype);
}

LinxTileCubeDimensions linx_tile_cube_dimensions_058(const CPULinxState *env)
{
    if (cube_is_tmatmul_family(env)) {
        return (LinxTileCubeDimensions) {
            .m = cube_dimension(env->lb[0]),
            .n = cube_dimension(env->lb[1]),
            .k = cube_dimension(env->lb[2]),
        };
    }
    return (LinxTileCubeDimensions) {
        .m = cube_dimension(env->lb[1]),
        .n = cube_dimension(env->lb[0]),
        .k = cube_dimension(env->lb[2]),
    };
}

static bool cube_fpatr_output_dtype(unsigned prequant, uint32_t *dtype)
{
    switch (prequant) {
    case 0u: case 27u: case 38u: *dtype = 1u; return true;
    case 1u: case 4u: case 5u: case 32u: case 33u:
        *dtype = 4u; return true;
    case 2u: case 3u: case 23u: case 24u:
        *dtype = 19u; return true;
    case 12u: case 13u: case 19u: case 20u:
        *dtype = 18u; return true;
    case 16u: case 34u: case 35u: case 36u: case 39u:
        *dtype = 5u; return true;
    case 17u: case 18u: *dtype = 20u; return true;
    case 25u: case 28u: *dtype = 6u; return true;
    case 26u: case 37u: *dtype = 7u; return true;
    default: return false;
    }
}

static bool cube_accumulator_output_dtype(uint32_t selected_dtype,
                                          uint32_t *dtype)
{
    selected_dtype &= 31u;
    if (selected_dtype == 0u) {
        *dtype = 0u; /* FP64 */
        return true;
    }
    if (selected_dtype >= 1u && selected_dtype <= 14u &&
        selected_dtype != 13u) {
        *dtype = 1u; /* FP32 */
        return true;
    }
    if (selected_dtype >= 16u && selected_dtype <= 20u) {
        *dtype = 17u; /* S32 */
        return true;
    }
    if (selected_dtype >= 24u && selected_dtype <= 28u) {
        *dtype = 25u; /* U32 */
        return true;
    }
    return false;
}

static bool cube_output_dtype(const CPULinxState *env, unsigned ordinal,
                              uint32_t *dtype)
{
    const unsigned prequant = (env->tile_fpatr_raw >> 26) & 0x3fu;

    if (ordinal == 0u && env->tile_fpatr_valid && prequant != 0u) {
        return cube_fpatr_output_dtype(prequant, dtype);
    }
    return cube_accumulator_output_dtype(env->tile_dtype, dtype);
}

bool linx_tile_cube_output_descriptor_058(
    const CPULinxState *env, unsigned ordinal, uint32_t bytes,
    uint32_t *dtype, uint32_t *valid_cols_out, uint32_t *valid_rows_out,
    uint32_t *cols_out, uint32_t *rows_out)
{
    const LinxTileCubeDimensions dims = linx_tile_cube_dimensions_058(env);
    const bool row_max = ((env->tile_fpatr_raw >> 18) & 1u) != 0u;
    const bool group_max = ((env->tile_fpatr_raw >> 17) & 1u) != 0u;
    const unsigned group_code = (env->tile_fpatr_raw >> 19) & 0xfu;
    const unsigned group_n = group_code == 0u ? 0u
                               : group_code == 1u ? 8u
                               : group_code == 2u ? 16u
                                                  : (group_code - 1u) * 16u;
    const bool auxiliary_row = ordinal > 0u && row_max &&
        (ordinal == 1u || !group_max);
    uint32_t valid_cols = dims.n;
    uint32_t valid_rows = dims.m;
    uint32_t cols = cube_is_tmatmul_family(env)
                        ? dims.n
                        : (env->lb[2] != 0u ? env->lb[2] : dims.n);
    uint32_t output_dtype;

    if (!cube_output_dtype(env, ordinal, &output_dtype)) {
        return false;
    }
    if (auxiliary_row) {
        valid_cols = 1u;
        cols = 1u;
    } else if (ordinal > 0u && group_max) {
        if (group_n == 0u) {
            return false;
        }
        valid_cols = (valid_cols + group_n - 1u) / group_n;
        cols = (cols + group_n - 1u) / group_n;
    }
    const unsigned elem_bytes = cube_dtype_bytes(output_dtype);
    const bool packed = output_dtype == 11u || output_dtype == 12u ||
                        output_dtype == 14u || output_dtype == 20u ||
                        output_dtype == 28u;
    const uint32_t row_bytes = packed ? (cols + 1u) / 2u
                                      : cols * elem_bytes;

    if (dtype == NULL || valid_cols_out == NULL || valid_rows_out == NULL ||
        cols_out == NULL || rows_out == NULL || elem_bytes == 0u ||
        bytes == 0u || row_bytes == 0u || bytes % row_bytes != 0u) {
        return false;
    }
    const uint32_t rows = bytes / row_bytes;
    if (valid_cols > cols || valid_rows > rows ||
        valid_cols > UINT16_MAX || valid_rows > UINT16_MAX ||
        cols > UINT16_MAX || rows > UINT16_MAX) {
        return false;
    }
    *dtype = output_dtype;
    *valid_cols_out = valid_cols;
    *valid_rows_out = valid_rows;
    *cols_out = cols;
    *rows_out = rows;
    return true;
}

bool linx_tile_cube_group_dimensions_legal_058(const CPULinxState *env)
{
    LinxTileCubeDimensions dims = linx_tile_cube_dimensions_058(env);

    return dims.m != 0u && dims.n != 0u && dims.k != 0u;
}

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

static bool cube_primary_dtype_legal(const CPULinxState *env,
                                     uint32_t left_dtype,
                                     uint32_t right_dtype, bool mx)
{
    const uint32_t datr_dtype = env->tile_attr_dtype & 31u;
    const uint32_t selected_right =
        (env->tile_attr_dtype & 0x100u) != 0u && datr_dtype != 31u
            ? datr_dtype : env->tile_dtype & 31u;

    return mx ? (left_dtype == (env->tile_dtype & 31u) &&
                 right_dtype == selected_right &&
                 linx_tile_numeric_mx_pair(left_dtype, right_dtype))
              : (left_dtype == (env->tile_dtype & 31u) &&
                 right_dtype == selected_right &&
                 linx_tile_numeric_ordinary_matrix_pair(left_dtype,
                                                        right_dtype));
}

bool linx_tile_cube_primary_legal_058(const CPULinxState *env,
                                      unsigned src_a, unsigned src_b,
                                      bool mx, bool accumulate)
{
    LinxTileCubeDimensions dims = linx_tile_cube_dimensions_058(env);
    uint32_t left_dtype = src_a < LINX_TILE_SLOT_COUNT
                              ? env->tile_reg_dtype[src_a] & 31u
                              : UINT32_MAX;
    uint32_t right_dtype = src_b < LINX_TILE_SLOT_COUNT
                               ? env->tile_reg_dtype[src_b] & 31u
                               : UINT32_MAX;
    uint8_t acc_dtype = mx ? LINX_TILE_ACC_FP32
                           : linx_tile_numeric_acc_dtype(env->tile_dtype);
    const uint32_t left_layout = src_a < LINX_TILE_SLOT_COUNT
                                     ? env->tile_reg_layout[src_a]
                                     : UINT32_MAX;
    const uint32_t right_layout = src_b < LINX_TILE_SLOT_COUNT
                                      ? env->tile_reg_layout[src_b]
                                      : UINT32_MAX;
    const bool left_layout_legal =
        (left_layout == LINX_TILE_LAYOUT_CUBE_M16 && dims.m <= 16u) ||
        (left_layout == LINX_TILE_LAYOUT_CUBE_M32 && dims.m <= 32u);

    if (!cube_is_tmatmul_family(env) && src_a < LINX_TILE_SLOT_COUNT) {
        /* TGEMV retains its source-descriptor inner dimension. */
        dims.k = env->tile_reg_valid_cols[src_a];
    }

    if (src_a >= LINX_TILE_SLOT_COUNT || src_b >= LINX_TILE_SLOT_COUNT ||
        dims.m == 0u || dims.n == 0u || dims.k == 0u ||
        !left_layout_legal || right_layout != LINX_TILE_LAYOUT_CUBE_N8 ||
        env->tile_reg_cube_storage_bytes[src_a] == 0u ||
        env->tile_reg_cube_storage_bytes[src_b] == 0u ||
        env->tile_reg_valid_rows[src_a] != dims.m ||
        env->tile_reg_valid_cols[src_a] != dims.k ||
        env->tile_reg_valid_rows[src_b] != dims.k ||
        env->tile_reg_valid_cols[src_b] != dims.n ||
        !cube_operand_legal(env, src_a, left_dtype, dims.m, dims.k) ||
        !cube_operand_legal(env, src_b, right_dtype, dims.k, dims.n) ||
        !cube_primary_dtype_legal(env, left_dtype, right_dtype, mx)) {
        return false;
    }
    return !accumulate ||
           (env->tile_acc_valid && env->tile_acc_dtype == acc_dtype &&
            env->tile_acc_rows == dims.m && env->tile_acc_cols == dims.n);
}

static bool cube_read_raw(const CPULinxState *env, unsigned tile, unsigned row,
                          unsigned column, uint64_t *raw,
                          unsigned *packed_lane_out)
{
    uint32_t dtype = env->tile_reg_dtype[tile] & 31u;
    unsigned elem_bytes = cube_dtype_bytes(dtype);
    unsigned stride = env->tile_reg_cols[tile];
    uint64_t lane, offset;
    uint32_t cube_index;

    if (tile >= LINX_TILE_SLOT_COUNT || elem_bytes == 0u ||
        row >= env->tile_reg_rows[tile] ||
        column >= stride) {
        return false;
    }
    if (cube_layout(env->tile_reg_layout[tile])) {
        if (!linx_tile_cube_payload_index_058(env, tile, row, column,
                                              &cube_index)) {
            return false;
        }
        lane = linx_tile_numeric_is_packed(dtype) ? cube_index / 2u
                                                   : cube_index;
        if (packed_lane_out != NULL) {
            *packed_lane_out = cube_index;
        }
    } else {
        lane = linx_tile_numeric_is_packed(dtype)
               ? (uint64_t)row * ((stride + 1u) / 2u) + column / 2u
               : (uint64_t)row * stride + column;
        if (packed_lane_out != NULL) {
            *packed_lane_out = column;
        }
    }
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
                                      unsigned cols, unsigned stride)
{
    unsigned elem_bytes = cube_dtype_bytes(dtype);
    uint64_t row_bytes, required;

    if (data == NULL || elem_bytes == 0u || rows == 0u || cols == 0u ||
        stride < cols) {
        return false;
    }
    row_bytes = linx_tile_numeric_is_packed(dtype) ? (stride + 1u) / 2u
                                                    : (uint64_t)stride * elem_bytes;
    required = (uint64_t)(rows - 1u) * row_bytes +
               (linx_tile_numeric_is_packed(dtype) ? (cols + 1u) / 2u
                                                    : (uint64_t)cols * elem_bytes);
    return required <= bytes;
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

static bool cube_shared_payload_index(uint32_t layout, uint32_t dtype,
                                      unsigned valid_rows, unsigned valid_cols,
                                      unsigned storage_cols, unsigned row,
                                      unsigned column, uint32_t *index)
{
    uint32_t cell_rows, cell_cols;
    uint32_t cell_index, inner_row, inner_col;
    uint32_t storage_rows;
    const unsigned bits = cube_dtype_bits(dtype);

    if (index == NULL || row >= valid_rows || column >= valid_cols ||
        !cube_cell_geometry(layout, dtype, &cell_rows, &cell_cols) ||
        storage_cols == 0u) {
        return false;
    }
    if (layout == LINX_TILE_LAYOUT_CUBE_N8) {
        storage_rows = ROUND_UP(valid_rows, cell_rows);
        if (storage_cols < 8u) {
            return false;
        }
        cell_index = (column / cell_cols) * (storage_rows / cell_rows) +
                     row / cell_rows;
        inner_row = row % cell_rows;
        inner_col = column % cell_cols;
        *index = cell_index * cell_rows * cell_cols +
                 inner_col * cell_rows + inner_row;
    } else {
        if (storage_cols < ROUND_UP(valid_cols, cell_cols)) {
            return false;
        }
        cell_index = column / cell_cols;
        inner_row = row;
        inner_col = column % cell_cols;
        if (layout == LINX_TILE_LAYOUT_CUBE_M16 && bits == 4u) {
            inner_col = inner_col < 4u ? inner_col
                        : inner_col < 8u ? inner_col + 4u
                        : inner_col < 12u ? inner_col - 4u : inner_col;
        }
        *index = cell_index * cell_rows * cell_cols +
                 inner_row * cell_cols + inner_col;
    }
    return true;
}

static bool cube_read_shared_matrix_raw(const uint8_t *data, uint32_t bytes,
                                        uint32_t dtype, unsigned stride,
                                        uint32_t layout, unsigned valid_rows,
                                        unsigned valid_cols,
                                        unsigned logical_row,
                                        unsigned logical_column,
                                        bool transpose, uint64_t *raw)
{
    const unsigned row = transpose ? logical_column : logical_row;
    const unsigned column = transpose ? logical_row : logical_column;
    const unsigned elem_bytes = cube_dtype_bytes(dtype);
    uint32_t index;

    if (cube_layout(layout)) {
        if (!cube_shared_payload_index(layout, dtype, valid_rows, valid_cols,
                                       stride, row, column, &index)) {
            return false;
        }
        const uint64_t offset = linx_tile_numeric_is_packed(dtype)
                                    ? index / 2u
                                    : (uint64_t)index * elem_bytes;
        if (offset + elem_bytes > bytes) {
            return false;
        }
        *raw = 0u;
        memcpy(raw, data + offset, elem_bytes);
        return true;
    }
    return cube_read_buffer_raw(data, bytes, dtype, stride, row, column, raw);
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

static uint64_t cube_unsigned(uint32_t dtype, uint64_t raw,
                              unsigned logical_lane)
{
    switch (dtype & 31u) {
    case 24u:
        return raw;
    case 25u:
        return (uint32_t)raw;
    case 26u:
        return (uint16_t)raw;
    case 27u:
        return (uint8_t)raw;
    case 28u:
        return linx_tile_numeric_nibble(raw, logical_lane);
    default:
        return 0u;
    }
}

static bool linx_tile_cube_compute_common_058(
    CPULinxState *env, unsigned src_a, unsigned src_b,
    const uint8_t *shared_a, uint32_t shared_a_bytes,
    uint32_t shared_a_dtype, uint32_t shared_a_cols,
    const uint8_t *shared_b, uint32_t shared_b_bytes,
    uint32_t shared_b_dtype, uint32_t shared_b_cols,
    const uint8_t *shared_row_scale, uint32_t shared_row_scale_bytes,
    uint32_t shared_row_scale_cols,
    const uint8_t *shared_column_scale, uint32_t shared_column_scale_bytes,
    uint32_t shared_column_scale_cols,
    unsigned row_scale, unsigned column_scale,
    unsigned bias, unsigned size_code, bool mx, bool with_bias,
    bool accumulate)
{
    LinxTileCubeDimensions dims = linx_tile_cube_dimensions_058(env);
    unsigned m = dims.m;
    unsigned n = dims.n;
    unsigned kdim = dims.k;
    uint32_t left_dtype = shared_a != NULL
                              ? shared_a_dtype & 31u
                              : src_a < LINX_TILE_SLOT_COUNT
                                    ? env->tile_reg_dtype[src_a] & 31u
                                    : UINT32_MAX;
    uint32_t right_dtype = shared_b != NULL
                               ? shared_b_dtype & 31u
                               : src_b < LINX_TILE_SLOT_COUNT
                                     ? env->tile_reg_dtype[src_b] & 31u
                                     : UINT32_MAX;
    uint8_t acc_dtype =
        mx ? LINX_TILE_ACC_FP32 : linx_tile_numeric_acc_dtype(env->tile_dtype);
    const uint32_t bias_dtype = acc_dtype == LINX_TILE_ACC_S64 ? 17u
                                : acc_dtype == LINX_TILE_ACC_U64 ? 25u
                                                                : acc_dtype;
    const bool scale_left = mx &&
        linx_tile_numeric_mx_requires_scale(left_dtype);
    const bool scale_right = mx &&
        linx_tile_numeric_mx_requires_scale(right_dtype);
    unsigned acc_bytes = acc_dtype == LINX_TILE_ACC_FP32 ? 4u : 8u;
    uint64_t allocated = size_code < 60u ? UINT64_C(1) << (size_code + 4u) : 0u;
    const unsigned output_m = m;
    const unsigned row_base = shared_a != NULL && shared_b != NULL && !mx
        ? env->pe_id * output_m : 0u;
    const bool transpose_a = ((env->tile_fpatr_raw >> 7) & 1u) != 0u;
    const bool transpose_b = ((env->tile_fpatr_raw >> 8) & 1u) != 0u;
    const unsigned shared_a_rows = transpose_a
        ? kdim : (mx ? m : m * LINX_CORE4_PE_COUNT);
    const unsigned shared_a_logical_cols = transpose_a
        ? (mx ? m : m * LINX_CORE4_PE_COUNT) : kdim;
    const unsigned shared_b_rows = transpose_b ? n : kdim;
    const unsigned shared_b_logical_cols = transpose_b ? kdim : n;
    uint64_t required = (uint64_t)output_m * n * acc_bytes;
    unsigned groups = (kdim + 31u) / 32u;
    float_status fp_status = {0};
    uint8_t *next;

    if (!cube_is_tmatmul_family(env) && src_a < LINX_TILE_SLOT_COUNT) {
        /* TGEMV retains its source-descriptor inner dimension. */
        dims.k = env->tile_reg_valid_cols[src_a];
        kdim = dims.k;
    }

    if ((shared_a == NULL && src_a >= LINX_TILE_SLOT_COUNT) ||
        (transpose_a && shared_a == NULL) ||
        (transpose_b && shared_b == NULL) ||
        (shared_a == NULL && shared_b == NULL &&
         !linx_tile_cube_primary_legal_058(env, src_a, src_b, mx,
                                           accumulate)) ||
        m > UINT16_MAX || n > UINT16_MAX ||
        acc_dtype == UINT8_MAX || allocated == 0u ||
        allocated > LINX_TILE_MAX_BYTES || required == 0u ||
        required > sizeof(env->tile_acc) ||
        (shared_b != NULL &&
         (m == 0u || n == 0u || kdim == 0u ||
          (shared_a != NULL
               ? !cube_buffer_operand_legal(shared_a, shared_a_bytes,
                                            left_dtype, shared_a_rows,
                                            shared_a_logical_cols,
                                            shared_a_cols)
               : env->tile_reg_valid_rows[src_a] != m ||
                     env->tile_reg_valid_cols[src_a] != kdim ||
                     !cube_operand_legal(env, src_a, left_dtype, m, kdim)) ||
          !cube_buffer_operand_legal(shared_b, shared_b_bytes,
                                     right_dtype, shared_b_rows,
                                     shared_b_logical_cols,
                                     shared_b_cols) ||
          !cube_primary_dtype_legal(env, left_dtype, right_dtype, mx))) ||
        (scale_left &&
         (shared_row_scale != NULL
              ? !cube_buffer_operand_legal(shared_row_scale,
                                           shared_row_scale_bytes, 13u,
                                           shared_a_rows, groups,
                                           shared_row_scale_cols)
              : !cube_operand_legal(env, row_scale, 13u, m, groups))) ||
        (scale_right &&
         (shared_column_scale != NULL
              ? !cube_buffer_operand_legal(shared_column_scale,
                                           shared_column_scale_bytes, 13u,
                                           groups, n,
                                           shared_column_scale_cols)
              : !cube_operand_legal(env, column_scale, 13u, groups, n))) ||
        (with_bias &&
         (bias >= LINX_TILE_SLOT_COUNT ||
          (env->tile_reg_valid_rows[bias] != 1u &&
           env->tile_reg_valid_rows[bias] != m) ||
          (env->tile_reg_valid_cols[bias] != 1u &&
           env->tile_reg_valid_cols[bias] != n) ||
          !cube_operand_legal(env, bias, bias_dtype,
                              env->tile_reg_valid_rows[bias],
                              env->tile_reg_valid_cols[bias]))) ||
        (accumulate && env->tile_acc_bytes < required) ||
        (shared_b != NULL && accumulate &&
         (!env->tile_acc_valid || env->tile_acc_dtype != acc_dtype ||
          env->tile_acc_rows != output_m || env->tile_acc_cols != n))) {
        return false;
    }

    set_float_rounding_mode(float_round_nearest_even, &fp_status);
    set_default_nan_mode(true, &fp_status);
    set_float_default_nan_pattern(0b01000000, &fp_status);
    next = g_malloc0(required);
    if (accumulate) {
        memcpy(next, env->tile_acc, required);
    }
    for (unsigned i = 0; i < output_m; i++) {
        const unsigned source_row = row_base + i;
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
                    unsigned a_lane = k, b_lane = j;
                    if ((shared_a != NULL
                             ? !cube_read_shared_matrix_raw(
                                   shared_a, shared_a_bytes, left_dtype,
                                   shared_a_cols,
                                   m <= 16u ? LINX_TILE_LAYOUT_CUBE_M16
                                             : LINX_TILE_LAYOUT_CUBE_M32,
                                   shared_a_rows, shared_a_logical_cols,
                                   source_row, k, transpose_a, &ar)
                             : !cube_read_raw(env, src_a, i, k, &ar,
                                              &a_lane)) ||
                        (shared_b != NULL
                             ? !cube_read_shared_matrix_raw(
                                   shared_b, shared_b_bytes, right_dtype,
                                   shared_b_cols, LINX_TILE_LAYOUT_CUBE_N8,
                                   shared_b_rows, shared_b_logical_cols,
                                   k, j, transpose_b, &br)
                             : !cube_read_raw(env, src_b, k, j, &br,
                                              &b_lane))) {
                        goto fail;
                    }
                    av = linx_tile_numeric_f32_raw(
                        linx_tile_numeric_decode(left_dtype, ar, a_lane));
                    bv = linx_tile_numeric_f32_raw(
                        linx_tile_numeric_decode(right_dtype, br, b_lane));
                    if (mx) {
                        uint64_t sar, sbr;
                        uint32_t scale_a = linx_tile_numeric_f32_raw(1.0);
                        uint32_t scale_b = linx_tile_numeric_f32_raw(1.0);
                        if (scale_left) {
                            if (shared_row_scale != NULL
                                    ? !cube_read_buffer_raw(
                                          shared_row_scale,
                                          shared_row_scale_bytes, 13u,
                                          shared_row_scale_cols, source_row,
                                          k / 32u, &sar)
                                    : !cube_read_raw(env, row_scale, i,
                                                     k / 32u, &sar, NULL)) {
                                goto fail;
                            }
                            scale_a = linx_tile_numeric_f32_raw(
                                linx_tile_numeric_decode(13u, sar, 0));
                        }
                        if (scale_right) {
                            if (shared_column_scale != NULL
                                    ? !cube_read_buffer_raw(
                                          shared_column_scale,
                                          shared_column_scale_bytes, 13u,
                                          shared_column_scale_cols,
                                          k / 32u, j, &sbr)
                                    : !cube_read_raw(env, column_scale,
                                                     k / 32u, j, &sbr, NULL)) {
                                goto fail;
                            }
                            scale_b = linx_tile_numeric_f32_raw(
                                linx_tile_numeric_decode(13u, sbr, 0));
                        }
                        av = float32_mul(av, scale_a, &fp_status);
                        bv = float32_mul(bv, scale_b, &fp_status);
                    }
                    acc = float32_muladd(av, bv, acc, 0, &fp_status);
                }
                if (with_bias) {
                    uint64_t raw;
                    uint32_t bias_raw;
                    const unsigned bias_row =
                        env->tile_reg_valid_rows[bias] == 1u ? 0u : i;
                    const unsigned bias_col =
                        env->tile_reg_valid_cols[bias] == 1u ? 0u : j;
                    if (!cube_read_raw(env, bias, bias_row, bias_col, &raw,
                                       NULL)) {
                        goto fail;
                    }
                    bias_raw =
                        linx_tile_numeric_f32_raw(linx_tile_numeric_decode(
                            bias_dtype, raw, j));
                    acc = float32_add(acc, bias_raw, &fp_status);
                }
                memcpy(next + index * 4u, &acc, 4u);
            } else if (acc_dtype == LINX_TILE_ACC_FP64) {
                uint64_t acc = 0u;
                if (accumulate) {
                    memcpy(&acc, next + index * 8u, 8u);
                }
                for (unsigned k = 0; k < kdim; k++) {
                    uint64_t ar, br;
                    unsigned a_lane = k, b_lane = j;
                    if ((shared_a != NULL
                             ? !cube_read_shared_matrix_raw(
                                   shared_a, shared_a_bytes, left_dtype,
                                   shared_a_cols,
                                   m <= 16u ? LINX_TILE_LAYOUT_CUBE_M16
                                             : LINX_TILE_LAYOUT_CUBE_M32,
                                   shared_a_rows, shared_a_logical_cols,
                                   source_row, k, transpose_a, &ar)
                             : !cube_read_raw(env, src_a, i, k, &ar,
                                              &a_lane)) ||
                        (shared_b != NULL
                             ? !cube_read_shared_matrix_raw(
                                   shared_b, shared_b_bytes, right_dtype,
                                   shared_b_cols, LINX_TILE_LAYOUT_CUBE_N8,
                                   shared_b_rows, shared_b_logical_cols,
                                   k, j, transpose_b, &br)
                             : !cube_read_raw(env, src_b, k, j, &br,
                                              &b_lane))) {
                        goto fail;
                    }
                    uint64_t av = linx_tile_numeric_f64_raw(
                        linx_tile_numeric_decode(left_dtype, ar, a_lane));
                    uint64_t bv = linx_tile_numeric_f64_raw(
                        linx_tile_numeric_decode(right_dtype, br, b_lane));
                    acc = float64_muladd(av, bv, acc, 0, &fp_status);
                }
                if (with_bias) {
                    uint64_t raw;
                    const unsigned bias_row =
                        env->tile_reg_valid_rows[bias] == 1u ? 0u : i;
                    const unsigned bias_col =
                        env->tile_reg_valid_cols[bias] == 1u ? 0u : j;
                    if (!cube_read_raw(env, bias, bias_row, bias_col, &raw,
                                       NULL)) {
                        goto fail;
                    }
                    uint64_t bias_raw = linx_tile_numeric_f64_raw(
                        linx_tile_numeric_decode(bias_dtype, raw, j));
                    acc = float64_add(acc, bias_raw, &fp_status);
                }
                memcpy(next + index * 8u, &acc, 8u);
            } else {
                uint64_t acc = 0u;
                if (accumulate) {
                    memcpy(&acc, next + index * 8u, 8u);
                }
                for (unsigned k = 0; k < kdim; k++) {
                    uint64_t ar, br;
                    unsigned a_lane = k, b_lane = j;
                    if ((shared_a != NULL
                             ? !cube_read_shared_matrix_raw(
                                   shared_a, shared_a_bytes, left_dtype,
                                   shared_a_cols,
                                   m <= 16u ? LINX_TILE_LAYOUT_CUBE_M16
                                             : LINX_TILE_LAYOUT_CUBE_M32,
                                   shared_a_rows, shared_a_logical_cols,
                                   source_row, k, transpose_a, &ar)
                             : !cube_read_raw(env, src_a, i, k, &ar,
                                              &a_lane)) ||
                        (shared_b != NULL
                             ? !cube_read_shared_matrix_raw(
                                   shared_b, shared_b_bytes, right_dtype,
                                   shared_b_cols, LINX_TILE_LAYOUT_CUBE_N8,
                                   shared_b_rows, shared_b_logical_cols,
                                   k, j, transpose_b, &br)
                             : !cube_read_raw(env, src_b, k, j, &br,
                                              &b_lane))) {
                        goto fail;
                    }
                    acc += acc_dtype == LINX_TILE_ACC_S64
                               ? (uint64_t)cube_signed(left_dtype, ar, a_lane) *
                                     (uint64_t)cube_signed(right_dtype, br,
                                                           b_lane)
                               : cube_unsigned(left_dtype, ar, a_lane) *
                                     cube_unsigned(right_dtype, br, b_lane);
                }
                if (with_bias) {
                    uint64_t raw;
                    if (!cube_read_raw(env, bias, 0, j, &raw, NULL)) {
                        goto fail;
                    }
                    acc += acc_dtype == LINX_TILE_ACC_S64
                               ? (uint64_t)cube_signed(bias_dtype, raw, j)
                               : cube_unsigned(bias_dtype, raw, j);
                }
                memcpy(next + index * 8u, &acc, 8u);
            }
        }
    }
    memset(env->tile_acc, 0, sizeof(env->tile_acc));
    memcpy(env->tile_acc, next, required);
    env->tile_acc_bytes = required;
    env->tile_acc_dtype = acc_dtype;
    env->tile_acc_valid = 1u;
    env->tile_acc_cols = n;
    env->tile_acc_rows = output_m;
    g_free(next);
    return true;
fail:
    g_free(next);
    return false;
}

bool linx_tile_cube_compute_058(CPULinxState *env, unsigned src_a,
                                unsigned src_b, unsigned row_scale,
                                unsigned column_scale, unsigned bias,
                                unsigned size_code, bool mx, bool with_bias,
                                bool accumulate)
{
    return linx_tile_cube_compute_common_058(
        env, src_a, src_b, NULL, 0u, 0u, 0u, NULL, 0u, 0u, 0u,
        NULL, 0u, 0u, NULL, 0u, 0u,
        row_scale, column_scale, bias,
        size_code, mx, with_bias, accumulate);
}

bool linx_tile_cube_compute_shared_b_058(
    CPULinxState *env, unsigned src_a, const uint8_t *shared_b,
    uint32_t shared_b_bytes, uint32_t shared_b_dtype, uint32_t shared_b_cols,
    unsigned size_code, bool accumulate)
{
    return linx_tile_cube_compute_common_058(
        env, src_a, UINT_MAX, NULL, 0u, 0u, 0u, shared_b, shared_b_bytes,
        shared_b_dtype, shared_b_cols, NULL, 0u, 0u, NULL, 0u, 0u,
        0u, 0u, 0u, size_code, false, false,
        accumulate);
}

bool linx_tile_cube_compute_shared_ab_058(
    CPULinxState *env, const uint8_t *shared_a, uint32_t shared_a_bytes,
    uint32_t shared_a_dtype, uint32_t shared_a_cols,
    const uint8_t *shared_b, uint32_t shared_b_bytes,
    uint32_t shared_b_dtype, uint32_t shared_b_cols,
    unsigned size_code, bool accumulate)
{
    return linx_tile_cube_compute_common_058(
        env, UINT_MAX, UINT_MAX, shared_a, shared_a_bytes, shared_a_dtype,
        shared_a_cols, shared_b, shared_b_bytes, shared_b_dtype, shared_b_cols,
        NULL, 0u, 0u, NULL, 0u, 0u,
        0u, 0u, 0u, size_code, false, false, accumulate);
}

bool linx_tile_cube_compute_shared_058(
    CPULinxState *env, unsigned src_a,
    const uint8_t *shared_a, uint32_t shared_a_bytes,
    uint32_t shared_a_dtype, uint32_t shared_a_cols,
    const uint8_t *shared_b, uint32_t shared_b_bytes,
    uint32_t shared_b_dtype, uint32_t shared_b_cols,
    const uint8_t *shared_row_scale, uint32_t shared_row_scale_bytes,
    uint32_t shared_row_scale_cols,
    const uint8_t *shared_column_scale, uint32_t shared_column_scale_bytes,
    uint32_t shared_column_scale_cols,
    unsigned row_scale, unsigned column_scale, unsigned bias,
    unsigned size_code, bool mx, bool with_bias, bool accumulate)
{
    return linx_tile_cube_compute_common_058(
        env, src_a, UINT_MAX, shared_a, shared_a_bytes, shared_a_dtype,
        shared_a_cols, shared_b, shared_b_bytes, shared_b_dtype,
        shared_b_cols, shared_row_scale, shared_row_scale_bytes,
        shared_row_scale_cols, shared_column_scale,
        shared_column_scale_bytes, shared_column_scale_cols,
        row_scale, column_scale, bias, size_code, mx,
        with_bias, accumulate);
}

static uint64_t cube_saturate_integer(uint64_t raw, uint8_t acc_dtype,
                                      uint32_t dst_dtype)
{
    bool dst_signed = dst_dtype >= 16u && dst_dtype <= 20u;
    unsigned bits = dst_dtype == 16u || dst_dtype == 24u   ? 64u
                    : dst_dtype == 17u || dst_dtype == 25u ? 32u
                    : dst_dtype == 18u || dst_dtype == 26u ? 16u
                    : dst_dtype == 19u || dst_dtype == 27u ? 8u
                                                           : 4u;
    uint64_t umax = bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
    uint64_t smax = bits == 64u ? INT64_MAX : (UINT64_C(1) << (bits - 1u)) - 1u;
    int64_t sval = raw;

    if (acc_dtype == LINX_TILE_ACC_S64) {
        if (dst_signed) {
            int64_t smin =
                bits == 64u ? INT64_MIN : -(INT64_C(1) << (bits - 1u));
            return sval < smin ? (uint64_t)smin
                               : (uint64_t)(sval > (int64_t)smax ? (int64_t)smax
                                                                 : sval);
        }
        return sval < 0 ? 0u : (uint64_t)sval > umax ? umax : sval;
    }
    return dst_signed ? (raw > smax ? smax : raw) : (raw > umax ? umax : raw);
}

static bool cube_resolve_ior(const CPULinxState *env, unsigned slot,
                             uint64_t *value)
{
    static const unsigned shifts[] = { 5u, 10u, 15u, 0u };
    unsigned current = 0u;

    for (unsigned i = 0; i < env->tile_ior_count; i++) {
        for (unsigned j = 0; j < ARRAY_SIZE(shifts); j++) {
            const unsigned reg = (env->tile_ior_desc[i] >> shifts[j]) & 0x1fu;
            if (reg == 0u) {
                continue;
            }
            if (current++ == slot) {
                if (reg >= LINX_GPR_COUNT) {
                    return false;
                }
                *value = env->gpr[reg];
                return true;
            }
        }
    }
    return false;
}

static bool cube_fp19(uint32_t raw, bool scale, double *value)
{
    const bool negative = ((raw >> 18) & 1u) != 0u;
    const unsigned exponent = (raw >> 10) & 0xffu;
    const unsigned fraction = raw & 0x3ffu;

    if (exponent == 0xffu || (scale && (negative || exponent == 0u)) ||
        (!scale && negative)) {
        return false;
    }
    *value = exponent == 0u
                 ? ldexp((double)fraction, -136)
                 : ldexp(1.0 + (double)fraction / 1024.0,
                         (int)exponent - 127);
    return true;
}

bool linx_tile_fpatr_mode_uses_vector_parameter_058(unsigned mode)
{
    switch (mode) {
    case 2u: case 4u: case 12u: case 18u: case 20u: case 23u:
    case 28u: case 33u: case 36u: case 37u: case 38u: case 39u:
        return true;
    default:
        return false;
    }
}

bool linx_tile_fpatr_mode_uses_scalar_parameter_058(unsigned mode)
{
    switch (mode) {
    case 3u: case 5u: case 13u: case 17u: case 19u: case 24u:
    case 25u: case 26u: case 27u: case 32u: case 34u: case 35u:
        return true;
    default:
        return false;
    }
}

bool linx_tile_fpatr_mode_uses_s32_accumulator_058(unsigned mode)
{
    switch (mode) {
    case 2u: case 3u: case 4u: case 5u: case 12u: case 13u:
    case 17u: case 18u: case 19u: case 20u: case 35u: case 39u:
        return true;
    default:
        return false;
    }
}

static unsigned cube_fpatr_offset_width(unsigned mode)
{
    if (mode == 17u || mode == 18u) {
        return 5u;
    }
    if (mode == 2u || mode == 3u || mode == 23u || mode == 24u) {
        return 9u;
    }
    if (mode == 19u || mode == 20u) {
        return 17u;
    }
    return 0u;
}

static bool cube_fpatr_mode_is_shift(unsigned mode)
{
    return mode == 12u || mode == 13u;
}

static bool cube_fpatr_fixed_rounding(unsigned mode)
{
    switch (mode) {
    case 1u: case 16u: case 25u: case 26u: case 28u: case 32u:
    case 33u: case 34u: case 36u: case 37u:
        return true;
    default:
        return false;
    }
}

bool linx_tile_fpatr_quant_parameter_legal_058(unsigned mode, uint64_t value)
{
    double scale;
    if (cube_fpatr_mode_is_shift(mode)) {
        return (value & UINT64_C(0xffffffff)) == 0u &&
               (value >> 36) == 0u;
    }
    if (!linx_tile_fpatr_mode_uses_scalar_parameter_058(mode) &&
        !linx_tile_fpatr_mode_uses_vector_parameter_058(mode)) {
        return false;
    }
    if ((value & UINT64_C(0x1fff)) != 0u ||
        !cube_fp19((value >> 13) & 0x7ffffu, true, &scale)) {
        return false;
    }
    switch (cube_fpatr_offset_width(mode)) {
    case 5u:
        return ((value >> 32) & 0x1fu) == 0u && (value >> 42) == 0u;
    case 9u:
        return ((value >> 32) & 0x1fu) == 0u && (value >> 46) == 0u;
    case 17u:
        return ((value >> 32) & 0x1fu) == 0u && (value >> 54) == 0u;
    default:
        return (value >> 32) == 0u;
    }
}

bool linx_tile_fpatr_relu_parameter_legal_058(uint64_t value)
{
    double parameter;
    return (value >> 19) == 0u &&
           cube_fp19(value & 0x7ffffu, false, &parameter);
}

bool linx_tile_fpatr_datr_legal_058(unsigned mode, unsigned rmode, bool sat)
{
    if (mode == 0u) {
        return rmode == 0u && !sat;
    }
    if (cube_fpatr_mode_is_shift(mode)) {
        return rmode == 0u && !sat;
    }
    if (cube_fpatr_fixed_rounding(mode)) {
        return rmode == 0u;
    }
    return true;
}

static int64_t cube_sign_extend(uint64_t value, unsigned bits)
{
    const uint64_t sign = UINT64_C(1) << (bits - 1u);
    const uint64_t mask = (UINT64_C(1) << bits) - 1u;
    value &= mask;
    return (int64_t)((value ^ sign) - sign);
}

static bool cube_fpatr_convert(CPULinxState *env, uint32_t dst_dtype,
                               uint64_t src_raw, double value,
                               unsigned rmode, bool sat,
                               uint64_t quant_param, uint64_t relu_param,
                               uint64_t *dst_raw)
{
    const unsigned mode = (env->tile_fpatr_raw >> 26) & 0x3fu;
    const unsigned relu = (env->tile_fpatr_raw >> 23) & 0x7u;
    double scale = 1.0, multiplier = 1.0;
    int64_t offset = 0;
    unsigned offset_width = cube_fpatr_offset_width(mode);

    if (cube_fpatr_mode_is_shift(mode)) {
        const unsigned shift = ((quant_param >> 32) & 0xfu) + 1u;
        int64_t shifted = (int32_t)src_raw >> shift;
        if (shifted < INT16_MIN || shifted > INT16_MAX) {
            env->fcsr |= 0x14u;
            shifted = MIN((int64_t)INT16_MAX,
                          MAX((int64_t)INT16_MIN, shifted));
        }
        *dst_raw = (uint16_t)shifted;
        return true;
    }
    if (linx_tile_fpatr_mode_uses_scalar_parameter_058(mode) ||
        linx_tile_fpatr_mode_uses_vector_parameter_058(mode)) {
        if (!linx_tile_fpatr_quant_parameter_legal_058(mode, quant_param) ||
            !cube_fp19((quant_param >> 13) & 0x7ffffu, true, &scale)) {
            return false;
        }
        if (offset_width != 0u) {
            offset = cube_sign_extend(quant_param >> 37, offset_width);
        }
    }
    multiplier = scale;
    if (signbit(value) && relu == 1u) {
        multiplier = 0.0;
    } else if (signbit(value) && (relu == 2u || relu == 3u)) {
        if (!linx_tile_fpatr_relu_parameter_legal_058(relu_param) ||
            !cube_fp19(relu_param & 0x7ffffu, false, &multiplier)) {
            return false;
        }
    } else if (relu > 3u) {
        return false;
    }
    if (isnan(value) && (dst_dtype >= 16u && dst_dtype <= 28u)) {
        env->fcsr |= 1u;
        *dst_raw = sat ? 0u : linx_tile_numeric_float_to_integer(
                                     dst_dtype, value, rmode, false);
        return true;
    }
    if (isinf(value) && signbit(value) && multiplier == 0.0) {
        value = 0.0;
    } else {
        value *= multiplier;
    }
    if (offset_width != 0u) {
        const int64_t minimum = offset_width == 5u ? -16
                                : offset_width == 9u ? -256 : -65536;
        const int64_t maximum = offset_width == 5u ? 15
                                : offset_width == 9u ? 255 : 65535;
        double rounded = (double)linx_tile_numeric_round_s64(value, rmode);
        if (rounded < minimum || rounded > maximum) {
            env->fcsr |= 0x14u;
        } else if (rounded != value) {
            env->fcsr |= 0x10u;
        }
        rounded = MIN((double)maximum, MAX((double)minimum, rounded));
        value = rounded + offset;
    }
    if (dst_dtype >= 16u && dst_dtype <= 28u) {
        if (offset_width != 0u && dst_dtype == 19u) {
            const int64_t rounded = linx_tile_numeric_round_s64(value, rmode);
            if (rounded < INT8_MIN || rounded > INT8_MAX ||
                (double)rounded != value) {
                env->fcsr |= 0x14u;
            }
            *dst_raw = sat ? (uint8_t)MIN((int64_t)INT8_MAX,
                                          MAX((int64_t)INT8_MIN, rounded))
                           : (uint8_t)rounded;
        } else {
            *dst_raw = linx_tile_numeric_float_to_integer(dst_dtype, value,
                                                           rmode, sat);
        }
    } else {
        const unsigned effective_rmode = mode == 25u || mode == 28u
                                             ? 7u
                                             : cube_fpatr_fixed_rounding(mode)
                                                   ? 0u : rmode;
        *dst_raw = linx_tile_numeric_encode(dst_dtype, value,
                                             effective_rmode, sat);
    }
    (void)src_raw;
    return true;
}

static bool linx_tile_fpatr_postprocess(const CPULinxState *env,
                                        uint32_t dst_dtype, double *value)
{
    const uint32_t raw = env->tile_fpatr_raw;
    const unsigned prequant = (raw >> 26) & 0x3fu;
    const unsigned relu = (raw >> 23) & 0x7u;
    const unsigned group_n = (raw >> 19) & 0xfu;
    const bool row_max = ((raw >> 18) & 1u) != 0;
    const bool group_max = ((raw >> 17) & 1u) != 0;
    const bool row_init = ((raw >> 16) & 1u) != 0;
    const bool max_abs = ((raw >> 15) & 1u) != 0;
    uint32_t expected_dtype;

    if (!env->tile_fpatr_valid) {
        return true;
    }

    if ((!group_max && group_n != 0u) || (group_max && group_n == 0u) ||
        (!row_max && row_init) || (!row_max && !group_max && max_abs) ||
        relu > 3u || !linx_tile_fpatr_datr_legal_058(
            prequant, (env->tile_attr_raw >> 25) & 7u,
            ((env->tile_attr_raw >> 28) & 1u) != 0u)) {
        return false;
    }
    if (prequant != 0u) {
        if (!cube_fpatr_output_dtype(prequant, &expected_dtype) ||
            ((linx_tile_fpatr_mode_uses_s32_accumulator_058(prequant)
                 ? env->tile_acc_dtype != LINX_TILE_ACC_S64
                 : env->tile_acc_dtype != LINX_TILE_ACC_FP32) ||
            dst_dtype != expected_dtype)) {
            return false;
        }
    } else if (!cube_accumulator_output_dtype(env->tile_acc_dtype,
                                               &expected_dtype) ||
               dst_dtype != expected_dtype) {
        return false;
    }
    (void)value;
    return true;
}

static bool cube_fpatr_parameters(const CPULinxState *env, unsigned column,
                                  unsigned quant_tile, unsigned relu_tile,
                                  uint64_t *quant_param,
                                  uint64_t *relu_param)
{
    const unsigned mode = (env->tile_fpatr_raw >> 26) & 0x3fu;
    const unsigned relu = (env->tile_fpatr_raw >> 23) & 0x7u;
    const bool scalar_quant =
        linx_tile_fpatr_mode_uses_scalar_parameter_058(mode);

    *quant_param = 0u;
    *relu_param = 0u;
    if (linx_tile_fpatr_mode_uses_vector_parameter_058(mode)) {
        if (quant_tile >= LINX_TILE_SLOT_COUNT ||
            !cube_read_raw(env, quant_tile, 0u, column, quant_param, NULL)) {
            return false;
        }
    } else if (scalar_quant) {
        if (!cube_resolve_ior(env, 0u, quant_param)) {
            return false;
        }
    }
    if (relu == 3u) {
        if (relu_tile >= LINX_TILE_SLOT_COUNT ||
            !cube_read_raw(env, relu_tile, 0u, column, relu_param, NULL)) {
            return false;
        }
    } else if (relu == 2u) {
        if (!cube_resolve_ior(env, scalar_quant ? 1u : 0u, relu_param)) {
            return false;
        }
    }
    return true;
}

bool linx_tile_accumulator_convert_with_aux_058(
    CPULinxState *env, unsigned dst_tile, unsigned size_code,
    unsigned quant_tile, unsigned relu_tile)
{
    uint32_t dst_dtype = env->tile_dtype & 31u;
    if (env->tile_fpatr_valid &&
        !cube_output_dtype(env, 0u, &dst_dtype)) {
        return false;
    }
    unsigned elem_bytes = cube_dtype_bytes(dst_dtype);
    uint64_t bytes = size_code < 60u ? UINT64_C(1) << (size_code + 4u) : 0u;
    unsigned physical_cols = cube_is_tmatmul_family(env)
                                 ? env->tile_acc_cols
                                 : (env->lb[2] != 0u
                                        ? env->lb[2] : env->tile_acc_cols);
    uint64_t row_bytes = linx_tile_numeric_is_packed(dst_dtype)
                             ? (physical_cols + 1u) / 2u
                             : (uint64_t)physical_cols * elem_bytes;
    uint64_t used = (uint64_t)env->tile_acc_rows * row_bytes;
    unsigned src_bytes = env->tile_acc_dtype == LINX_TILE_ACC_FP32 ? 4u : 8u;
    bool sat = ((env->tile_attr_raw >> 28) & 1u) != 0u;
    unsigned rmode = (env->tile_attr_raw >> 25) & 7u;
    bool dst_integer = (dst_dtype >= 16u && dst_dtype <= 20u) ||
                       (dst_dtype >= 24u && dst_dtype <= 28u);
    uint8_t *next;

    if (dst_tile >= LINX_TILE_SLOT_COUNT ||
        !linx_tile_numeric_ordinary(dst_dtype) ||
        !env->tile_acc_valid || elem_bytes == 0u || bytes == 0u ||
        bytes > LINX_TILE_MAX_BYTES || used > bytes ||
        env->tile_reg_capacity[dst_tile] < bytes || env->tile_acc_cols == 0u ||
        env->tile_acc_rows == 0u) {
        return false;
    }
    {
        double probe = 0.0;
        if (!linx_tile_fpatr_postprocess(env, dst_dtype, &probe)) {
            return false;
        }
    }
    next = g_malloc0(bytes);
    for (unsigned i = 0; i < env->tile_acc_rows; i++) {
        for (unsigned j = 0; j < env->tile_acc_cols; j++) {
            uint64_t index = (uint64_t)i * env->tile_acc_cols + j;
            uint64_t src_raw = 0u, dst_raw, quant_param, relu_param;
            double value;

            memcpy(&src_raw, (uint8_t *)env->tile_acc + index * src_bytes,
                   src_bytes);
            value = linx_tile_numeric_decode(env->tile_acc_dtype, src_raw, 0);
            g_assert(linx_tile_fpatr_postprocess(env, dst_dtype, &value));
            if (env->tile_fpatr_valid &&
                (!cube_fpatr_parameters(env, j, quant_tile, relu_tile,
                                        &quant_param, &relu_param) ||
                !cube_fpatr_convert(env, dst_dtype, src_raw, value,
                                    rmode, sat, quant_param, relu_param,
                                    &dst_raw))) {
                g_free(next);
                return false;
            } else if (env->tile_fpatr_valid) {
                /* cube_fpatr_convert produced dst_raw. */
            } else if (dst_integer) {
                if (env->tile_acc_dtype == LINX_TILE_ACC_S64 ||
                    env->tile_acc_dtype == LINX_TILE_ACC_U64) {
                    dst_raw = sat ? cube_saturate_integer(
                                        src_raw, env->tile_acc_dtype, dst_dtype)
                                  : src_raw;
                } else {
                    dst_raw = linx_tile_numeric_float_to_integer(
                        dst_dtype, value, rmode, sat);
                }
            } else {
                dst_raw =
                    linx_tile_numeric_encode(dst_dtype, value, rmode, sat);
            }
            uint32_t payload_index = (uint32_t)index;
            if (cube_layout(env->tile_reg_layout[dst_tile]) &&
                !linx_tile_cube_payload_index_058(env, dst_tile, i, j,
                                                  &payload_index)) {
                g_free(next);
                return false;
            }
            if (linx_tile_numeric_is_packed(dst_dtype)) {
                uint64_t byte_index = payload_index / 2u;
                uint8_t nibble = dst_integer
                                     ? dst_raw & 0xfu
                                     : linx_tile_numeric_encode_nibble(
                                           dst_dtype, value, rmode, sat);
                next[byte_index] |= nibble << ((payload_index & 1u) * 4u);
            } else {
                memcpy(next + (uint64_t)payload_index * elem_bytes,
                       &dst_raw, elem_bytes);
            }
        }
    }

    memset(env->tile_reg[dst_tile], 0, sizeof(env->tile_reg[dst_tile]));
    memcpy(env->tile_reg[dst_tile], next, bytes);
    g_free(next);
    env->tile_reg_bytes[dst_tile] = bytes;
    env->tile_reg_elem_bytes[dst_tile] = elem_bytes;
    env->tile_reg_dtype[dst_tile] = dst_dtype;
    if (!cube_layout(env->tile_reg_layout[dst_tile])) {
        env->tile_reg_valid_cols[dst_tile] = env->tile_acc_cols;
        env->tile_reg_valid_rows[dst_tile] = env->tile_acc_rows;
        env->tile_reg_cols[dst_tile] = physical_cols;
        env->tile_reg_rows[dst_tile] = bytes / row_bytes;
    }
    return true;
}

bool linx_tile_accumulator_convert(CPULinxState *env, unsigned dst_tile,
                                   unsigned size_code)
{
    return linx_tile_accumulator_convert_with_aux_058(
        env, dst_tile, size_code, UINT_MAX, UINT_MAX);
}

static uint32_t cube_accumulator_arch_dtype(const CPULinxState *env)
{
    return env->tile_acc_dtype == LINX_TILE_ACC_S64 ? 17u
           : env->tile_acc_dtype == LINX_TILE_ACC_U64 ? 25u
                                                      : env->tile_acc_dtype;
}

static bool cube_reduction_candidate(CPULinxState *env, uint64_t raw,
                                     bool max_abs, double *value,
                                     uint64_t *output_raw)
{
    switch (env->tile_acc_dtype) {
    case LINX_TILE_ACC_FP32: {
        uint32_t bits = raw;
        float input;
        memcpy(&input, &bits, sizeof(input));
        if (max_abs) {
            input = fabsf(input);
            memcpy(&bits, &input, sizeof(bits));
        }
        *value = input;
        *output_raw = bits;
        return true;
    }
    case LINX_TILE_ACC_FP64: {
        double input;
        memcpy(&input, &raw, sizeof(input));
        if (max_abs) {
            input = fabs(input);
            memcpy(&raw, &input, sizeof(raw));
        }
        *value = input;
        *output_raw = raw;
        return true;
    }
    case LINX_TILE_ACC_S64: {
        int32_t input = raw;
        if (max_abs && input < 0) {
            if (input == INT32_MIN) {
                input = INT32_MAX;
                env->fcsr |= 4u;
            } else {
                input = -input;
            }
        }
        *value = input;
        *output_raw = (uint32_t)input;
        return true;
    }
    case LINX_TILE_ACC_U64:
        *value = (uint32_t)raw;
        *output_raw = (uint32_t)raw;
        return true;
    default:
        return false;
    }
}

bool linx_tile_cube_reduction_outputs_with_input_058(
    CPULinxState *env, unsigned row_max_tile, unsigned group_max_tile,
    unsigned row_max_input)
{
    const unsigned group_code = (env->tile_fpatr_raw >> 19) & 0xfu;
    const unsigned group_n = group_code == 0u ? 0u
                               : group_code == 1u ? 8u
                               : group_code == 2u ? 16u
                                                  : (group_code - 1u) * 16u;
    const bool row_max = ((env->tile_fpatr_raw >> 18) & 1u) != 0u;
    const bool group_max = ((env->tile_fpatr_raw >> 17) & 1u) != 0u;
    const uint32_t arch_dtype = cube_accumulator_arch_dtype(env);
    const unsigned acc_bytes = env->tile_acc_dtype == LINX_TILE_ACC_FP32
                                   ? 4u : 8u;
    const unsigned output_bytes = cube_dtype_bytes(arch_dtype);
    const bool row_init = ((env->tile_fpatr_raw >> 16) & 1u) != 0u;

    if (!env->tile_acc_valid || (row_max && row_max_tile >= LINX_TILE_SLOT_COUNT) ||
        (group_max && (group_max_tile >= LINX_TILE_SLOT_COUNT || group_n == 0u)) ||
        output_bytes == 0u ||
        (row_init && row_max_input >= LINX_TILE_SLOT_COUNT)) {
        return false;
    }
    for (unsigned row = 0; row < env->tile_acc_rows; row++) {
        uint64_t row_best_output = 0u;
        double row_best_value = 0.0;
        bool row_seen = false;
        const unsigned groups = group_max
            ? DIV_ROUND_UP(env->tile_acc_cols, group_n) : 0u;
        for (unsigned group = 0; group < MAX(groups, 1u); group++) {
            uint64_t group_best_output = 0u;
            double group_best_value = 0.0;
            bool group_seen = false;
            const unsigned begin = group_max ? group * group_n : 0u;
            const unsigned end = group_max
                ? MIN(begin + group_n, env->tile_acc_cols)
                : env->tile_acc_cols;
            for (unsigned col = begin; col < end; col++) {
                uint64_t raw = 0u;
                memcpy(&raw, (uint8_t *)env->tile_acc +
                       ((uint64_t)row * env->tile_acc_cols + col) * acc_bytes,
                       acc_bytes);
                double value;
                uint64_t output_raw;
                if (!cube_reduction_candidate(env, raw,
                                               ((env->tile_fpatr_raw >> 15) &
                                                1u) != 0u,
                                               &value, &output_raw)) {
                    return false;
                }
                if (!group_seen || value > group_best_value) {
                    group_best_value = value;
                    group_best_output = output_raw;
                    group_seen = true;
                }
                if (!row_seen || value > row_best_value) {
                    row_best_value = value;
                    row_best_output = output_raw;
                    row_seen = true;
                }
            }
            if (group_max && group_seen) {
                memcpy((uint8_t *)env->tile_reg[group_max_tile] +
                       ((uint64_t)row * groups + group) * output_bytes,
                       &group_best_output, output_bytes);
            }
        }
        if (row_init) {
            uint64_t input_raw;
            double input_value;
            uint64_t input_output;
            if (!cube_read_raw(env, row_max_input, row, 0u, &input_raw,
                               NULL) ||
                !cube_reduction_candidate(env, input_raw,
                    ((env->tile_fpatr_raw >> 15) & 1u) != 0u,
                    &input_value, &input_output)) {
                return false;
            }
            if (!row_seen || input_value > row_best_value) {
                row_best_value = input_value;
                row_best_output = input_output;
                row_seen = true;
            }
        }
        if (row_max && row_seen) {
            memcpy((uint8_t *)env->tile_reg[row_max_tile] +
                   (uint64_t)row * output_bytes, &row_best_output,
                   output_bytes);
        }
    }
    return true;
}

bool linx_tile_cube_reduction_outputs_058(CPULinxState *env,
                                          unsigned row_max_tile,
                                          unsigned group_max_tile)
{
    return linx_tile_cube_reduction_outputs_with_input_058(
        env, row_max_tile, group_max_tile, UINT_MAX);
}

bool linx_tile_acccvt_058(CPULinxState *env, unsigned dst_tile,
                          unsigned size_code)
{
    return linx_tile_accumulator_convert(env, dst_tile, size_code);
}
