/*
 * LINX cpu parameters for qemu.
 *
 * Copyright (c) 2022 HiSilicon Technologies.
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef LINX_CPU_PARAM_H
#define LINX_CPU_PARAM_H 1

#define TARGET_LONG_BITS 64
#define TARGET_PHYS_ADDR_SPACE_BITS 56 /* 44-bit PPN */
#define TARGET_VIRT_ADDR_SPACE_BITS 48 /* sv48 */

#define TARGET_PAGE_BITS 12 /* 4 KiB Pages */
/*
 * The current MMU Modes are:
 *  - U mode 0b000
 *  - S mode 0b001
 *  - M mode 0b011
 */
#define NB_MMU_MODES 4

#endif
