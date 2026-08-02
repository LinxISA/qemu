/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LINX_TILE_CUBE_057_H
#define LINX_TILE_CUBE_057_H

#include "cpu.h"

bool linx_tile_cube_compute_057(CPULinxState *env, unsigned src_a,
                                unsigned src_b, unsigned row_scale,
                                unsigned column_scale, unsigned bias,
                                unsigned size_code, bool mx, bool with_bias,
                                bool accumulate);
bool linx_tile_acccvt_057(CPULinxState *env, unsigned dst_tile,
                          unsigned size_code);

#endif
