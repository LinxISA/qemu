/*
 * LINX VMState Description
 *
 * Copyright (c) 2020 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/error-report.h"
#include "sysemu/kvm.h"
#include "migration/cpu.h"

/* TODO: When migration is used, we need to redefine VMStateDescription. */

const VMStateDescription vmstate_linx_cpu = {
    .name = "cpu",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (VMStateField[]) {
        VMSTATE_UINTTL_ARRAY(env.gpr, LINXCPU, GPR_REG_SIZE),
        VMSTATE_UINTTL(env.pc, LINXCPU),
        VMSTATE_UINTTL(env.linx_load_res, LINXCPU),
        VMSTATE_UINTTL(env.linx_load_val, LINXCPU),
        VMSTATE_UINTTL(env.badaddr, LINXCPU),
        VMSTATE_UINT32(env.features, LINXCPU),
        VMSTATE_UINTTL(env.priv, LINXCPU),
        VMSTATE_UINTTL(env.resetvec, LINXCPU),
        VMSTATE_UINTTL(env.lxlcid, LINXCPU),
        VMSTATE_UINTTL(env.satp, LINXCPU),

        VMSTATE_END_OF_LIST()
    }
};
