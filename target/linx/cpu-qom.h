/*
 * QEMU LinxISA CPU QOM header (target agnostic)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_CPU_QOM_H
#define LINX_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_LINX_CPU "linx-cpu"

OBJECT_DECLARE_CPU_TYPE(LinxCPU, LinxCPUClass, LINX_CPU)

#define LINX_CPU_TYPE_SUFFIX "-" TYPE_LINX_CPU
#define LINX_CPU_TYPE_NAME(model) model LINX_CPU_TYPE_SUFFIX

#define TYPE_LINX_CPU_LINX LINX_CPU_TYPE_NAME("linx")

#ifdef TARGET_LINX64
#define CPU_RESOLVING_TYPE TYPE_LINX_CPU_LINX
#else
#define CPU_RESOLVING_TYPE TYPE_LINX_CPU_LINX
#endif

#endif

