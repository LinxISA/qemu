/*
 * LinxISA CPU parameters
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_CPU_PARAM_H
#define LINX_CPU_PARAM_H

#define TARGET_PAGE_BITS 12

#if defined(TARGET_LINX64)
#define TARGET_PHYS_ADDR_SPACE_BITS 64
#define TARGET_VIRT_ADDR_SPACE_BITS 64
#elif defined(TARGET_LINX32)
#define TARGET_PHYS_ADDR_SPACE_BITS 32
#define TARGET_VIRT_ADDR_SPACE_BITS 32
#endif

#define TARGET_INSN_START_EXTRA_WORDS 0

#endif

