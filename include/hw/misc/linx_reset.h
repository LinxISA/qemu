/*
 * Linx Test Reset interface
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

#ifndef HW_LINX_RESET_H
#define HW_LINX_RESET_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_LINX_RESET "linx.reset"

typedef struct LinxResetState LinxResetState;
DECLARE_INSTANCE_CHECKER(LinxResetState, LINX_RESET,
                         TYPE_LINX_RESET)

struct LinxResetState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
};

enum {
    RESET_PASS,
    RESET_ERROR,
    RESET_FAIL
};

DeviceState *linx_reset_create(hwaddr addr);
extern int linx_cpus_total_num;

#endif
