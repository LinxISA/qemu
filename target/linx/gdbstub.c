/*
 * LINX GDB Server Stub
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

#include "qemu/osdep.h"
#include "exec/gdbstub.h"
#include "cpu.h"

int linx_cpu_gdb_read_register(CPUState *cs, GByteArray *mem_buf, int n)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;

    if (n < 16) {
        return gdb_get_regl(mem_buf, env->gpr[n]);
    } else if (n == 16) {
        return gdb_get_regl(mem_buf, env->pc);
    }
    return 0;
}

int linx_cpu_gdb_write_register(CPUState *cs, uint8_t *mem_buf, int n)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;

    if (n == 0) {
        /* discard writes to x0 */
        return sizeof(target_ulong);
    } else if (n < 16) {
        env->gpr[n] = ldtul_p(mem_buf);
        return sizeof(target_ulong);
    } else if (n == 16) {
        env->pc = ldtul_p(mem_buf);
        return sizeof(target_ulong);
    }
    return 0;
}

#if 0
static int linx_gdb_get_fpu(CPULINXState *env, GByteArray *buf, int n)
{
    if (n < 32) {
        if (env->misa_ext & RVD) {
            return gdb_get_reg64(buf, env->fpr[n]);
        }
        if (env->misa_ext & RVF) {
            return gdb_get_reg32(buf, env->fpr[n]);
        }
    /* there is hole between ft11 and fflags in fpu.xml */
    } else if (n < 36 && n > 32) {
        target_ulong val = 0;
        int result;
        /*
         * CSR_FFLAGS is at index 1 in csr_register, and gdb says it is FP
         * register 33, so we recalculate the map index.
         * This also works for CSR_FRM and CSR_FCSR.
         */
        result = linx_csrrw_debug(env, n - 32, &val,
                                   0, 0);
        if (result == LINX_EXCP_NONE) {
            return gdb_get_regl(buf, val);
        }
    }
    return 0;
}

static int linx_gdb_set_fpu(CPULINXState *env, uint8_t *mem_buf, int n)
{
    if (n < 32) {
        env->fpr[n] = ldq_p(mem_buf); /* always 64-bit */
        return sizeof(uint64_t);
    /* there is hole between ft11 and fflags in fpu.xml */
    } else if (n < 36 && n > 32) {
        target_ulong val = ldtul_p(mem_buf);
        int result;
        /*
         * CSR_FFLAGS is at index 1 in csr_register, and gdb says it is FP
         * register 33, so we recalculate the map index.
         * This also works for CSR_FRM and CSR_FCSR.
         */
        result = linx_csrrw_debug(env, n - 32, NULL,
                                   val, -1);
        if (result == LINX_EXCP_NONE) {
            return sizeof(target_ulong);
        }
    }
    return 0;
}
#endif

static int linx_gdb_get_csr(CPULINXState *env, GByteArray *buf, int n)
{
    if (n < SSR_TABLE_SIZE) {
        target_ulong val = 0;
        int result;

        result = linx_csrrw_debug(env, n, &val, 0, 0);
        if (result == LINX_EXCP_NONE) {
            return gdb_get_regl(buf, val);
        }
    }
    return 0;
}

static int linx_gdb_set_csr(CPULINXState *env, uint8_t *mem_buf, int n)
{
    if (n < SSR_TABLE_SIZE) {
        target_ulong val = ldtul_p(mem_buf);
        int result;

        result = linx_csrrw_debug(env, n, NULL, val, -1);
        if (result == LINX_EXCP_NONE) {
            return sizeof(target_ulong);
        }
    }
    return 0;
}

/*
static int linx_gdb_get_virtual(CPULINXState *cs, GByteArray *buf, int n)
{
    if (n == 0) {
#ifdef CONFIG_USER_ONLY
        return gdb_get_regl(buf, 0);
#else
        return gdb_get_regl(buf, cs->priv);
#endif
    }
    return 0;
}

static int linx_gdb_set_virtual(CPULINXState *cs, uint8_t *mem_buf, int n)
{
    if (n == 0) {
#ifndef CONFIG_USER_ONLY
        cs->priv = ldtul_p(mem_buf) & 0x3;
        if (cs->priv == PRV_H) {
            cs->priv = PRV_S;
        }
#endif
        return sizeof(target_ulong);
    }
    return 0;
}
*/

static int linx_gen_dynamic_csr_xml(CPUState *cs, int base_reg)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    GString *s = g_string_new(NULL);
    linx_csr_predicate_fn predicate;
    int bitsize = 64;
    int i;

    g_string_printf(s, "<?xml version=\"1.0\"?>");
    g_string_append_printf(s, "<!DOCTYPE feature SYSTEM \"gdb-target.dtd\">");
    g_string_append_printf(s, "<feature name=\"org.gnu.gdb.linx.csr\">");

    for (i = 0; i < SSR_TABLE_SIZE; i++) {
        predicate = csr_ops[i].predicate;
        if (predicate && (predicate(env, i) == LINX_EXCP_NONE)) {
            if (csr_ops[i].name) {
                g_string_append_printf(s, "<reg name=\"%s\"", csr_ops[i].name);
            } else {
                g_string_append_printf(s, "<reg name=\"csr%03x\"", i);
            }
            g_string_append_printf(s, " bitsize=\"%d\"", bitsize);
            g_string_append_printf(s, " regnum=\"%d\"/>", base_reg + i);
        }
    }

    g_string_append_printf(s, "</feature>");

    cpu->dyn_csr_xml = g_string_free(s, false);
    return SSR_TABLE_SIZE;
}

void linx_cpu_register_gdb_regs_for_features(CPUState *cs)
{
    /* other extension is not used in linx
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;

    if (env->misa_ext & RVD) {
        gdb_register_coprocessor(cs, linx_gdb_get_fpu, linx_gdb_set_fpu,
                                 36, "linx-64bit-fpu.xml", 0);
    } else if (env->misa_ext & RVF) {
        gdb_register_coprocessor(cs, linx_gdb_get_fpu, linx_gdb_set_fpu,
                                 36, "linx-32bit-fpu.xml", 0);
    }

    gdb_register_coprocessor(cs, linx_gdb_get_virtual, linx_gdb_set_virtual,
                             1, "linx-64bit-virtual.xml", 0);
                             */

    gdb_register_coprocessor(cs, linx_gdb_get_csr, linx_gdb_set_csr,
                             linx_gen_dynamic_csr_xml(cs, cs->gdb_num_regs),
                             "linx-csr.xml", 0);
}
