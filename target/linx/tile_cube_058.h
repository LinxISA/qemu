/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LINX_TILE_CUBE_058_H
#define LINX_TILE_CUBE_058_H

#include "cpu.h"

typedef struct LinxTileCubeDimensions {
    unsigned m;
    unsigned n;
    unsigned k;
} LinxTileCubeDimensions;

LinxTileCubeDimensions linx_tile_cube_dimensions_058(const CPULinxState *env);
bool linx_tile_cube_output_shape_valid(const CPULinxState *env,
                                       uint32_t bytes,
                                       unsigned elem_bytes);
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
bool linx_tile_cube_compute_shared_ab_058(
    CPULinxState *env, const uint8_t *shared_a, uint32_t shared_a_bytes,
    uint32_t shared_a_dtype, uint32_t shared_a_cols,
    const uint8_t *shared_b, uint32_t shared_b_bytes,
    uint32_t shared_b_dtype, uint32_t shared_b_cols,
    unsigned size_code, bool accumulate);
bool linx_tile_acccvt_058(CPULinxState *env, unsigned dst_tile,
                           unsigned size_code);

#endif
