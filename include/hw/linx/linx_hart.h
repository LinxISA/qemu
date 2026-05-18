/*
 * QEMU LINX Hart Array interface
 *
 * Copyright (c) 2022 HiSilicon Technologies.
 *
 * Holds the state of a heterogenous array of LINX harts
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

#ifndef HW_LINX_HART_H
#define HW_LINX_HART_H

#include "hw/sysbus.h"
#include "target/linx/cpu.h"
#include "qom/object.h"

#define TYPE_LINX_HART_ARRAY "linx.hart_array"

OBJECT_DECLARE_SIMPLE_TYPE(LINXHartArrayState, LINX_HART_ARRAY)

typedef struct {
    int priv;
    int ppriv;
    bool valid;
    int childpriv[16];
    int childnum;
} ACRTree;

struct LINXHartArrayState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    uint32_t num_harts;
    uint32_t boot_hartid;
    uint32_t hartid_base;
    char *cpu_type;
    uint64_t resetvec;
    LINXCPU *harts;
    ACRTree acr[16];
};

struct LINXHartArrayState *env2linx_arry(CPULINXState *env);
int linx_reset_hart(int hartid);

#endif
