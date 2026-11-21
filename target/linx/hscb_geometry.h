/*
 * PTO 0.59.1 HSCB worker geometry binding.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_HSCB_GEOMETRY_H
#define LINX_HSCB_GEOMETRY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct CPUArchState CPULinxState;

bool linx_hscb_geometry_bind(CPULinxState *env, uint32_t acr,
                             uint64_t invocation, uint64_t block_num,
                             uint64_t block_id);
bool linx_hscb_geometry_clear(CPULinxState *env, uint64_t invocation);
bool linx_hscb_geometry_matches(const CPULinxState *env,
                                uint64_t invocation);
uint64_t linx_hscb_geometry_blocknum(const CPULinxState *env);
uint64_t linx_hscb_geometry_blockid(const CPULinxState *env);

#endif
