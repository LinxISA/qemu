/*
 * QEMU LINX VirtIO Board
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

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial.h"
#include "target/linx/cpu.h"
#include "hw/linx/linx_hart.h"
#include "hw/linx/virt.h"
#include "hw/linx/boot.h"
#include "hw/linx/numa.h"
#include "hw/intc/lxintc.h"
#include "hw/misc/linx_test.h"
#include "hw/misc/linx_reset.h"
#include "chardev/char.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "hw/pci/pci.h"
#include "hw/pci-host/gpex.h"
#include "hw/display/ramfb.h"

static const MemMapEntry virt_memmap[] = {
    [VIRT_DEBUG] =       {        0x0,         0x100 },
    [VIRT_MROM] =        {     0x1000,        0x1000 },
    [VIRT_RESET] =       {     0x2000,        0x1000 },
    [VIRT_TEST] =        {   0x100000,        0x1000 },
    [VIRT_RTC] =         {   0x101000,        0x1000 },
    [VIRT_PCIE_PIO] =    {  0x3000000,       0x10000 },
    [VIRT_LXIC] =        {  VIRT_LXIC_BASE, VIRT_LXIC_SIZE(VIRT_CPUS_MAX) },
    [VIRT_UART0] =       { 0x10000000,         0x100 },
    [VIRT_VIRTIO] =      { 0x10001000,        0x1000 },
    [VIRT_PCIE_ECAM] =   { 0x30000000,    0x10000000 },
    [VIRT_PCIE_MMIO] =   { 0x40000000,    0x40000000 },
    [VIRT_DRAM] =        { 0x80000000,           0x0 },
};

/* PCIe high mmio is fixed for RV32 */
#define VIRT32_HIGH_PCIE_MMIO_BASE  0x300000000ULL
#define VIRT32_HIGH_PCIE_MMIO_SIZE  (4 * GiB)

/* PCIe high mmio for RV64, size is fixed but base depends on top of RAM */
#define VIRT64_HIGH_PCIE_MMIO_SIZE  (16 * GiB)

static MemMapEntry virt_high_pcie_memmap;

static uint32_t mmio_mask, virtio_mask, pcie_mask;

int linx_cpus_total_num;

static void create_pcie_irq_map(void *fdt, char *nodename,
                                uint32_t irq_phandle)
{
    int pin, dev;
    uint32_t
        full_irq_map[GPEX_NUM_IRQS * GPEX_NUM_IRQS * FDT_INT_MAP_WIDTH] = {};
    uint32_t *irq_map = full_irq_map;

    /* This code creates a standard swizzle of interrupts such that
     * each device's first interrupt is based on it's PCI_SLOT number.
     * (See pci_swizzle_map_irq_fn())
     *
     * We only need one entry per interrupt in the table (not one per
     * possible slot) seeing the interrupt-map-mask will allow the table
     * to wrap to any number of devices.
     */
    for (dev = 0; dev < GPEX_NUM_IRQS; dev++) {
        int devfn = dev * 0x8;

        for (pin = 0; pin < GPEX_NUM_IRQS; pin++) {
            int irq_nr = PCIE_IRQ + ((pin + PCI_SLOT(devfn)) % GPEX_NUM_IRQS);
            int i = 0;

            irq_map[i] = cpu_to_be32(devfn << 8);
            i += FDT_PCI_ADDR_CELLS;

            irq_map[i] = cpu_to_be32(pin + 1);
            i += FDT_PCI_INT_CELLS;

            irq_map[i++] = cpu_to_be32(irq_phandle);
            irq_map[i++] = cpu_to_be32(irq_nr);
            irq_map[i] = cpu_to_be32((0x4 | (pcie_mask << 4)));

            irq_map += FDT_INT_MAP_WIDTH;
        }
    }

    qemu_fdt_setprop(fdt, nodename, "interrupt-map",
                     full_irq_map, sizeof(full_irq_map));

    qemu_fdt_setprop_cells(fdt, nodename, "interrupt-map-mask",
                           0x1800, 0, 0, 0x7);
}

