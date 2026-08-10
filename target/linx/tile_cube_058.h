/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LINX_TILE_CUBE_058_H
#define LINX_TILE_CUBE_058_H

#include "cpu.h"

bool linx_tile_cube_compute_058(CPULinxState *env, unsigned src_a,
                                unsigned src_b, unsigned row_scale,
                                unsigned column_scale, unsigned bias,
                                unsigned size_code, bool mx, bool with_bias,
                                bool accumulate);
bool linx_tile_cube_compute_shared_b_058(
    CPULinxState *env, unsigned src_a, const uint8_t *shared_b,
    uint32_t shared_b_bytes, uint32_t shared_b_dtype, unsigned size_code,
    bool accumulate);
bool linx_tile_acccvt_058(CPULinxState *env, unsigned dst_tile,
                          unsigned size_code);

#endif
