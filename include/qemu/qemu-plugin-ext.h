/*
 * Copyright (C) 2017, Emilio G. Cota <cota@braap.org>
 * Copyright (C) 2019, Linaro
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef QEMU_PLUGIN_API_EXT_H
#define QEMU_PLUGIN_API_EXT_H

#include "qemu/qemu-plugin.h"

/**
 * qemu_plugin_ext_vcpu_memory_read_vaddr() - Read Memory by virtual addr
 * @cpu_index: vcpu index
 * @vaddr: virtual address
 * @ptr: pointer to save data which read from memory
 * @len: bytes of data to read
 *
 */
int qemu_plugin_ext_vcpu_memory_read_vaddr(uint32_t cpu_index, uint64_t vaddr,
                                           void *ptr, uint64_t len);

int qemu_plugin_ext_vcpu_get_header_info(uint32_t *header_info);

plugin_tileop_info qemu_plugin_ext_vcpu_get_tileop_info(
  plugin_tileop_info *tileop_info);

int qemu_plugin_ext_vcpu_get_src_fvec_reg(uint32_t cpu_index,
    int src_enc, uint64_t *blk, int lane_id);

int qemu_plugin_ext_vcpu_get_simt_predm(uint32_t cpu_index, uint64_t *value);

int qemu_plugin_ext_vcpu_get_src_fvec_ri_gpr_idx(uint32_t cpu_index, int src,
                                                 uint64_t *blk);
int qemu_plugin_ext_vcpu_get_dst_fvec_ro_gpr_idx(uint32_t cpu_index, int src,
                                                 uint64_t *blk);

int qemu_plugin_ext_vcpu_get_dst_fvec_ro_reg(uint32_t cpu_index, int src,
                                             uint64_t *blk);

int qemu_plugin_ext_vcpu_get_src_fvec_sys_reg(uint32_t cpu_index, int src,
                                      uint64_t *blk);

int qemu_plugin_ext_vcpu_get_src_reg(uint32_t cpu_index, int src_enc,
                                     uint64_t *blk_t);

/**
 * qemu_plugin_ext_vcpu_read_gpr() - read gpr
 * @cpu_index: vcpu index
 * @gpr_idx: 0-N, N depends on cpu arch
 * @gpr_value: return gpr value if successed
 *
 * Returns: 0: success, !=0: failed
 */
int qemu_plugin_ext_vcpu_read_gpr(uint32_t cpu_index, uint32_t gpr_idx,
                                  uint64_t *gpr_value);

/**
 * qemu_plugin_ext_vcpu_read_sys_reg() - read system reg
 * @cpu_index: vcpu index
 * @reg_idx: idx depends on cpu arch
 * @reg_value: return sys reg value if successed
 *
 * Returns: 0: success, !=0: failed
 */
int qemu_plugin_ext_vcpu_read_sys_reg(uint32_t cpu_index, int reg_idx,
                                      uint64_t *reg_value);

/**
 * enum system_status_idx, a common/normalized system status index.
 */
enum system_status_idx {
  QEMU_SYS_INVALID_IDX,

  // 0: user level, 1: os kernel, 2: hypervisor/virtualization
  QEMU_SYS_PRIVILEGE_LEVEL,

  // ASID in arm/mips/riscv
  QEMU_SYS_HW_CONTEXT_ID,

  QEMU_SYS_MAX_INDEX
};

/**
 * qemu_plugin_ext_vcpu_read_gpr() - read gpr
 * @cpu_index: vcpu index
 * @sys_idx:   normalized system status index
 * @sys_value: return sys status value if successed
 *
 * Returns: 0: success, !=0: failed
 */
int qemu_plugin_ext_vcpu_get_system_status(uint32_t cpu_index,
                                           enum system_status_idx sys_idx,
                                           uint64_t *sys_value);

#endif // QEMU_PLUGIN_API_EXT_H
