/*
 * QEMU LINX NUMA Helper
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

#ifndef LINX_NUMA_H
#define LINX_NUMA_H

#include "hw/sysbus.h"
#include "sysemu/numa.h"

/**
 * linx_socket_count:
 * @ms: pointer to machine state
 *
 * Returns: number of sockets for a numa system and 1 for a non-numa system
 */
int linx_socket_count(const MachineState *ms);

/**
 * linx_socket_first_hartid:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Returns: first hartid for a valid socket and -1 for an invalid socket
 */
int linx_socket_first_hartid(const MachineState *ms, int socket_id);

/**
 * linx_socket_last_hartid:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Returns: last hartid for a valid socket and -1 for an invalid socket
 */
int linx_socket_last_hartid(const MachineState *ms, int socket_id);

/**
 * linx_socket_hart_count:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Returns: number of harts for a valid socket and -1 for an invalid socket
 */
int linx_socket_hart_count(const MachineState *ms, int socket_id);

/**
 * linx_socket_mem_offset:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Returns: offset of ram belonging to given socket
 */
uint64_t linx_socket_mem_offset(const MachineState *ms, int socket_id);

/**
 * linx_socket_mem_size:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Returns: size of ram belonging to given socket
 */
uint64_t linx_socket_mem_size(const MachineState *ms, int socket_id);

/**
 * linx_socket_check_hartids:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Returns: true if hardids belonging to given socket are contiguous else false
 */
bool linx_socket_check_hartids(const MachineState *ms, int socket_id);

/**
 * linx_socket_fdt_write_id:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Write NUMA node-id FDT property for given FDT node
 */
void linx_socket_fdt_write_id(const MachineState *ms, void *fdt,
                               const char *node_name, int socket_id);

/**
 * linx_socket_fdt_write_distance_matrix:
 * @ms: pointer to machine state
 * @socket_id: socket index
 *
 * Write NUMA distance matrix in FDT for given machine
 */
void linx_socket_fdt_write_distance_matrix(const MachineState *ms, void *fdt);

CpuInstanceProperties
linx_numa_cpu_index_to_props(MachineState *ms, unsigned cpu_index);

int64_t linx_numa_get_default_cpu_node_id(const MachineState *ms, int idx);

const CPUArchIdList *linx_numa_possible_cpu_arch_ids(MachineState *ms);

#endif /* LINX_NUMA_H */
