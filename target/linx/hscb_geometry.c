/*
 * PTO 0.59.1 HSCB worker geometry binding.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hscb_geometry.h"

bool linx_hscb_geometry_bind(CPULinxState *env, uint32_t acr,
                             uint64_t invocation, uint64_t block_num,
                             uint64_t block_id)
{
    if (acr != 0 || invocation == 0 || block_num == 0 ||
        block_num > UINT32_MAX || block_id > UINT32_MAX ||
        block_id >= block_num || env->hscb_geometry_valid) {
        return false;
    }

    env->hscb_geometry_invocation = invocation;
    env->hscb_blocknum = block_num;
    env->hscb_blockid = block_id;
    env->hscb_geometry_valid = 1;
    return true;
}

bool linx_hscb_geometry_matches(const CPULinxState *env,
                                uint64_t invocation)
{
    return env->hscb_geometry_valid && invocation != 0 &&
           env->hscb_geometry_invocation == invocation;
}

bool linx_hscb_geometry_clear(CPULinxState *env, uint64_t invocation)
{
    if (!linx_hscb_geometry_matches(env, invocation)) {
        return false;
    }

    env->hscb_geometry_valid = 0;
    env->hscb_geometry_invocation = 0;
    env->hscb_blocknum = 0;
    env->hscb_blockid = 0;
    return true;
}

uint64_t linx_hscb_geometry_blocknum(const CPULinxState *env)
{
    return env->hscb_geometry_valid && (env->acr & 0xfu) == 0 ?
           env->hscb_blocknum : 0;
}

uint64_t linx_hscb_geometry_blockid(const CPULinxState *env)
{
    return env->hscb_geometry_valid && (env->acr & 0xfu) == 0 ?
           env->hscb_blockid : 0;
}
