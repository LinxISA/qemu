#ifndef QEMU_LINX_CPU_WRAPPER_H
#define QEMU_LINX_CPU_WRAPPER_H

/*
 * This Linx tree is built as a single-target QEMU variant. Common sources
 * still include a top-level cpu.h, so provide the expected shim and forward
 * it to the active Linx target header.
 */
#ifndef NEED_CPU_H
#define NEED_CPU_H
#endif

#include "target/linx/cpu.h"

#endif /* QEMU_LINX_CPU_WRAPPER_H */
