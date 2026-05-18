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

#ifndef LINX_BOOT_H
#define LINX_BOOT_H

#include "exec/cpu-defs.h"
#include "hw/loader.h"
#include "hw/linx/linx_hart.h"

#define LINX_BIOS_BIN    "LinxInit.bin"

char *linx_plic_hart_config_string(int hart_count);

target_ulong linx_calc_kernel_start_addr(LINXHartArrayState *harts,
                                          target_ulong firmware_end_addr);
target_ulong linx_find_and_load_firmware(MachineState *machine,
                                          const char *default_machine_firmware,
                                          hwaddr firmware_load_addr,
                                          symbol_fn_t sym_cb);
char *linx_find_firmware(const char *firmware_filename);
target_ulong linx_load_firmware(const char *firmware_filename,
                                 hwaddr firmware_load_addr,
                                 symbol_fn_t sym_cb);
target_ulong linx_load_kernel(const char *kernel_filename,
                               target_ulong firmware_end_addr,
                               symbol_fn_t sym_cb);
hwaddr linx_load_initrd(const char *filename, uint64_t mem_size,
                         uint64_t kernel_entry, hwaddr *start);
uint32_t linx_load_fdt(hwaddr dram_start, uint64_t dram_size, void *fdt);
void linx_setup_rom_reset_vec(MachineState *machine, LINXHartArrayState *harts,
                               hwaddr saddr,
                               hwaddr rom_base, hwaddr rom_size,
                               uint64_t kernel_entry,
                               uint32_t fdt_load_addr, void *fdt);
void linx_rom_copy_firmware_info(MachineState *machine, hwaddr rom_base,
                                  hwaddr rom_size,
                                  uint32_t reset_vec_size,
                                  uint64_t kernel_entry);
bool linx_is_32bit(LINXHartArrayState *harts);

#endif /* LINX_BOOT_H */
