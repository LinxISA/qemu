/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LINX_TILE_CUBE_058_H
#define LINX_TILE_CUBE_058_H

#include "cpu.h"

typedef struct LinxTileCubeDimensions {
    unsigned m;
    unsigned n;
    unsigned k;
} LinxTileCubeDimensions;

enum {
    LINX_TILE_LAYOUT_CUBE_M16 = 4u,
    LINX_TILE_LAYOUT_CUBE_M32 = 5u,
    LINX_TILE_LAYOUT_CUBE_N8 = 6u,
};

bool linx_tile_cube_descriptor_058(CPULinxState *env, unsigned tile,
                                   uint32_t layout, uint32_t dtype,
                                   uint32_t valid_rows, uint32_t valid_cols,
                                   uint32_t capacity_bytes);
bool linx_tile_cube_payload_index_058(const CPULinxState *env, unsigned tile,
                                      unsigned row, unsigned column,
                                      uint32_t *index);

LinxTileCubeDimensions linx_tile_cube_dimensions_058(const CPULinxState *env);
bool linx_tile_cube_output_descriptor_058(
    const CPULinxState *env, unsigned ordinal, uint32_t bytes,
    uint32_t *dtype, uint32_t *valid_cols, uint32_t *valid_rows,
    uint32_t *cols, uint32_t *rows);
bool linx_tile_cube_group_dimensions_legal_058(const CPULinxState *env);
bool linx_tile_cube_primary_legal_058(const CPULinxState *env,
                                      unsigned src_a, unsigned src_b,
                                      bool mx, bool accumulate);
bool linx_tile_cube_compute_058(CPULinxState *env, unsigned src_a,
                                unsigned src_b, unsigned row_scale,
                                unsigned column_scale, unsigned bias,
                                unsigned size_code, bool mx, bool with_bias,
                                bool accumulate);
bool linx_tile_cube_compute_shared_b_058(
    CPULinxState *env, unsigned src_a, const uint8_t *shared_b,
    uint32_t shared_b_bytes, uint32_t shared_b_dtype, uint32_t shared_b_cols,
    unsigned size_code, bool accumulate);
bool linx_tile_accumulator_convert(CPULinxState *env, unsigned dst_tile,
                                   unsigned size_code);
bool linx_tile_accumulator_convert_with_aux_058(
    CPULinxState *env, unsigned dst_tile, unsigned size_code,
    unsigned quant_tile, unsigned relu_tile);
bool linx_tile_cube_reduction_outputs_058(CPULinxState *env,
                                          unsigned row_max_tile,
                                          unsigned group_max_tile);
bool linx_tile_cube_reduction_outputs_with_input_058(
    CPULinxState *env, unsigned row_max_tile, unsigned group_max_tile,
    unsigned row_max_input);
bool linx_tile_fpatr_mode_uses_vector_parameter_058(unsigned mode);
bool linx_tile_fpatr_mode_uses_scalar_parameter_058(unsigned mode);
bool linx_tile_fpatr_mode_uses_s32_accumulator_058(unsigned mode);
bool linx_tile_fpatr_quant_parameter_legal_058(unsigned mode, uint64_t value);
bool linx_tile_fpatr_relu_parameter_legal_058(uint64_t value);
bool linx_tile_fpatr_datr_legal_058(unsigned mode, unsigned rmode, bool sat);
bool linx_tile_cube_compute_shared_ab_058(
    CPULinxState *env, const uint8_t *shared_a, uint32_t shared_a_bytes,
    uint32_t shared_a_dtype, uint32_t shared_a_cols,
    const uint8_t *shared_b, uint32_t shared_b_bytes,
    uint32_t shared_b_dtype, uint32_t shared_b_cols,
    unsigned size_code, bool accumulate);
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
    unsigned size_code, bool mx, bool with_bias, bool accumulate,
    uint32_t shared_a_layout, uint32_t shared_b_layout);
bool linx_tile_acccvt_058(CPULinxState *env, unsigned dst_tile,
                           unsigned size_code);

#endif
