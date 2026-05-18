/*
 * QEMU monitor for LINX
 *
 * Copyright (c) 2022 HiSilicon Technologies.
 *
 * LINX specific monitor commands implementation
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
#include "cpu.h"
#include "cpu_bits.h"
#include "monitor/monitor.h"
#include "monitor/hmp-target.h"

#ifdef TARGET_LINX
#define PTE_HEADER_FIELDS       "vaddr            paddr            "\
                                "size             attr\n"
#define PTE_HEADER_DELIMITER    "---------------- ---------------- "\
                                "---------------- -------\n"
#else
#define PTE_HEADER_FIELDS       "vaddr    paddr            size     attr\n"
#define PTE_HEADER_DELIMITER    "-------- ---------------- -------- -------\n"
#endif


static void mem_info_svxx(Monitor *mon, CPUArchState *env)
{
    /* TODO: implementation for linx vmma */
}

void hmp_info_mem(Monitor *mon, const QDict *qdict)
{
    CPUArchState *env;

    env = mon_get_cpu_env(mon);
    if (!env) {
        monitor_printf(mon, "No CPU available\n");
        return;
    }

    if (!linx_feature(env, LINX_FEATURE_MMU)) {
        monitor_printf(mon, "S-mode MMU unavailable\n");
        return;
    }

    mem_info_svxx(mon, env);
}
