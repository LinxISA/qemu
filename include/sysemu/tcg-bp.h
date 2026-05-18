/*
 * QEMU TCG Breakpoint support
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef SYSEMU_TCG_BP_H
#define SYSEMU_TCG_BP_H

#include "hw/core/cpu.h"

extern bool tcg_bp_add_bp_pc(uintptr_t pc);
extern bool tcg_bp_add_bp_pc_count(uintptr_t pc, int count);
extern bool tcg_bp_add_bp_event(void);
extern GArray *tcg_bps_init(guint *sz);
extern void tcg_bp_set_static_bp(const char *optarg);
extern char *tcg_bp_info(CPUState *cpu, gint i);
extern bool tcg_bp_add(CPUState *cpu, const char *spec);
extern void tcg_bp_remove(CPUState *cpu, gint bp_id);
extern void tcg_bp_disable(CPUState *cpu, gint bp_id);
extern void tcg_bp_enable(CPUState *cpu, gint bp_id);
extern void tcg_bp_tlb_fill(CPUState *cpu, uint64_t addr, int size,
                     MMUAccessType access_type);
#endif
