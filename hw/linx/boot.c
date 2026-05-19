/*
 * QEMU LINX Boot Helper
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
#include "qemu-common.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "exec/cpu-defs.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/linx/boot.h"
#include "hw/linx/boot_opensbi.h"
#include "elf.h"
#include "sysemu/device_tree.h"
#include "sysemu/qtest.h"

#include <libfdt.h>

#define EM_LINX_LEGACY 233

static int linx_try_load_elf(const char *filename, uint64_t *pentry,
                             uint64_t *highaddr, symbol_fn_t sym_cb)
{
    ssize_t ret;

    ret = load_elf_ram_sym(filename, NULL, NULL, NULL,
                           pentry, NULL, highaddr, NULL,
                           0, EM_LINX, 1, 0, NULL, true, sym_cb);
    if (ret > 0) {
        return ret;
    }

    ret = load_elf_ram_sym(filename, NULL, NULL, NULL,
                           pentry, NULL, highaddr, NULL,
                           0, EM_LINX_LEGACY, 1, 0, NULL, true, sym_cb);
    return ret;
}

target_ulong linx_calc_kernel_start_addr(LINXHartArrayState *harts,
                                          target_ulong firmware_end_addr) {
    /* linx only have 64bit */
    return QEMU_ALIGN_UP(firmware_end_addr, 2 * MiB);
}

target_ulong linx_find_and_load_firmware(MachineState *machine,
                                          const char *default_machine_firmware,
                                          hwaddr firmware_load_addr,
                                          symbol_fn_t sym_cb)
{
    char *firmware_filename = NULL;
    target_ulong firmware_end_addr = firmware_load_addr;

    if ((!machine->firmware) || (!strcmp(machine->firmware, "default"))) {
        /*
         * The user didn't specify -bios, or has specified "-bios default".
         * That means we are going to load the OpenSBI binary included in
         * the QEMU source.
         */
        firmware_filename = linx_find_firmware(default_machine_firmware);
    } else if (strcmp(machine->firmware, "none")) {
        firmware_filename = linx_find_firmware(machine->firmware);
    }

    if (firmware_filename) {
        /* If not "none" load the firmware */
        firmware_end_addr = linx_load_firmware(firmware_filename,
                                                firmware_load_addr, sym_cb);
        g_free(firmware_filename);
    }

    return firmware_end_addr;
}

char *linx_find_firmware(const char *firmware_filename)
{
    char *filename;

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware_filename);
    if (filename == NULL) {
        if (!qtest_enabled()) {
            /*
             * We only ship plain binary bios images in the QEMU source.
             * With Spike machine that uses ELF images as the default bios,
             * running QEMU test will complain hence let's suppress the error
             * report for QEMU testing.
             */
            error_report("Unable to load the LINX firmware \"%s\"",
                         firmware_filename);
            exit(1);
        }
    }

    return filename;
}

target_ulong linx_load_firmware(const char *firmware_filename,
                                 hwaddr firmware_load_addr,
                                 symbol_fn_t sym_cb)
{
    uint64_t firmware_entry, firmware_size, firmware_end;

    if (linx_try_load_elf(firmware_filename, &firmware_entry,
                          &firmware_end, sym_cb) > 0) {
        return firmware_end;
    }

    firmware_size = load_image_targphys_as(firmware_filename,
                                           firmware_load_addr,
                                           current_machine->ram_size, NULL);

    if (firmware_size > 0) {
        return firmware_load_addr + firmware_size;
    }

    error_report("could not load firmware '%s'", firmware_filename);
    exit(1);
}

target_ulong linx_load_kernel(const char *kernel_filename,
                               target_ulong kernel_start_addr,
                               symbol_fn_t sym_cb)
{
    uint64_t kernel_entry;

    if (linx_try_load_elf(kernel_filename, &kernel_entry, NULL,
                          sym_cb) > 0) {
        return kernel_entry;
    }

    if (load_uimage_as(kernel_filename, &kernel_entry, NULL, NULL,
                       NULL, NULL, NULL) > 0) {
        return kernel_entry;
    }

    if (load_image_targphys_as(kernel_filename, kernel_start_addr,
                               current_machine->ram_size, NULL) > 0) {
        return kernel_start_addr;
    }

    error_report("could not load kernel '%s'", kernel_filename);
    exit(1);
}

hwaddr linx_load_initrd(const char *filename, uint64_t mem_size,
                         uint64_t kernel_entry, hwaddr *start)
{
    int size;

    /*
     * We want to put the initrd far enough into RAM that when the
     * kernel is uncompressed it will not clobber the initrd. However
     * on boards without much RAM we must ensure that we still leave
     * enough room for a decent sized initrd, and on boards with large
     * amounts of RAM we must avoid the initrd being so far up in RAM
     * that it is outside lowmem and inaccessible to the kernel.
     * So for boards with less  than 256MB of RAM we put the initrd
     * halfway into RAM, and for boards with 256MB of RAM or more we put
     * the initrd at 128MB.
     */
    *start = kernel_entry + MIN(mem_size / 2, 128 * MiB);

    size = load_ramdisk(filename, *start, mem_size - *start);
    if (size == -1) {
        size = load_image_targphys(filename, *start, mem_size - *start);
        if (size == -1) {
            error_report("could not load ramdisk '%s'", filename);
            exit(1);
        }
    }

    return *start + size;
}

