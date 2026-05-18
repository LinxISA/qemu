/*
 * QEMU LINX Hart Array
 *
 * Copyright (c) 2022 HiSilicon Technologies.
 *
 * Holds the state of a homogeneous array of LINX harts
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
#include "qapi/error.h"
#include "qemu/module.h"
#include "sysemu/reset.h"
#include "hw/sysbus.h"
#include "target/linx/cpu.h"
#include "hw/qdev-properties.h"
#include "hw/linx/linx_hart.h"
#include "hw/misc/linx_reset.h"

static Property linx_harts_props[] = {
    DEFINE_PROP_UINT32("num-harts", LINXHartArrayState, num_harts, 1),
    DEFINE_PROP_UINT32("boot-hartid", LINXHartArrayState, boot_hartid, 0),
    DEFINE_PROP_UINT32("hartid-base", LINXHartArrayState, hartid_base, 0),
    DEFINE_PROP_STRING("cpu-type", LINXHartArrayState, cpu_type),
    DEFINE_PROP_UINT64("resetvec", LINXHartArrayState, resetvec,
                       DEFAULT_RSTVEC),
    DEFINE_PROP_INT32("acr[0].priv", LINXHartArrayState, acr[0].priv, 0),
    DEFINE_PROP_INT32("acr[1].priv", LINXHartArrayState, acr[1].priv, 1),
    DEFINE_PROP_INT32("acr[2].priv", LINXHartArrayState, acr[2].priv, 2),
    DEFINE_PROP_END_OF_LIST(),
};

static GArray *linx_nodes;
static bool linx_nodes_need_init = true;

static void linx_harts_cpu_reset(void *opaque)
{
    LINXCPU *cpu = opaque;
    cpu_reset(CPU(cpu));
}

static bool linx_hart_realize(LINXHartArrayState *s, int idx,
                               char *cpu_type, Error **errp)
{
    object_initialize_child(OBJECT(s), "harts[*]", &s->harts[idx], cpu_type);
    qdev_prop_set_uint64(DEVICE(&s->harts[idx]), "resetvec", s->resetvec);

    target_ulong hartid = s->hartid_base + idx;
    s->harts[idx].env.lxlcid = hartid;

    /* a0[0:15] keep hartid */
    s->harts[idx].env.gpr[xA0] |= hartid;

    /* set boot hart flag: a0[16] is boot hart */
    /* TODO: need modify reset vector, remove "set a0" */
    if (hartid == s->boot_hartid) {
        s->harts[idx].env.gpr[xA0] |= (1 << 16);
    } else {
        /* not set boot hart flag, set start-powered-off to forbid
         *  this hart to run, s->harts[idx].parent_obj is CPUState
         */
        Object *obj = OBJECT(&s->harts[idx].parent_obj);
        object_property_set_bool(obj, "start-powered-off", true, errp);
    }
    qemu_register_reset(linx_harts_cpu_reset, &s->harts[idx]);
    return qdev_realize(DEVICE(&s->harts[idx]), NULL, errp);
}

static bool check_acr_helper(CPULINXState *env, int high, int low)
{
    if (high == low) {
        return true;
    }

    if (high < 0 || low < 0) {
        return false;
    }

    LINXHartArrayState *s = env2linx_arry(env);

    if (!s->acr[high].valid || !s->acr[low].valid) {
        return false;
    }

    for (int i = 0; i < s->acr[high].childnum; ++i) {
        if (s->acr[high].childpriv[i] == low) {
            return true;
        }
    }

    return false;
}

struct LINXHartArrayState *env2linx_arry(CPULINXState *env)
{
    CPUState *cs = env_cpu(env);

    /* top Object -> Object -> DeviceState -> CPUState */
    Object *top_obj = cs->parent_obj.parent_obj.parent;

    /* LINXHartArrayState is the top Object */
    return LINX_HART_ARRAY(top_obj);
}

int linx_reset_hart(int hartid)
{
    if (hartid > linx_cpus_total_num - 1) {
        return RESET_ERROR;
    }

    LINXCPU *linx_cpu = NULL;
    LINXHartArrayState* tmp_array;
    LINXCPU *tmp_cpu;

    for (int i = 0; i < linx_nodes->len; ++i) {
        tmp_array = g_array_index(linx_nodes, LINXHartArrayState*, i);
        for (int j = 0; j < tmp_array->num_harts; ++j) {
            tmp_cpu = &tmp_array->harts[j];
            CPULINXState *env = &tmp_cpu->env;
            if (env->lxlcid == hartid) {
                linx_cpu = tmp_cpu;
            }
        }
    }

    CPUState *cs = CPU(linx_cpu);
    if (cs->start_powered_off == 0) {
        return RESET_FAIL;
    }

    cs->start_powered_off = 0;
    if (linx_cpu != NULL) {
        linx_harts_cpu_reset(linx_cpu);
    } else {
        return RESET_FAIL;
    }

    return RESET_PASS;
}

bool check_acr_request(CPULINXState *env, int aacr, int target_acr)
{
    return check_acr_helper(env, target_acr, aacr);
}

bool check_acr_enter(CPULINXState *env, int aacr, int target_acr)
{
    return check_acr_helper(env, aacr, target_acr);
}

static void acr_init(LINXHartArrayState *s)
{
    /*init valid */
    s->acr[0].valid = 1;
    s->acr[1].valid = 1;
    s->acr[2].valid = 1;
    s->acr[3].valid = 1;
    s->acr[4].valid = 1;

    for (int i = 5; i < 16; ++i) {
        s->acr[i] = (ACRTree){0};
    }

    /* acr0 */
    s->acr[0].ppriv = -1;
    s->acr[0].childnum = 4;
    s->acr[0].childpriv[0] = 1;
    s->acr[0].childpriv[1] = 2;
    s->acr[0].childpriv[2] = 3;
    s->acr[0].childpriv[3] = 4;

    /* acr1 */
    s->acr[1].ppriv = 0;
    s->acr[1].childnum = 3;
    s->acr[1].childpriv[0] = 2;
    s->acr[1].childpriv[1] = 3;
    s->acr[1].childpriv[2] = 4;

    /* acr2 */
    s->acr[2].ppriv = 1;
    s->acr[2].childnum = 0;

    /* acr3 */
    s->acr[3].ppriv = 1;
    s->acr[3].childnum = 1;
    s->acr[3].childpriv[0] = 4;

    /* acr4 */
    s->acr[4].ppriv = 3;
    s->acr[4].childnum = 0;
}

static void linx_harts_realize(DeviceState *dev, Error **errp)
{
    LINXHartArrayState *s = LINX_HART_ARRAY(dev);
    int n;

    if (linx_nodes_need_init) {
        linx_nodes = g_array_new (false, false, sizeof (LINXHartArrayState*));
        linx_nodes_need_init = false;
    }

    g_array_append_val(linx_nodes, s);

    s->harts = g_new0(LINXCPU, s->num_harts);
    acr_init(s);

    for (n = 0; n < s->num_harts; n++) {
        if (!linx_hart_realize(s, n, s->cpu_type, errp)) {
            return;
        }
    }
}

static void linx_harts_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, linx_harts_props);
    dc->realize = linx_harts_realize;
}

static const TypeInfo linx_harts_info = {
    .name          = TYPE_LINX_HART_ARRAY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LINXHartArrayState),
    .class_init    = linx_harts_class_init,
};

static void linx_harts_register_types(void)
{
    type_register_static(&linx_harts_info);
}

type_init(linx_harts_register_types)
