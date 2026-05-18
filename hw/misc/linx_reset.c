/*
 * Linx Test Reset interface
 *
 *
 * Test memory mapped device used to reset hart
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
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/runstate.h"
#include "hw/misc/linx_reset.h"

extern int linx_reset_hart(int hartid);

static uint64_t linx_reset_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint64_t ret = 0;

    if (addr == 64)
        ret = linx_cpus_total_num;

    return ret;
}

static void linx_reset_write(void *opaque, hwaddr addr, uint64_t val64,
                             unsigned int size)
{
    int ret = RESET_ERROR;

    if (addr == 0) {
        /* val64[0:8] */
        int control_word = val64 & 0x1ff;
        /* val64[16:31] */
        int hartid = (val64 >> 16) & 0xffff;

        if (control_word == 1)
            ret = linx_reset_hart(hartid);

        if (ret != RESET_PASS)
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write: addr=0x%x val=0x%016" PRIx64 "\n",
                          __func__, (int)addr, val64);
    }

    return;
}

static const MemoryRegionOps linx_reset_ops = {
    .read = linx_reset_read,
    .write = linx_reset_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4
    }
};

static void linx_reset_init(Object *obj)
{
    LinxResetState *s = LINX_RESET(obj);

    memory_region_init_io(&s->mmio, obj, &linx_reset_ops, s,
                          TYPE_LINX_RESET, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const TypeInfo linx_reset_info = {
    .name          = TYPE_LINX_RESET,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LinxResetState),
    .instance_init = linx_reset_init,
};

static void linx_reset_register_types(void)
{
    type_register_static(&linx_reset_info);
}

type_init(linx_reset_register_types)

/*
 * Create Reset device.
 */
DeviceState *linx_reset_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_LINX_RESET);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}