uint32_t linx_load_fdt(hwaddr dram_base, uint64_t mem_size, void *fdt)
{
    uint32_t temp, fdt_addr;
    hwaddr dram_end = dram_base + mem_size;
    int ret, fdtsize = fdt_totalsize(fdt);

    if (fdtsize <= 0) {
        error_report("invalid device-tree");
        exit(1);
    }

    /*
     * We should put fdt as far as possible to avoid kernel/initrd overwriting
     * its content. But it should be addressable by 32 bit system as well.
     * Thus, put it at an 16MB aligned address that less than fdt size from the
     * end of dram or 3GB whichever is lesser.
     */
    temp = MIN(dram_end, 3072 * MiB);
    fdt_addr = QEMU_ALIGN_DOWN(temp - fdtsize, 16 * MiB);

    ret = fdt_pack(fdt);
    /* Should only fail if we've built a corrupted tree */
    g_assert(ret == 0);
    /* copy in the device tree */
    qemu_fdt_dumpdtb(fdt, fdtsize);

    rom_add_blob_fixed_as("fdt", fdt, fdtsize, fdt_addr,
                          &address_space_memory);

    return fdt_addr;
}

void linx_rom_copy_firmware_info(MachineState *machine, hwaddr rom_base,
                                  hwaddr rom_size, uint32_t reset_vec_size,
                                  uint64_t kernel_entry)
{
    struct fw_dynamic_info dinfo;
    size_t dinfo_len;

    if (sizeof(dinfo.magic) == 4) {
        dinfo.magic = cpu_to_le32(FW_DYNAMIC_INFO_MAGIC_VALUE);
        dinfo.version = cpu_to_le32(FW_DYNAMIC_INFO_VERSION);
        dinfo.next_mode = cpu_to_le32(FW_DYNAMIC_INFO_NEXT_MODE_S);
        dinfo.next_addr = cpu_to_le32(kernel_entry);
    } else {
        dinfo.magic = cpu_to_le64(FW_DYNAMIC_INFO_MAGIC_VALUE);
        dinfo.version = cpu_to_le64(FW_DYNAMIC_INFO_VERSION);
        dinfo.next_mode = cpu_to_le64(FW_DYNAMIC_INFO_NEXT_MODE_S);
        dinfo.next_addr = cpu_to_le64(kernel_entry);
    }
    dinfo.options = 0;
    dinfo.boot_hart = 0;
    dinfo_len = sizeof(dinfo);

    /**
     * copy the dynamic firmware info. This information is specific to
     * OpenSBI but doesn't break any other firmware as long as they don't
     * expect any certain value in "a2" register.
     */
    if (dinfo_len > (rom_size - reset_vec_size)) {
        error_report("not enough space to store dynamic firmware info");
        exit(1);
    }

    rom_add_blob_fixed_as("mrom.finfo", &dinfo, dinfo_len,
                           rom_base + reset_vec_size,
                           &address_space_memory);
}

void linx_setup_rom_reset_vec(MachineState *machine, LINXHartArrayState *harts,
                               hwaddr start_addr,
                               hwaddr rom_base, hwaddr rom_size,
                               uint64_t kernel_entry,
                               uint32_t fdt_load_addr, void *fdt)
{
    int i;
    uint32_t start_addr_hi32 = start_addr >> 32;
    uint32_t kernel_entry_hi32 = kernel_entry >> 32;

    /* reset vector */
    /* a0->lxlcid, a1->fdt_laddr, a2->kernel_entry */
    uint32_t reset_vec[18] = {
        0x00005001,                  /* BSTART.STD    IND               */
                                     /* 1:                              */
        0x00000f87,                  /* addtpc %tpcrel_hi(fw_dyn) ,->t  */
        0x044c0f95,                  /* addi t#1, %tpcrel_lo(1b) 68 ->t  */
        0x02100f3b,                  /* ssrget  33,     ->u             */
        0x000e0115,                  /* addi    u#1, 0,         ->a0    */
        0xffec3f19,                  /* ldi     [t#1, -16],     ->u     */
        0x000e0195,                  /* addi    u#1, 0,         ->a1    */
        0xfffc3f19,                  /* ldi     [t#1, -8],      ->u     */
        0x000e0215,                  /* addi    u#1, 0,         ->a2    */
        0xffdc3f19,                  /* ldi     [t#1, -24],     ->u     */
        0x0000071c,                  /* c.setc.tgt      u#1             */
        0x00000001,                  /* BSTOP                           */
        start_addr,                  /* start: .dword                   */
        start_addr_hi32,
        fdt_load_addr,               /* fdt_laddr: .dword               */
        0x00000000,
        kernel_entry,                /* kernel_entry: .dword            */
        kernel_entry_hi32,
                                     /* fw_dyn:                         */
    };

    /* copy in the reset vector in little_endian byte order */
    for (i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }
    rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          rom_base, &address_space_memory);

    return;
}
