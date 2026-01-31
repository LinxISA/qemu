/*
 * LinxISA helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "system/system.h"

void HELPER(linx_ebreak)(CPULinxState *env)
{
    CPUState *cs = env_cpu(env);
    int code = (int)env->gpr[LINX_REG_A0];

    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN, code);
    cpu_loop_exit(cs);
}

