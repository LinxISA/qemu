/*
 * LINX load/store instructions for code (linx-user support)
 *
 *  Copyright (c) 2012 CodeSourcery, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LINX_LDST_H
#define LINX_LDST_H

#include "exec/translator.h"

/*
 * If Target_WORDS_BIGENYAN is y, both instructions and data are read in
 * big-endian mode. However, for LinxISA in big-endian mode, instructions are
 * still little-endian. Therefore, swap is required.
 */

static inline uint64_t linx_ldq_code(CPULINXState *env, DisasContextBase *s,
                                    target_ulong addr)
{
    return
#ifdef TARGET_WORDS_BIGENDIAN
    translator_ldq_swap(env, s, addr, true);
#else
    translator_ldq(env, s, addr);
#endif
}
static inline uint32_t linx_ldl_code(CPULINXState *env, DisasContextBase *s,
                                    target_ulong addr)
{
    return
#ifdef TARGET_WORDS_BIGENDIAN
    translator_ldl_swap(env, s, addr, true);
#else
    translator_ldl(env, s, addr);
#endif
}

static inline uint16_t linx_lduw_code(CPULINXState *env, DisasContextBase* s,
                                     target_ulong addr)
{
    return
#ifdef TARGET_WORDS_BIGENDIAN
    translator_lduw_swap(env, s, addr, true);
#else
    translator_lduw(env, s, addr);
#endif
}

#endif