static void create_fdt_socket_cpus(LINXVirtState *s, int socket,
                                   char *clust_name, uint32_t *phandle,
                                   uint32_t *intc_phandles)
{
    int cpu;
    uint32_t cpu_phandle;
    MachineState *mc = MACHINE(s);
    char *cpu_name, *core_name, *intc_name;
    const char *name;

    for (cpu = s->soc[socket].num_harts - 1; cpu >= 0; cpu--) {
        cpu_phandle = (*phandle)++;

        cpu_name = g_strdup_printf("/cpus/cpu@%d",
            s->soc[socket].hartid_base + cpu);
        qemu_fdt_add_subnode(mc->fdt, cpu_name);
        /* todo: should design and fix this */
        qemu_fdt_setprop_string(mc->fdt, cpu_name, "mmu-type", "linx,sv48");
        name = "rv(todo: not yet design, hack to let kernel go)";
        qemu_fdt_setprop_string(mc->fdt, cpu_name, "linx,isa", name);
        qemu_fdt_setprop_string(mc->fdt, cpu_name, "compatible", "linx");
        qemu_fdt_setprop_string(mc->fdt, cpu_name, "status", "okay");
        qemu_fdt_setprop_cell(mc->fdt, cpu_name, "reg",
            s->soc[socket].hartid_base + cpu);
        qemu_fdt_setprop_string(mc->fdt, cpu_name, "device_type", "cpu");
        linx_socket_fdt_write_id(mc, mc->fdt, cpu_name, socket);
        qemu_fdt_setprop_cell(mc->fdt, cpu_name, "phandle", cpu_phandle);

        intc_phandles[cpu] = (*phandle)++;

        intc_name = g_strdup_printf("%s/interrupt-controller", cpu_name);
        qemu_fdt_add_subnode(mc->fdt, intc_name);
        qemu_fdt_setprop_cell(mc->fdt, intc_name, "phandle",
            intc_phandles[cpu]);
        qemu_fdt_setprop_string(mc->fdt, intc_name, "compatible",
            "linx,cpu-intc");
        qemu_fdt_setprop(mc->fdt, intc_name, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(mc->fdt, intc_name, "#interrupt-cells", 1);

        core_name = g_strdup_printf("%s/core%d", clust_name, cpu);
        qemu_fdt_add_subnode(mc->fdt, core_name);
        qemu_fdt_setprop_cell(mc->fdt, core_name, "cpu", cpu_phandle);

        g_free(core_name);
        g_free(intc_name);
        g_free(cpu_name);
    }
}

static void create_fdt_socket_memory(LINXVirtState *s,
                                     const MemMapEntry *memmap, int socket)
{
    char *mem_name;
    uint64_t addr, size;
    MachineState *mc = MACHINE(s);

    addr = memmap[VIRT_DRAM].base + linx_socket_mem_offset(mc, socket);
    size = linx_socket_mem_size(mc, socket);
    mem_name = g_strdup_printf("/memory@%lx", (long)addr);
    qemu_fdt_add_subnode(mc->fdt, mem_name);
    qemu_fdt_setprop_cells(mc->fdt, mem_name, "reg",
        addr >> 32, addr, size >> 32, size);
    qemu_fdt_setprop_string(mc->fdt, mem_name, "device_type", "memory");
    linx_socket_fdt_write_id(mc, mc->fdt, mem_name, socket);
    g_free(mem_name);
}

/*
 * timer {
 *      interrupts-extended = <0x04 0x04 0x02 0x04>;
 *      always-on;
 *      compatible = "linx,linx-timer";
 * };
 */
static void create_fdt_timer(LINXVirtState *s, int cpu_num, uint32_t timer_extended[])
{
    const char compat[] = "linx,linx-timer";
	MachineState *mc = MACHINE(s);

    qemu_fdt_add_subnode(mc->fdt, "/timer");
	qemu_fdt_setprop(mc->fdt, "/timer", "compatible", compat, sizeof(compat));
    qemu_fdt_setprop(mc->fdt, "/timer", "always-on", NULL, 0);
    qemu_fdt_setprop(mc->fdt, "/timer", "interrupts-extended",
	                 timer_extended, cpu_num * sizeof(uint32_t) * 2);
}

/* lxintc@4000000 {
 *		phandle = <0xa>;
 *		lxic,nirq = <0x100>;
 *		lxic,stride = <0x1000>;
 *		reg = <0x0 0x4000000 0x0 0x8000>;
 *		#interrupt-cells = <0x2>;
 *		interrupts-extended = <0x8 0xb 0x8 0x9 0x6 0xb 0x6 0x9 0x4 0xb 0x4 0x9 0x2 0xb 0x2 0x9>;
 *		interrupt-controller;
 *		msi-controller;
 *		compatible = "huawei,lxic";
 *	};
 * */
static void create_fdt_lxintc(LINXVirtState *s, const MemMapEntry *memmap,
                              uint32_t *phandle, uint32_t cpu_num,
                              uint32_t lxintc_extended[], uint32_t *msi_phandle)
{
    MachineState *mc = MACHINE(s);
    char *name;
    uint64_t base;

    base = memmap[VIRT_LXIC].base;
    name = g_strdup_printf("/soc/lxintc@%" PRIx64, base);
    qemu_fdt_add_subnode(mc->fdt, name);
    qemu_fdt_setprop_string(mc->fdt, name, "compatible", "huawei,lxic");
    qemu_fdt_setprop(mc->fdt, name, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop(mc->fdt, name, "msi-controller", NULL, 0);
    qemu_fdt_setprop(mc->fdt, name, "interrupts-extended", lxintc_extended, cpu_num * sizeof(uint32_t) * 2);
    qemu_fdt_setprop_cell(mc->fdt, name, "#interrupt-cells", FDT_LXINTC_INTERRUPT_CELLS);
    qemu_fdt_setprop_cells(mc->fdt, name, "reg", 0x0, base, 0x0, memmap[VIRT_LXIC].size);
    qemu_fdt_setprop_cell(mc->fdt, name, "lxic,stride", VIRT_LXIC_STRIDE);
    qemu_fdt_setprop_cell(mc->fdt, name, "lxic,domain-stride", VIRT_LXIC_DOMAIN_STRIDE);
    qemu_fdt_setprop_cell(mc->fdt, name, "lxic,nirq", FDT_LXIC_NIRQ);
    *msi_phandle = (*phandle)++;
    qemu_fdt_setprop_cell(mc->fdt, name, "phandle", *msi_phandle);

    /* Add device-tree nodes here whose 'interrupt-parent' is 'lxintc' */
    g_free(name);
}

static void create_fdt_sockets(LINXVirtState *s, const MemMapEntry *memmap,
                               bool is_32_bit, uint32_t *phandle,
                               uint32_t *irq_mmio_phandle,
                               uint32_t *irq_pcie_phandle,
                               uint32_t *irq_virtio_phandle,
			       uint32_t *linx_phandle, uint32_t *msi_pcie_phandle)
{
    int socket, cpu = 0, i;
    char *clust_name;
    uint32_t *intc_phandles;
    uint32_t *lxintc_extended, *timer_extended;;
    uint32_t msi_phandle;
    MachineState *mc = MACHINE(s);

    qemu_fdt_add_subnode(mc->fdt, "/cpus");
    qemu_fdt_setprop_cell(mc->fdt, "/cpus", "timebase-frequency",
                          LINX_DEFAULT_TIMEBASE_FREQ);
    qemu_fdt_setprop_cell(mc->fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(mc->fdt, "/cpus", "#address-cells", 0x1);
    qemu_fdt_add_subnode(mc->fdt, "/cpus/cpu-map");

    lxintc_extended = g_new0(uint32_t, linx_socket_count(mc) * VIRT_CPUS_MAX * 2);
    timer_extended = g_new0(uint32_t, linx_socket_count(mc) * VIRT_CPUS_MAX * 2);

    for (socket = (linx_socket_count(mc) - 1); socket >= 0; socket--) {
        clust_name = g_strdup_printf("/cpus/cpu-map/cluster%d", socket);
        qemu_fdt_add_subnode(mc->fdt, clust_name);

        intc_phandles = g_new0(uint32_t, s->soc[socket].num_harts);

        create_fdt_socket_cpus(s, socket, clust_name, phandle,
            intc_phandles);

        for (i = 0; i < s->soc[socket].num_harts; i++, cpu++) {
            lxintc_extended[cpu * 2 + 0] = cpu_to_be32(intc_phandles[i]);
            lxintc_extended[cpu * 2 + 1] = cpu_to_be32(ACR1_EI);
            timer_extended[cpu * 2 + 0] = cpu_to_be32(intc_phandles[i]);
            timer_extended[cpu * 2 + 1] = cpu_to_be32(ACR1_TI);
        }

        create_fdt_socket_memory(s, memmap, socket);

        g_free(intc_phandles);
        g_free(clust_name);
    }

    create_fdt_lxintc(s, memmap, phandle, cpu, lxintc_extended, &msi_phandle);
    create_fdt_timer(s, cpu, timer_extended);
    g_free(lxintc_extended);
    g_free(timer_extended);

    *linx_phandle = msi_phandle;
    *irq_mmio_phandle = msi_phandle;
    *irq_virtio_phandle = msi_phandle;
    *irq_pcie_phandle = msi_phandle;
    *msi_pcie_phandle = msi_phandle;

    linx_socket_fdt_write_distance_matrix(mc, mc->fdt);
}

static void create_fdt_virtio(LINXVirtState *s, const MemMapEntry *memmap,
                              uint32_t irq_virtio_phandle)
{
    int i;
    char *name;
    MachineState *mc = MACHINE(s);

    for (i = 0; i < VIRTIO_COUNT; i++) {
        name = g_strdup_printf("/soc/virtio_mmio@%lx",
            (long)(memmap[VIRT_VIRTIO].base + i * memmap[VIRT_VIRTIO].size));
        qemu_fdt_add_subnode(mc->fdt, name);
        qemu_fdt_setprop_string(mc->fdt, name, "compatible", "virtio,mmio");
        qemu_fdt_setprop_cells(mc->fdt, name, "reg",
            0x0, memmap[VIRT_VIRTIO].base + i * memmap[VIRT_VIRTIO].size,
            0x0, memmap[VIRT_VIRTIO].size);
        qemu_fdt_setprop_cell(mc->fdt, name, "interrupt-parent",
            irq_virtio_phandle);
        qemu_fdt_setprop_cells(mc->fdt, name, "interrupts", VIRTIO_IRQ + i, (0x4 | (virtio_mask << 4)));
        g_free(name);
    }
}

static void create_fdt_pcie(LINXVirtState *s, const MemMapEntry *memmap,
                            uint32_t irq_pcie_phandle, uint32_t msi_pcie_phandle)
{
    char *name;
    MachineState *mc = MACHINE(s);

    name = g_strdup_printf("/soc/pci@%lx",
        (long) memmap[VIRT_PCIE_ECAM].base);
    qemu_fdt_add_subnode(mc->fdt, name);
    qemu_fdt_setprop_cell(mc->fdt, name, "#address-cells",
        FDT_PCI_ADDR_CELLS);
    qemu_fdt_setprop_cell(mc->fdt, name, "#interrupt-cells",
        FDT_PCI_INT_CELLS);
    qemu_fdt_setprop_cell(mc->fdt, name, "#size-cells", 0x2);
    qemu_fdt_setprop_string(mc->fdt, name, "compatible",
        "pci-host-ecam-generic");
    qemu_fdt_setprop_string(mc->fdt, name, "device_type", "pci");
    qemu_fdt_setprop_cell(mc->fdt, name, "linux,pci-domain", 0);
    qemu_fdt_setprop_cells(mc->fdt, name, "bus-range", 0,
        memmap[VIRT_PCIE_ECAM].size / PCIE_MMCFG_SIZE_MIN - 1);
    qemu_fdt_setprop(mc->fdt, name, "dma-coherent", NULL, 0);
    qemu_fdt_setprop_cell(mc->fdt, name, "msi-parent", msi_pcie_phandle);
    qemu_fdt_setprop_cells(mc->fdt, name, "reg", 0,
        memmap[VIRT_PCIE_ECAM].base, 0, memmap[VIRT_PCIE_ECAM].size);
    qemu_fdt_setprop_sized_cells(mc->fdt, name, "ranges",
        1, FDT_PCI_RANGE_IOPORT, 2, 0,
        2, memmap[VIRT_PCIE_PIO].base, 2, memmap[VIRT_PCIE_PIO].size,
        1, FDT_PCI_RANGE_MMIO,
        2, memmap[VIRT_PCIE_MMIO].base,
        2, memmap[VIRT_PCIE_MMIO].base, 2, memmap[VIRT_PCIE_MMIO].size,
        1, FDT_PCI_RANGE_MMIO_64BIT,
        2, virt_high_pcie_memmap.base,
        2, virt_high_pcie_memmap.base, 2, virt_high_pcie_memmap.size);

    create_pcie_irq_map(mc->fdt, name, irq_pcie_phandle);
    g_free(name);
}

static void create_fdt_reset(LINXVirtState *s, const MemMapEntry *memmap,
                             uint32_t *phandle)
{
    char *name;
    uint32_t test_phandle;
    MachineState *mc = MACHINE(s);

    test_phandle = (*phandle)++;
    name = g_strdup_printf("/soc/test@%lx",
        (long)memmap[VIRT_TEST].base);
    qemu_fdt_add_subnode(mc->fdt, name);
    {
        static const char * const compat[3] = {
            "sifive,test1", "sifive,test0", "syscon"
        };
        qemu_fdt_setprop_string_array(mc->fdt, name, "compatible",
                                      (char **)&compat, ARRAY_SIZE(compat));
    }
    qemu_fdt_setprop_cells(mc->fdt, name, "reg",
        0x0, memmap[VIRT_TEST].base, 0x0, memmap[VIRT_TEST].size);
    qemu_fdt_setprop_cell(mc->fdt, name, "phandle", test_phandle);
    test_phandle = qemu_fdt_get_phandle(mc->fdt, name);
    g_free(name);

    name = g_strdup_printf("/soc/reboot");
    qemu_fdt_add_subnode(mc->fdt, name);
    qemu_fdt_setprop_string(mc->fdt, name, "compatible", "syscon-reboot");
    qemu_fdt_setprop_cell(mc->fdt, name, "regmap", test_phandle);
    qemu_fdt_setprop_cell(mc->fdt, name, "offset", 0x0);
    qemu_fdt_setprop_cell(mc->fdt, name, "value", FINISHER_RESET);
    g_free(name);

    name = g_strdup_printf("/soc/poweroff");
    qemu_fdt_add_subnode(mc->fdt, name);
    qemu_fdt_setprop_string(mc->fdt, name, "compatible", "syscon-poweroff");
    qemu_fdt_setprop_cell(mc->fdt, name, "regmap", test_phandle);
    qemu_fdt_setprop_cell(mc->fdt, name, "offset", 0x0);
    qemu_fdt_setprop_cell(mc->fdt, name, "value", FINISHER_PASS);
    g_free(name);
}

static void create_fdt_uart(LINXVirtState *s, const MemMapEntry *memmap,
                            uint32_t irq_mmio_phandle)
{
    char *name;
    MachineState *mc = MACHINE(s);

    name = g_strdup_printf("/soc/uart@%lx", (long)memmap[VIRT_UART0].base);
    qemu_fdt_add_subnode(mc->fdt, name);
    qemu_fdt_setprop_string(mc->fdt, name, "compatible", "ns16550a");
    qemu_fdt_setprop_cells(mc->fdt, name, "reg",
        0x0, memmap[VIRT_UART0].base,
        0x0, memmap[VIRT_UART0].size);
    qemu_fdt_setprop_cell(mc->fdt, name, "clock-frequency", 3686400);
    qemu_fdt_setprop_cell(mc->fdt, name, "interrupt-parent", irq_mmio_phandle);
    qemu_fdt_setprop_cells(mc->fdt, name, "interrupts", UART0_IRQ, (0x4 | (mmio_mask << 4)));

    qemu_fdt_add_subnode(mc->fdt, "/chosen");
    qemu_fdt_setprop_string(mc->fdt, "/chosen", "stdout-path", name);
    g_free(name);
}

static void create_fdt_rtc(LINXVirtState *s, const MemMapEntry *memmap,
                           uint32_t irq_mmio_phandle)
{
    char *name;
    MachineState *mc = MACHINE(s);

    name = g_strdup_printf("/soc/rtc@%lx", (long)memmap[VIRT_RTC].base);
    qemu_fdt_add_subnode(mc->fdt, name);
    qemu_fdt_setprop_string(mc->fdt, name, "compatible",
        "google,goldfish-rtc");
    qemu_fdt_setprop_cells(mc->fdt, name, "reg",
        0x0, memmap[VIRT_RTC].base, 0x0, memmap[VIRT_RTC].size);
    qemu_fdt_setprop_cell(mc->fdt, name, "interrupt-parent",
        irq_mmio_phandle);
    qemu_fdt_setprop_cells(mc->fdt, name, "interrupts", RTC_IRQ, (0x4 | (mmio_mask << 4)));
    g_free(name);
}

static void create_fdt(LINXVirtState *s, const MemMapEntry *memmap,
                       uint64_t mem_size, const char *cmdline, bool is_32_bit)
{
    MachineState *mc = MACHINE(s);
    uint32_t phandle = 1, irq_mmio_phandle = 1, msi_pcie_phandle = 1;
    uint32_t irq_pcie_phandle = 1, irq_virtio_phandle = 1;
    uint32_t linx_phandle = 1;

    if (mc->dtb) {
        mc->fdt = load_device_tree(mc->dtb, &s->fdt_size);
        if (!mc->fdt) {
            error_report("load_device_tree() failed");
            exit(1);
        }
        goto update_bootargs;
    } else {
        mc->fdt = create_device_tree(&s->fdt_size);
        if (!mc->fdt) {
            error_report("create_device_tree() failed");
            exit(1);
        }
    }

    qemu_fdt_setprop_string(mc->fdt, "/", "model", "linx-virtio,qemu");
    qemu_fdt_setprop_string(mc->fdt, "/", "compatible", "linx-virtio");
    qemu_fdt_setprop_cell(mc->fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(mc->fdt, "/", "#address-cells", 0x2);

    qemu_fdt_add_subnode(mc->fdt, "/soc");
    qemu_fdt_setprop(mc->fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(mc->fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(mc->fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(mc->fdt, "/soc", "#address-cells", 0x2);

    create_fdt_sockets(s, memmap, is_32_bit, &phandle,
        &irq_mmio_phandle, &irq_pcie_phandle, &irq_virtio_phandle, &linx_phandle,
		&msi_pcie_phandle);

    create_fdt_virtio(s, memmap, linx_phandle);

    create_fdt_pcie(s, memmap, linx_phandle, msi_pcie_phandle);

    create_fdt_reset(s, memmap, &phandle);

    create_fdt_uart(s, memmap, linx_phandle);

    create_fdt_rtc(s, memmap, linx_phandle);

update_bootargs:
    if (cmdline) {
        qemu_fdt_setprop_string(mc->fdt, "/chosen", "bootargs", cmdline);
    }
}

static inline DeviceState *gpex_pcie_init(MemoryRegion *sys_mem,
                                          hwaddr ecam_base, hwaddr ecam_size,
                                          hwaddr mmio_base, hwaddr mmio_size,
                                          hwaddr high_mmio_base,
                                          hwaddr high_mmio_size,
                                          hwaddr pio_base,
                                          DeviceState *irqchip)
{
    DeviceState *dev;
    MemoryRegion *ecam_alias, *ecam_reg;
    MemoryRegion *mmio_alias, *high_mmio_alias, *mmio_reg;

    dev = qdev_new(TYPE_GPEX_HOST);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    ecam_alias = g_new0(MemoryRegion, 1);
    ecam_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_init_alias(ecam_alias, OBJECT(dev), "pcie-ecam",
                             ecam_reg, 0, ecam_size);
    memory_region_add_subregion(get_system_memory(), ecam_base, ecam_alias);

    mmio_alias = g_new0(MemoryRegion, 1);
    mmio_reg = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "pcie-mmio",
                             mmio_reg, mmio_base, mmio_size);
    memory_region_add_subregion(get_system_memory(), mmio_base, mmio_alias);

    /* Map high MMIO space */
    high_mmio_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(high_mmio_alias, OBJECT(dev), "pcie-mmio-high",
                             mmio_reg, high_mmio_base, high_mmio_size);
    memory_region_add_subregion(get_system_memory(), high_mmio_base,
                                high_mmio_alias);

    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, pio_base);

    /* todo: pcie INTx irq should be connected to Lxic */

    return dev;
}

static void virt_machine_init(MachineState *machine)
{
    const MemMapEntry *memmap = virt_memmap;
    LINXVirtState *s = LINX_VIRT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);
    char *soc_name;
    target_ulong start_addr = memmap[VIRT_DRAM].base;
    target_ulong firmware_end_addr, kernel_start_addr;
    uint32_t fdt_load_addr;
    uint64_t kernel_entry;
    DeviceState *mmio_lxic, *virtio_lxic, *pcie_lxic;
    int i, j, k, base_hartid, hart_count, hartid;

    /* Check socket count limit */
    if (VIRT_SOCKETS_MAX < linx_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
            VIRT_SOCKETS_MAX);
        exit(1);
    }

    env2ctl = g_hash_table_new(NULL, NULL);

    /* Initialize interrupt domain */
    for (i = 0; i < FDT_LXIC_NIRQ; i++) {
        j = i / 8;
        k = i % 8;
        lxic_domain[j] |= (DOM_ACR1 << (k *4));
    }

    /* Initialize sockets */
    mmio_lxic = virtio_lxic = pcie_lxic = NULL;
    for (i = 0; i < linx_socket_count(machine); i++) {
        if (!linx_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        base_hartid = linx_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        hart_count = linx_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_LINX_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_abort);
        linx_cpus_total_num += hart_count;

        /* Per-socket Linx Controller*/
        for (j = 0; j < s->soc[i].num_harts; j++) {
            hartid = base_hartid + j;
            DeviceState *cpudev = DEVICE(qemu_get_cpu(hartid));
            for (k = 0; k < VIRT_LXIC_DOMAIN_NUM; k++)
                linx_intc[hartid][k] = lxic_create(hartid,
        memmap[VIRT_LXIC].base + hartid * VIRT_LXIC_STRIDE, VIRT_LXIC_STRIDE);
            /* todo: fix this global virable */
            g_hash_table_insert(env2ctl, &s->soc[i].harts[j].env, linx_intc[hartid]);

            if (j == 0) {
                mmio_lxic = linx_intc[hartid][DOM_ACR1];
                mmio_mask = 1 << hartid;
                virtio_lxic = linx_intc[hartid][DOM_ACR1];
                virtio_mask = 1 << hartid;
                pcie_lxic = linx_intc[hartid][DOM_ACR1];
                pcie_mask = 1 << hartid;
            }
            if (j == 1) {
                virtio_lxic = linx_intc[hartid][DOM_ACR1];
                virtio_mask = 1 << hartid;
                pcie_lxic = linx_intc[hartid][DOM_ACR1];
                pcie_mask = 1 << hartid;
            }
            if (j == 2) {
                pcie_lxic = linx_intc[hartid][DOM_ACR1];
                pcie_mask = 1 << hartid;
            }
            qdev_connect_gpio_out(cpudev, GTIMER_ACR1,
                                  qdev_get_gpio_in(cpudev, ACR1_TI));
            qdev_connect_gpio_out(cpudev, GTIMER_ACR0,
                                  qdev_get_gpio_in(cpudev, ACR0_TI));

        }

    }

    virt_high_pcie_memmap.size = VIRT64_HIGH_PCIE_MMIO_SIZE;
    virt_high_pcie_memmap.base = memmap[VIRT_DRAM].base + machine->ram_size;
    virt_high_pcie_memmap.base =
        ROUND_UP(virt_high_pcie_memmap.base, virt_high_pcie_memmap.size);

    /* register system main memory (actual RAM) */
    memory_region_add_subregion(system_memory, memmap[VIRT_DRAM].base,
        machine->ram);

    /* create device tree */
    create_fdt(s, memmap, machine->ram_size, machine->kernel_cmdline, false);

    /* boot rom */
    memory_region_init_rom(mask_rom, NULL, "linx_virt_board.mrom",
                           memmap[VIRT_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[VIRT_MROM].base,
                                mask_rom);

    firmware_end_addr = linx_find_and_load_firmware(machine,
                                LINX_BIOS_BIN, start_addr, NULL);

    if (machine->kernel_filename) {
        kernel_start_addr = linx_calc_kernel_start_addr(&s->soc[0],
                                                         firmware_end_addr);

        kernel_entry = linx_load_kernel(machine->kernel_filename,
                                         kernel_start_addr, NULL);

        if (machine->initrd_filename) {
            hwaddr start;
            hwaddr end = linx_load_initrd(machine->initrd_filename,
                                           machine->ram_size, kernel_entry,
                                           &start);
            qemu_fdt_setprop_cell(machine->fdt, "/chosen",
                                  "linux,initrd-start", start);
            qemu_fdt_setprop_cell(machine->fdt, "/chosen", "linux,initrd-end",
                                  end);
        }
    } else {
       /*
        * If dynamic firmware is used, it doesn't know where is the next mode
        * if kernel argument is not set.
        */
        kernel_entry = 0;
    }


    /* Compute the fdt load address in dram */
    fdt_load_addr = linx_load_fdt(memmap[VIRT_DRAM].base,
                                   machine->ram_size, machine->fdt);
    /* load the reset vector */
    linx_setup_rom_reset_vec(machine, &s->soc[0], start_addr,
                              virt_memmap[VIRT_MROM].base,
                              virt_memmap[VIRT_MROM].size, kernel_entry,
                              fdt_load_addr, machine->fdt);

    /* Linx Reset MMIO device */
    linx_reset_create(memmap[VIRT_RESET].base);

    /* SiFive Test MMIO device */
    sifive_test_create(memmap[VIRT_TEST].base);

    /* VirtIO MMIO devices */
    for (i = 0; i < VIRTIO_COUNT; i++) {
        sysbus_create_simple("virtio-mmio",
            memmap[VIRT_VIRTIO].base + i * memmap[VIRT_VIRTIO].size,
            qdev_get_gpio_in(DEVICE(virtio_lxic), VIRTIO_IRQ + i));
    }

    gpex_pcie_init(system_memory,
                   memmap[VIRT_PCIE_ECAM].base,
                   memmap[VIRT_PCIE_ECAM].size,
                   memmap[VIRT_PCIE_MMIO].base,
                   memmap[VIRT_PCIE_MMIO].size,
                   virt_high_pcie_memmap.base,
                   virt_high_pcie_memmap.size,
                   memmap[VIRT_PCIE_PIO].base,
                   DEVICE(pcie_lxic));

    serial_mm_init(system_memory, memmap[VIRT_UART0].base,
        0, qdev_get_gpio_in(DEVICE(mmio_lxic), UART0_IRQ), 399193,
        serial_hd(0), DEVICE_LITTLE_ENDIAN);

    sysbus_create_simple("goldfish_rtc", memmap[VIRT_RTC].base,
        qdev_get_gpio_in(DEVICE(mmio_lxic), RTC_IRQ));

}

static void virt_machine_instance_init(Object *obj)
{
}

static void virt_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "LINX VirtIO board";
    mc->init = virt_machine_init;
    mc->max_cpus = VIRT_CPUS_MAX;
    mc->default_cpu_type = TYPE_LINX_CPU_BASE;
    mc->pci_allow_0_address = true;
    mc->possible_cpu_arch_ids = linx_numa_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = linx_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id = linx_numa_get_default_cpu_node_id;
    mc->numa_mem_supported = true;
    mc->default_ram_id = "linx_virt_board.ram";

    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_RAMFB_DEVICE);
}

static const TypeInfo virt_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("virt"),
    .parent     = TYPE_MACHINE,
    .class_init = virt_machine_class_init,
    .instance_init = virt_machine_instance_init,
    .instance_size = sizeof(LINXVirtState),
};

static void virt_machine_init_register_types(void)
{
    type_register_static(&virt_machine_typeinfo);
}

type_init(virt_machine_init_register_types)
