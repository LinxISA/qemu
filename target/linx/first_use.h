/*
 * Linx extension first-use profile.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_LINX_FIRST_USE_H
#define TARGET_LINX_FIRST_USE_H

#include "cpu.h"

#define LINX_SSR_ECONFIG 0xF07
#define LINX_ECONFIG_VECTOR_BIT (UINT64_C(1) << 32)
#define LINX_ECONFIG_CUBE_BIT (UINT64_C(1) << 33)
#define LINX_ECONFIG_RESET UINT64_C(0x0000000300000008)
#define LINX_ECONFIG_ALLOWED_MASK UINT64_C(0x000000030000000f)
#define LINX_FIRST_USE_CAUSE UINT32_C(4)
#define LINX_FIRST_USE_BLOCK_CUBE 6u

typedef enum LinxFirstUseKind {
    LINX_FIRST_USE_VECTOR = 0,
    LINX_FIRST_USE_CUBE = 1,
} LinxFirstUseKind;

uint64_t linx_econfig_sanitize(uint64_t value);
void linx_first_use_reset(CPULinxState *env);
bool linx_first_use_prepare(CPULinxState *env, LinxFirstUseKind kind,
                            uint64_t pc);

#endif
