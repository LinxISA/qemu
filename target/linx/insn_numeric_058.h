/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LINX_INSN_NUMERIC_058_H
#define LINX_INSN_NUMERIC_058_H

#include <stdint.h>

static inline uint64_t linx_hl_lui_value(uint32_t immediate)
{
    return (uint64_t)immediate << 32;
}

static inline uint64_t linx_hl_liu_value(uint32_t immediate)
{
    return immediate;
}

static inline uint64_t linx_hl_lis_value(int32_t immediate)
{
    return (uint64_t)(int64_t)immediate;
}

#endif
