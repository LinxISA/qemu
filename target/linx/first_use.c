/*
 * Linx extension first-use profile.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "first_use.h"

uint64_t linx_econfig_sanitize(uint64_t value)
{
    return value & LINX_ECONFIG_ALLOWED_MASK;
}

void linx_first_use_reset(CPULinxState *env)
{
    unsigned acr;

    for (acr = 0; acr < LINX_ACR_COUNT; acr++) {
        env->ssr_acr[acr][LINX_SSR_ECONFIG] = LINX_ECONFIG_RESET;
    }
}

bool linx_first_use_prepare(CPULinxState *env, LinxFirstUseKind kind,
                            uint64_t pc)
{
    uint64_t enable_bit;

    if (env->acr != 2u) {
        return false;
    }
    switch (kind) {
    case LINX_FIRST_USE_VECTOR:
        enable_bit = LINX_ECONFIG_VECTOR_BIT;
        break;
    case LINX_FIRST_USE_CUBE:
        enable_bit = LINX_ECONFIG_CUBE_BIT;
        break;
    default:
        return false;
    }
    if ((env->ssr_acr[1][LINX_SSR_ECONFIG] & enable_bit) == 0) {
        return false;
    }

    env->pc = pc;
    env->pending_trap_cause = LINX_FIRST_USE_CAUSE;
    env->pending_trap_arg0 = kind;
    return true;
}
