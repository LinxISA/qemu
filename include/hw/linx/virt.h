/*
 * QEMU LINX VirtIO machine interface
 *
 * Copyright (c) 2022 HiSilicon Technologies.
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

#ifndef HW_LINX_VIRT_H
#define HW_LINX_VIRT_H

#include "hw/linx/linx_hart.h"
#include "hw/sysbus.h"
#include "hw/block/flash.h"
#include "qom/object.h"

#define VIRT_CPUS_MAX 8
#define VIRT_SOCKETS_MAX 8

#define TYPE_LINX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")
typedef struct LINXVirtState LINXVirtState;
DECLARE_INSTANCE_CHECKER(LINXVirtState, LINX_VIRT_MACHINE,
                         TYPE_LINX_VIRT_MACHINE)

struct LINXVirtState {
    /*< private >*/
    MachineState parent;

    /*< public >*/
    LINXHartArrayState soc[VIRT_SOCKETS_MAX];
    DeviceState *plic[VIRT_SOCKETS_MAX];

    int fdt_size;
    bool have_aclint;
};

enum {
    VIRT_DEBUG,
    VIRT_MROM,
    VIRT_RESET,
    VIRT_TEST,
    VIRT_RTC,
    VIRT_CLINT,
    VIRT_ACLINT_SSWI,
    VIRT_LXIC,
    VIRT_UART0,
    VIRT_VIRTIO,
    VIRT_FW_CFG,
    VIRT_FLASH,
    VIRT_DRAM,
    VIRT_PCIE_MMIO,
    VIRT_PCIE_PIO,
    VIRT_PCIE_ECAM
};

enum {
    UART0_IRQ = 10,
    RTC_IRQ = 11,
    VIRTIO_IRQ = 1, /* 1 to 8 */
    VIRTIO_COUNT = 8,
    PCIE_IRQ = 0x20, /* 32 to 35 */
    VIRTIO_NDEV = 0x35 /* Arbitrary maximum number of interrupts */
};

#define FDT_PCI_ADDR_CELLS    3
#define FDT_PCI_INT_CELLS     1
#define FDT_INT_MAP_WIDTH     (FDT_PCI_ADDR_CELLS + FDT_PCI_INT_CELLS + 3)

#endif
