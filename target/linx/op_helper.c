/*
 * LINX Emulation Helpers for QEMU.
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
#include "cpu.h"
#include "qemu/main-loop.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "exec/memattrs.h"
#include "exec/memory.h"
#include "exec/log.h"
#include <math.h>
#include "exec/address-spaces.h"
#include "sysemu/runstate.h"
#include "sysemu/runstate-action.h"
#include "sysemu/hw_accel.h"
#include "hw/core/cpu.h"
#include "tcg/tcg.h"

#ifdef CONFIG_USER_ONLY
#include "cpu_user.h"
#endif

typedef enum {
    EXT_ZERO_SIMT,
    EXT_SIGN_SIMT,
    EXT_NONE_SIMT,
} DisasExtend_SIMT;

struct simt_tmp_reg {
    uint32_t ri_idx;
    uint32_t blk_ri[RI_SIZE];
    target_ulong gpr[GPR_REG_SIZE];

    uint64_t fvec_t[CPU_NB_LANE_NUM][FVEC_REG_SIZE];
    uint64_t fvec_u[CPU_NB_LANE_NUM][FVEC_REG_SIZE];
    uint64_t fvec_m[CPU_NB_LANE_NUM][FVEC_REG_SIZE];
    uint64_t fvec_n[CPU_NB_LANE_NUM][FVEC_REG_SIZE];

    uint64_t csr_lc[3];
    uint64_t csr_lb[3];
};

static struct simt_tmp_reg *tmp_reg;

bool is_cache_aligned(CPULINXState *env, target_ulong addr, int size);
void load_store_prepare(CPULINXState *env, target_ulong addr, int size);

target_ulong helper_get_carg_flag(target_ulong c_flag, uint32_t flag)
{
    /* flag[0:7]   => {trap[7],taken[6],br_type[4:5],blktype[0:3]} */
    return deposit32(flag, 6, 1, c_flag);
}

#define MATRIX_LOG_BYTES(dat_type)                          \
{                                                           \
    switch (dat_type) {                                     \
    case E5M2:                                              \
    case E4M3:                                              \
    case E8M0:                                              \
    case INT8:                                              \
    case UINT8:                                             \
    case U4x2:                                             \
    case S4x2:                                             \
    case HiF4x2:                                             \
    case E1M2x2:                                             \
    case E2M1x2:                                             \
        qemu_log("%02x, ", ((uint8_t *)src)[i * n + j]);    \
        break;                                              \
    case FP16:                                              \
    case BF16:                                              \
    case INT16:                                             \
    case UINT16:                                            \
        qemu_log("%04x, ", ((uint16_t *)src)[i * n + j]);   \
        break;                                              \
    case FP32:                                              \
        qemu_log("%4.2f, ", ((float *)src)[i * n + j]);     \
        break;                                              \
    case INT32:                                             \
    case UINT32:                                            \
        qemu_log("%08x, ", ((uint32_t *)src)[i * n + j]);   \
        break;                                              \
    case FP64:                                              \
    case INT64:                                             \
    case UINT64:                                            \
        qemu_log("%016lx, ", ((uint64_t *)src)[i * n + j]); \
        break;                                              \
    default:                                                \
        qemu_log("error data type:%d\n", dat_type);         \
        break;                                              \
    }                                                       \
}

static void helper_matrix_memprnt(CPULINXState *env, target_ulong src,
    uint32_t m, uint32_t n, uint32_t data_type, target_ulong type,
    const char *label)
{
    if (!(qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(env->bpc))) {
        return;
    }

    const char * const type_name[] = {"(MEMR)", "(MEMW)"};
    uint32_t element_bytes = 1;
    switch (data_type) {
    case FP64:
    case INT64:
    case UINT64:
        element_bytes = 8;
        break;
    case FP32:
    case INT32:
    case UINT32:
        element_bytes = 4;
        break;
    case FP16:
    case BF16:
    case INT16:
    case UINT16:
        element_bytes = 2;
        break;
    case E5M2:
    case E4M3:
    case E8M0:
    case INT8:
    case UINT8:
        element_bytes = 1;
        break;
    default:
        element_bytes = 1;
        break;
    }

    qemu_log("%s%016lx ~ %016lx(Element bytes:%d):\n", type_name[type], src,
        src + (m * n * element_bytes), element_bytes);
    qemu_log("%s\nMatrix:%d X %d\n", label, m, n);
    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = 0; j < n; j++) {
            MATRIX_LOG_BYTES(data_type);
        }
        qemu_log("\n");
    }
}

void helper_memprnt_range(target_ulong src, target_ulong size, target_ulong type)
{
    const char * const type_name[] = {"(MEMR)", "(MEMW)"};
    qemu_log("%s%016lx ~ %016lx:\n", type_name[type], src, src + size);
    int offset = 0;
    for (target_ulong addr = src; offset < size; offset++) {
        qemu_log("%02x, ", *(uint8_t *)(addr + offset));
        if (offset % 16 == 15) {
            qemu_log("\n");
        }
    }
    qemu_log("\n");
}

void helper_memprnt(target_ulong addr, target_ulong val, target_ulong type)
{
    if (type == READ) {
        qemu_log("(MEMR)%016lx:0x%lx\n", addr, val);
    } else if (type == WRITE) {
        qemu_log("(MEMW)%016lx:0x%lx\n", addr, val);
    }
}

void helper_vec_print(target_ulong val)
{
    qemu_log("%lx ", val);
}

/* Exceptions processing helpers */
void QEMU_NORETURN linx_raise_exception(CPULINXState *env,
                                          uint32_t exception, uintptr_t pc)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = exception;
    cpu_loop_exit_restore(cs, pc);
}

void helper_raise_exception(CPULINXState *env, uint32_t exception)
{
    linx_raise_exception(env, exception, 0);
}

void helper_mem_check(CPULINXState *env, target_ulong addr)
{
    uint64_t start, end;
    start = (uint64_t)env->tile_reg_t[0];
    end = start + 4 * TILE_REG_SIZE * TILE_REG_MEM;
    if (addr < start || addr >= end) {
        start = (uint64_t) env->s;
        end = start + TILE_REG_MEM;
        if (addr < start || addr >= end) {
            qemu_log("memory addresss error!!!\naddr:0x%lx, "
            "start:0x%lx, end:0x%lx\n", addr, start, end);
            helper_raise_exception(env, LINX_EXCP_INSN_ILLEGAL);
        }
    }
    return;
}

static void linx_debug_dump_memory(CPUState *cs, hwaddr addr, hwaddr size)
{
    qemu_log("dump memory 0x%" PRIx64 "+%lu:\n", addr, size);
#define BUF_SIZE 1024
    int i;
    char buf[BUF_SIZE];
    if (size > BUF_SIZE)
        size = BUF_SIZE;

#ifdef CONFIG_USER_ONLY
    memcpy(buf, (void *)(addr+guest_base), size);
#else
    if (cpu_memory_rw_debug(cs, addr, buf, size, 0) < 0) {
        qemu_log("memory unaccessible");
        return;
    }
#endif

    for (i = 0; i < size; i++) {
        qemu_log("%02" PRIx8 " ", (uint8_t)buf[i]);
        if (i % 16 == 15)
            qemu_log("\n");
     }
     qemu_log("\n");
}

static void dump_guest_string(CPUState *cs, hwaddr addr, char *buf, int buflen)
{
#ifdef CONFIG_USER_ONLY
    qemu_log("todo: not support in user mode yet\n");
#else
    char *ptr = buf;
    int i;

    for(i = 0; i < buflen; ++i) {
        if (cpu_memory_rw_debug(cs, addr, (void *)ptr, 1, 0) < 0) {
            qemu_log("memory unaccessible\n");
            return;
        } else {
            if(*ptr == '\0') {
                break;
            }
            ++ptr;
            ++addr;
        }
    }
#endif
}

/* linx_debug instruction format:
 * para1 [0:9] used [10:31] res [32:41] attr [42:63] res
 * para data
 */
#define LD_ATTR_BIT_STOP_VM        0x001
#define LD_ATTR_BIT_DUMP_STATE     0x002
#define LD_ATTR_BIT_DUMP_MEM       0x004
#define LD_ATTR_BIT_SHOW_ID        0x008
#define LD_ATTR_BIT_DUMP_STRING    0x010
#define LD_ATTR_BIT_PREEMPT_REPORT 0x020
#define LD_ATTR_BIT_LOG_ENABLE     0x040
#define LD_ATTR_BIT_LOG_DISABLE    0x080
void helper_linx_debug(CPULINXState *env, target_ulong attr, target_ulong para)
{
    CPUState *cs = env_cpu(env);

    /*
     * if qemu_loglevel_save is -1, qemu is in formal print log state, otherwise
     * qemu_loglevel_save is used to save qemu_loglevel.
     */

#define BUF_SIZE 1024
    char buf[BUF_SIZE] = { '\0' };

    cpu_synchronize_state(cs);

    if (attr & LD_ATTR_BIT_DUMP_MEM) {
        if (unlikely(qemu_loglevel_mask(CPU_LOG_LINX_DEBUG))) {
            qemu_log("linx_debug(%ld, 0x%lx) mem:\n", env->gpr[xA0],
                                                      para); /* a0 = id */
            linx_debug_dump_memory(cs, (hwaddr)env->gpr[xA1],
                (hwaddr)env->gpr[xA2]); /* (a1, a2) = (address, size) */
        }
    } else if (attr & LD_ATTR_BIT_SHOW_ID) {
        qemu_log_mask(CPU_LOG_LINX_DEBUG, "linx_debug(%ld, 0x%lx) hit\n",
                      env->gpr[xA0], para); /* a0 = id */
    } else if (attr & LD_ATTR_BIT_DUMP_STRING) {
        dump_guest_string(cs, env->gpr[xA1], buf, BUF_SIZE);
        qemu_log_mask(CPU_LOG_LINX_DEBUG, "linx_debug(%ld, 0x%lx) info: %s\n",
                      env->gpr[xA0], para, buf); /* a0 = id */
    } else if (attr & LD_ATTR_BIT_LOG_ENABLE) {
        qemu_log("linx_debug(%ld, 0x%lx) qemu log enabled\n", env->gpr[xA0],
                    para); /* a0 = id */
        qemu_enable_log();
    } else if (attr & LD_ATTR_BIT_LOG_DISABLE) {
        qemu_log("linx_debug(%ld, 0x%lx) qemu log disabled\n", env->gpr[xA0],
                    para); /* a0 = id */
        qemu_disable_log();
    } else
        qemu_log_mask(CPU_LOG_LINX_DEBUG,
            "linx_debug instruction: attr=0x%lx, para=0x%lx\n", attr, para);

    if ((attr & LD_ATTR_BIT_DUMP_STATE) &&
         qemu_loglevel_mask(CPU_LOG_LINX_DEBUG)) {
        qemu_log("linx_debug(%ld, 0x%lx) state:\n", env->gpr[xA0],
                                                    para); /* a0 = id */
        log_cpu_state(cs, CPU_DUMP_CODE);
    }

    if (attr & LD_ATTR_BIT_STOP_VM) {
#ifdef CONFIG_USER_ONLY
        printf("linx_debug(%ld, 0x%lx).LD_ATTR_BIT_STOP_VM: application is suspended for debug\n",
        env->gpr[xA0], para); /* a0 = id */
        linx_raise_exception(env, EXCP_DEBUG, GETPC()+16);
#else
        printf("linx_debug(%ld, 0x%lx): vm stop by linx_debug instruction, use monitor to diagnose\n",
        env->gpr[xA0], para); /* a0 = id */
        vm_stop(RUN_STATE_PAUSED);
#endif
    }

    if (attr & LD_ATTR_BIT_PREEMPT_REPORT) {
        qemu_log("linx_debug(%ld, 0x%lx) preempt_count %ld\n",
        env->gpr[xA0], para, env->gpr[xA1]); /* a0 = id, a1 = preempt_count */
        if (env->gpr[xA1] > 0) {
            cs->debug_state_mask &= ~CPU_DSM_PREEMPT;
        } else {
            cs->debug_state_mask |= CPU_DSM_PREEMPT;
        }
    }
}

/*
 * reg0: input gpr, store the dest addr
 * reg1: input gpr, store the src addr
 * reg2: input gpr, store the size
 */
void helper_block_memmove(CPULINXState *env,
                            uint32_t mreg0, uint32_t mreg1, uint32_t mreg2)
{
    target_ulong des, src, size;

    des = env->gpr[mreg0];
    src = env->gpr[mreg1];
    size = env->gpr[mreg2];
#ifdef CONFIG_USER_ONLY
    des += guest_base;
    src += guest_base;

    size = size > 0 ? size : env->tm_ext >> 5;

    int idx = 0;    /* in user mode, idx always equal 0 */

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(env->bpc)) {
        helper_memprnt_range(src, size, READ);
    }

    memmove((void *)des, (void *)src, size);

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(env->bpc)) {
        helper_memprnt_range(des, size, WRITE);
    }

    /* The vcpu_mem callback is triggered only after the plug-in is loaded. */
    if (!test_bit(QEMU_PLUGIN_EV_VCPU_TB_TRANS,
                  CPU(container_of(env, LINXCPU, env))->plugin_mask)) {
        return;
    }

    int i = 0;
    int off = 128;
    int mem_op = MO_1024;
    while (size) {
        if (size >= 128) {
            off = 128;
            mem_op = MO_1024;
        } else if (size >= 8) {
            off = 8;
            mem_op = MO_64;
        } else {
            off = 1;
            mem_op = MO_8;
        }
        qemu_plugin_vcpu_mem_cb(env_cpu(env), env->gpr[mreg1] + i,
            make_memop_idx(mem_op, idx), QEMU_PLUGIN_MEM_R);
        qemu_plugin_vcpu_mem_cb(env_cpu(env), env->gpr[mreg0] + i,
            make_memop_idx(mem_op, idx), QEMU_PLUGIN_MEM_W);
        size -= off;
        i += off;
    }
#else
    uint64_t data, remain = size;
    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        /* tpc will store the writen size. */
        remain = size - env->tpc;
        des += env->tpc;
        src += env->tpc;
        size = remain;
    }

    /* set in_body to save bstate when an exception occurs. */
    env->in_body = true;
    env->tpc = 0;

    while (remain >= 8) {
        data = cpu_ldq_data(env, src);
        cpu_stq_data_ra(env, des, data, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(src, data, READ);
            helper_memprnt(des, data, WRITE);
        }
        src += 8;
        des += 8;
        env->tpc += 8;
        remain -= 8;
    }

    while (remain) {
        data = cpu_ldub_data(env, src);
        cpu_stb_data_ra(env, des, data, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(src, data, READ);
            helper_memprnt(des, data, WRITE);
        }
        ++src;
        ++des;
        ++env->tpc;
        --remain;
    }

    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
#endif
}

/*
 * reg0: input gpr, store the dest addr
 * reg1: input gpr, store the src addr
 * reg2: input gpr, store the size
 */
void helper_block_memcpy(CPULINXState *env,
                            uint32_t mreg0, uint32_t mreg1, uint32_t mreg2)
{
    target_ulong des, src, size;

    des = env->gpr[mreg0];
    src = env->gpr[mreg1];
    size = env->gpr[mreg2];
#ifdef CONFIG_USER_ONLY
    des += guest_base;
    src += guest_base;

    size = size > 0 ? size : env->tm_ext >> 5;

    int idx = 0;    /* in user mode, idx always equal 0 */

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(env->bpc)) {
        helper_memprnt_range(src, size, READ);
    }

    memcpy((void *)des, (void *)src, size);

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(env->bpc)) {
        helper_memprnt_range(des, size, WRITE);
    }

    /* The vcpu_mem callback is triggered only after the plug-in is loaded. */
    if (!test_bit(QEMU_PLUGIN_EV_VCPU_TB_TRANS, CPU(container_of(env, LINXCPU, env))->plugin_mask)) {
        return;
    }

    int i = 0;
    int off = 128;
    int mem_op = MO_1024;
    while (size) {
        if (size >= 128) {
            off = 128;
            mem_op = MO_1024;
        } else if (size >= 8) {
            off = 8;
            mem_op = MO_64;
        } else {
            off = 1;
            mem_op = MO_8;
        }
        qemu_plugin_vcpu_mem_cb(env_cpu(env), env->gpr[mreg1] + i,
            make_memop_idx(mem_op, idx), QEMU_PLUGIN_MEM_R);
        qemu_plugin_vcpu_mem_cb(env_cpu(env), env->gpr[mreg0] + i,
            make_memop_idx(mem_op, idx), QEMU_PLUGIN_MEM_W);
        size -= off;
        i += off;
    }
#else
    uint64_t data, remain = size;
    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        /* tpc will store the writen size. */
        remain -= env->tpc;
        des += env->tpc;
        src += env->tpc;
        size = remain;
    }

    /* set in_body to save bstate when an exception occurs. */
    env->in_body = true;
    env->tpc = 0;

    while (remain >= 8) {
        data = cpu_ldq_data(env, src);
        cpu_stq_data_ra(env, des, data, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(src, data, READ);
            helper_memprnt(des, data, WRITE);
        }
        src += 8;
        des += 8;
        env->tpc += 8;
        remain -= 8;
    }

    while (remain) {
        data = cpu_ldub_data(env, src);
        cpu_stb_data_ra(env, des, data, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(src, data, READ);
            helper_memprnt(des, data, WRITE);
        }
        ++src;
        ++des;
        ++env->tpc;
        --remain;
    }

    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
#endif
}

/*
 * reg0: input gpr, store the dest addr
 * reg1: input gpr, store the src data
 * reg2: input gpr, store the size
 */
void helper_block_memset(CPULINXState *env, uint32_t mreg0, uint32_t mreg1,
                            uint32_t mreg2)
{
    char ch;
    target_ulong des, size;
    ch = env->gpr[mreg1] & 0xff;
    des = env->gpr[mreg0];
    size = env->gpr[mreg2];
#ifdef CONFIG_USER_ONLY
    des += guest_base;

    size = size > 0 ? size : env->tm_ext >> 5;

    int idx = 0;    /* in user mode, idx always equal 0 */

    memset((void *)des, ch, size);

    if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
        qemu_log_in_addr_range(env->bpc)) {
        helper_memprnt_range(des, size, WRITE);
    }

    /* The vcpu_mem callback is triggered only after the plug-in is loaded. */
    if (!test_bit(QEMU_PLUGIN_EV_VCPU_TB_TRANS, CPU(container_of(env, LINXCPU, env))->plugin_mask)) {
        return;
    }

    int i = 0;
    int off = 128;
    int mem_op = MO_1024;
    while (size) {
        if (size >= 128) {
            off = 128;
            mem_op = MO_1024;
        } else if (size >= 8) {
            off = 8;
            mem_op = MO_64;
        } else {
            off = 1;
            mem_op = MO_8;
        }
        qemu_plugin_vcpu_mem_cb(env_cpu(env), env->gpr[mreg0] + i,
            make_memop_idx(mem_op, idx), QEMU_PLUGIN_MEM_W);
        size -= off;
        i += off;
    }
#else
    uint64_t tmp_data, remain = size;
    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        /* tpc will store the writen size. */
        remain -= env->tpc;
        des += env->tpc;
        size = remain;
    }

    /* set in_body to save bstate when an exception occurs. */
    env->in_body = true;
    env->tpc = LINX_ILLEGAL_INSTR_ADDR;

    /* write with 64-bit */
    while (remain >= 8) {
        tmp_data = ((uint64_t) ch << 56) | ((uint64_t) ch << 48) |
                   ((uint64_t) ch << 40) | ((uint64_t) ch << 32) |
                   ((uint64_t) ch << 24) | ((uint64_t) ch << 16) |
                   ((uint64_t) ch << 8)  | ((uint64_t) ch);
        cpu_stq_data_ra(env, des, tmp_data, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(des, tmp_data, WRITE);
        }
        des += 8;
        env->tpc += 8;
        remain -= 8;
    }

    /* processing the remaining bytes */
    while (remain) {
        cpu_stb_data_ra(env, des, ch, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(des, ch, WRITE);
        }
        ++des;
        ++env->tpc;
        --remain;
    }

    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
#endif
}


void helper_block_fentry(CPULINXState *env,
                         uint32_t src_begin, uint32_t src_end, int32_t imm)
{
    target_ulong target_addr, target_original, written_num = 0;

    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        written_num = env->tpc - src_begin;
        src_begin = env->tpc;
    }

    /* set in_body to save bstate when an exception occurs. */
    env->in_body = true;
    env->tpc = src_begin;

    target_original = env->gpr[xSP];
    target_addr = target_original - (written_num * 8);

    assert(src_begin <= src_end);

    for (int i = src_begin; i <= src_end; i++) {
        target_addr -= 8;
        env->tpc = i;
        cpu_stq_data_ra(env, target_addr, env->gpr[i], GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(target_addr, env->gpr[i], WRITE);
        }
    }
    env->gpr[xSP] = target_original - imm;
    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
}


void helper_block_fexit(CPULINXState *env,
                        uint32_t dst_begin, uint32_t dst_end, int32_t imm)
{
    target_ulong target_addr, target_original, read_num = 0;

    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        read_num = env->tpc - dst_begin;
        dst_begin = env->tpc;
    }

    /* set in_body to save bstate when an exception occurs. */
    env->in_body = true;
    env->tpc = dst_begin;

    target_original = env->gpr[xSP];
    target_addr =  env->gpr[xSP] + imm - (read_num * 8);

    assert(dst_begin <= dst_end);

    for (int i = dst_begin; i <= dst_end; i++) {
        target_addr -= 8;
        env->tpc = i;
        env->gpr[i] = cpu_ldq_data_ra(env, target_addr, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(target_addr, env->gpr[i], READ);
        }
    }

    env->gpr[xSP] = target_original + imm;
    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
}

void helper_block_fret_ra(CPULINXState *env,
                          uint32_t dst_begin, uint32_t dst_end, int32_t imm)
{
    target_ulong target_addr, target_original, read_num = 0;

    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        read_num = env->tpc - dst_begin;
        dst_begin = env->tpc;
    }

    /* set in_body to save bstate when an exception occurs. */
    env->in_body = true;
    env->tpc = dst_begin;

    env->carg_tgt = env->gpr[xRA];
    target_original = env->gpr[xSP];
    target_addr = target_original + imm - (read_num * 8);

    assert(dst_begin <= dst_end);

    for (int i = dst_begin; i <= dst_end; i++) {
        target_addr -= 8;
        env->gpr[i] = cpu_ldq_data_ra(env, target_addr, GETPC());
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(target_addr, env->gpr[i], READ);
        }
    }

    env->gpr[xSP] = target_original + imm;
    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
}

void helper_block_fret_stk(CPULINXState *env,
                           uint32_t dst_begin, uint32_t dst_end, int32_t imm)
{
    target_ulong target_addr, target_original, read_num = 0, ra_idx;

    ra_idx = dst_begin;

    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        read_num = env->tpc - dst_begin;
        dst_begin = env->tpc;
    }

    /* set in_body to save bstate when an exception occurs. */
    env->in_body = true;
    env->tpc = dst_begin;

    target_original = env->gpr[xSP];
    target_addr = target_original + imm - (read_num * 8);

    assert(dst_begin <= dst_end);

    for (int i = dst_begin; i <= dst_end; i++) {
        target_addr -= 8;
        env->tpc = i;
        env->gpr[i] = cpu_ldq_data_ra(env, target_addr, GETPC());
        if (i == ra_idx) {
            /*
             * Preserve the restored return target as soon as RA is reloaded.
             * If an exception or interrupt lands later in this template block,
             * ebpcn must carry the return target rather than an older carg_tgt.
             */
            env->carg_tgt = env->gpr[ra_idx];
        }
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            helper_memprnt(target_addr, env->gpr[i], READ);
        }
    }

    env->carg_tgt = env->gpr[ra_idx];
    env->gpr[xSP] = target_original + imm;
    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
}

static target_ulong get_src_regx_fvec_extx(target_ulong src, int src_width,
                                   DisasExtend_SIMT ext)
{
    target_ulong t;
    switch (src_width) {
    case WIDTH_BYTE:
        if (ext == EXT_SIGN_SIMT) {
            t = (int8_t)src;
        } else if (ext == EXT_ZERO_SIMT) {
            t = (uint8_t)src;
        } else {
            g_assert_not_reached();
        }
        break;
    case WIDTH_HALF:
        if (ext == EXT_SIGN_SIMT) {
            t = (int16_t)src;
        } else if (ext == EXT_ZERO_SIMT) {
            t = (uint16_t)src;
        } else {
            g_assert_not_reached();
        }
        break;
    case WIDTH_WORD:
        if (ext == EXT_SIGN_SIMT) {
            t = (int32_t)src;
        } else if (ext == EXT_ZERO_SIMT) {
            t = (uint32_t)src;
        } else {
            g_assert_not_reached();
        }
        break;
    case WIDTH_DOUBLE:
        return src;
        break;
    default:
        g_assert_not_reached();
    }
    return t;
}

static target_ulong get_src_regx_fvec(target_ulong src_code,
                                      target_ulong lane_id)
{
    int src = extract16(src_code, 0, 7);
    int offset = src & FVEC_REG_IDX_MASK;
    if ((src >= SRC_FVEC_VT_1 && src <= SRC_FVEC_VT_4) ||
        (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4)) {
        return tmp_reg->fvec_t[lane_id][offset];
    } else if ((src >= SRC_FVEC_VU_1 && src <= SRC_FVEC_VU_4) ||
        (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4)) {
        return tmp_reg->fvec_u[lane_id][offset];
    } else if ((src >= SRC_FVEC_VM_1 && src <= SRC_FVEC_VM_4) ||
        (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4)) {
        return tmp_reg->fvec_m[lane_id][offset];
    } else if ((src >= SRC_FVEC_VN_1 && src <= SRC_FVEC_VN_4) ||
        (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4)) {
        return tmp_reg->fvec_n[lane_id][offset];
    } else if (src >= SRC_FVEC_RI0 && src <= SRC_FVEC_RI11) {
        return tmp_reg->gpr[tmp_reg->blk_ri[src - SRC_FVEC_RI0]];
    } else {
        switch (src) {
        case SRC_FVEC_LC0:
            /*
             * After updating lc0/1/2 in helper_update_lcreg(),
             * they were then copied to tmp_reg
             */
            return tmp_reg->csr_lc[0];
        case SRC_FVEC_LC1:
            return tmp_reg->csr_lc[1];
        case SRC_FVEC_LC2:
            return tmp_reg->csr_lc[2];
        case SRC_FVEC_LB0:
            return tmp_reg->csr_lb[0];
        case SRC_FVEC_LB1:
            return tmp_reg->csr_lb[1];
        case SRC_FVEC_LB2:
            return tmp_reg->csr_lb[2];
        default:
            g_assert_not_reached();
            break;
        }
    }
}

static void copy_fvec_data(CPULINXState *env, target_ulong cur_lane_id)
{
    if (cur_lane_id == 0) {
        tmp_reg = malloc(sizeof(struct simt_tmp_reg));
        int i, j;
        tmp_reg->ri_idx = env->ri_idx;
        for (i = 0; i < RI_SIZE; i++) {
            tmp_reg->blk_ri[i] = env->blk_ri[i];
        }
        for (i = 0; i < GPR_REG_SIZE; i++) {
            tmp_reg->gpr[i] = env->gpr[i];
        }
        for (i = 0; i < CPU_NB_LANE_NUM; i++) {
            for (j = 0; j < FVEC_REG_SIZE; j++) {
                tmp_reg->fvec_t[i][j] = env->fvec_t[i][j];
                tmp_reg->fvec_u[i][j] = env->fvec_u[i][j];
                tmp_reg->fvec_m[i][j] = env->fvec_m[i][j];
                tmp_reg->fvec_n[i][j] = env->fvec_n[i][j];
            }
        }
        helper_update_lcreg(env, cur_lane_id);
        for (i = 0; i < 3; ++i) {
            tmp_reg->csr_lc[i] = env->csr_lc[i];
            tmp_reg->csr_lb[i] = env->csr_lb[i];
        }
    }
}

void helper_dma(target_ulong des, target_ulong src)
{
#ifdef CONFIG_USER_ONLY
    des += guest_base;
    src += guest_base;

    uint32_t size = 64;

    memcpy((void *)des, (void *)src, size);
#endif
    return;
}

static target_ulong get_src_regx_fvec_with_width(target_ulong src_code,
                                                 target_ulong lane_id)
{
    int src_width = extract16(src_code, 7, 2);
    int ext = extract16(src_code, 9, 1);
    target_ulong src_reg = get_src_regx_fvec(src_code, lane_id);
    return get_src_regx_fvec_extx(src_reg, src_width, ext);
}

target_ulong helper_shfl_up_set_dest(CPULINXState *env, target_ulong srcL,
                                      target_ulong cur_lane_id,
                                      target_ulong src_lane_id)
{
    target_ulong lane_id = (int32_t)src_lane_id < 0 ? cur_lane_id : src_lane_id;
    copy_fvec_data(env, cur_lane_id);
    return get_src_regx_fvec_with_width(srcL, lane_id);
}

target_ulong helper_shfl_down_set_dest(CPULINXState *env, target_ulong srcR_val,
                                      target_ulong srcL,
                                      target_ulong cur_lane_id,
                                      target_ulong src_lane_id)
{
    target_ulong lane_id = src_lane_id < srcR_val ? src_lane_id : cur_lane_id;
    copy_fvec_data(env, cur_lane_id);
    return get_src_regx_fvec_with_width(srcL, lane_id);
}

target_ulong helper_shfl_set_dest(CPULINXState *env, target_ulong srcL,
                                  target_ulong cur_lane_id,
                                  target_ulong src_lane_id)
{
    copy_fvec_data(env, cur_lane_id);
    return get_src_regx_fvec_with_width(srcL, src_lane_id);
}

void helper_simt_push_cstk(CPULINXState *env, target_ulong rpc,
                           target_ulong else_pc, target_ulong pred_mask)
{
    /* We push the rpc at last, so that pc.pop can easily check rpc. */
    g_queue_push_tail(env->cstk, (gpointer)pred_mask);
    g_queue_push_tail(env->cstk, (gpointer)else_pc);
    g_queue_push_tail(env->cstk, (gpointer)rpc);
}

target_ulong helper_simt_pop_cstk(CPULINXState *env)
{
    return (target_ulong)g_queue_pop_tail(env->cstk);
}

target_ulong helper_simt_peek_cstk(CPULINXState *env)
{
    return (target_ulong)g_queue_peek_tail(env->cstk);
}

void helper_update_lcreg(CPULINXState *env, uint32_t lane_id)
{
    target_ulong sum = env->csr_lc_sum + lane_id;
    env->csr_lc[0] = sum % env->csr_lb[0];
    env->csr_lc[1] = ((sum / env->csr_lb[0])) % env->csr_lb[1];
    env->csr_lc[2] = sum % (env->csr_lb[0] * env->csr_lb[1]);
}

target_ulong helper_update_predm(CPULINXState *env)
{
    env->csr_lc_sum += env->enable_lane_num;
    int remain_lane = env->csr_lb_sum - env->csr_lc_sum;
    if (!remain_lane) {
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM)) {
            int i = 0;
            qemu_log("to:0x%lx\n", env->to);
            for (; env->tile_reg_dst_num > 0 &&
                   i < env->tile_attr[0].size / 8; ++i) {
                qemu_log("%lx ", ((uint64_t *)env->to)[i]);
            }
            qemu_log("\n");
        }
        return 0;
    }
    remain_lane = env->csr_lb[0] - (env->csr_lc_sum % env->csr_lb[0]);
    remain_lane = remain_lane > env->csr_lanenum ?
                    env->csr_lanenum : remain_lane;

    env->enable_lane_num = remain_lane;
    env->predm = MAKE_64BIT_MASK(0, remain_lane);
    return 1;
}

target_ulong helper_get_ri_gpr(CPULINXState *env, uint32_t idx)
{
    if (idx >= env->ri_idx) {
        qemu_log("RI%d exceeds BIOR input para list, bpc: 0x%lx, tpc: 0x%lx\n",
            idx, env->bpc, env->pc);
        helper_raise_exception(env, LINX_EXCP_INSN_ILLEGAL);
    }
    return env->gpr[env->blk_ri[idx]];
}

target_ulong helper_get_ro_gpr(CPULINXState *env, uint32_t idx)
{
    if (idx >= env->ro_idx) {
        qemu_log("RO%d exceeds BIOR input para list, bpc: 0x%lx, tpc: 0x%lx\n",
            idx, env->bpc, env->pc);
        helper_raise_exception(env, LINX_EXCP_INSN_ILLEGAL);
    }
    return env->gpr[env->blk_ro[idx]];
}

void helper_set_ro_gpr(CPULINXState *env, target_ulong data, uint32_t idx)
{
    if (idx >= env->ro_idx) {
        qemu_log("RO%d exceeds BIOR output para list, bpc: 0x%lx, tpc: 0x%lx\n",
            idx, env->bpc, env->pc);
        helper_raise_exception(env, LINX_EXCP_INSN_ILLEGAL);
    }
    env->gpr[env->blk_ro[idx]] = data;
}

static uint64_t get_tile_src(CPULINXState *env, int src_code)
{
    uint64_t *ret = 0;
    int idx = 0;
    uint32_t offset = src_code & (TILE_REG_LOGICAL_SIZE - 1);

    if (src_code >= 0 && src_code < 16) {
        idx = (env->tile_reg_t_idx - offset - 1) % TILE_REG_SIZE;
        ret = env->tile_reg_t[idx];
    } else if (src_code >= 16 && src_code < 32) {
        idx = (env->tile_reg_u_idx - offset - 1) % TILE_REG_SIZE;
        ret = env->tile_reg_u[idx];
    } else if (src_code >= 32 && src_code < 48) {
        idx = (env->tile_reg_m_idx - offset - 1) % TILE_REG_SIZE;
        ret = env->tile_reg_m[idx];
    } else if (src_code >= 48 && src_code < 64) {
        idx = (env->tile_reg_n_idx - offset - 1) % TILE_REG_SIZE;
        ret = env->tile_reg_n[idx];
    } else {
        qemu_log("Tile Register Number Error! Error Number:%d\n", src_code);
        assert(0);
    }
    return (uint64_t)ret;
}

static bool get_tile_attr(CPULINXState *env, int src_code, TileAttr *ret)
{
    int idx = 0;
    uint32_t offset = src_code & (TILE_REG_LOGICAL_SIZE - 1);

    if (src_code >= 0 && src_code < 16) {
        idx = (env->tile_reg_t_idx - offset - 1) % TILE_REG_SIZE;
        *ret = env->tile_attr_t[idx];
    } else if (src_code >= 16 && src_code < 32) {
        idx = (env->tile_reg_u_idx - offset - 1) % TILE_REG_SIZE;
        *ret = env->tile_attr_u[idx];
    } else if (src_code >= 32 && src_code < 48) {
        idx = (env->tile_reg_m_idx - offset - 1) % TILE_REG_SIZE;
        *ret = env->tile_attr_m[idx];
    } else if (src_code >= 48 && src_code < 64) {
        idx = (env->tile_reg_n_idx - offset - 1) % TILE_REG_SIZE;
        *ret = env->tile_attr_n[idx];
    } else {
        return false;
    }
    return true;
}

static uint64_t set_tile_src(CPULINXState *env, int dst_code, TileAttr *in)
{
    uint64_t *ret = 0;
    if (dst_code == 0) {
        ret = env->tile_reg_t[env->tile_reg_t_idx % TILE_REG_SIZE];
        env->tile_attr_t[env->tile_reg_t_idx % TILE_REG_SIZE] = *in;
        env->tile_reg_t_idx++;
    } else if (dst_code == 1) {
        ret = env->tile_reg_u[env->tile_reg_u_idx % TILE_REG_SIZE];
        env->tile_attr_u[env->tile_reg_u_idx % TILE_REG_SIZE] = *in;
        env->tile_reg_u_idx++;
    } else if (dst_code == 2) {
        ret = env->tile_reg_m[env->tile_reg_m_idx % TILE_REG_SIZE];
        env->tile_attr_m[env->tile_reg_m_idx % TILE_REG_SIZE] = *in;
        env->tile_reg_m_idx++;
    } else if (dst_code == 3) {
        ret = env->tile_reg_n[env->tile_reg_n_idx % TILE_REG_SIZE];
        env->tile_attr_n[env->tile_reg_n_idx % TILE_REG_SIZE] = *in;
        env->tile_reg_n_idx++;
    } else if (dst_code == 4) {
        ret = env->acc;
    } else if (dst_code == 5) {
        ret = env->s;
        env->to1 = (uint64_t)env->s;
    } else {
        qemu_log("set tile src error dst code: %d\n", dst_code);
        linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
    }
    return (uint64_t)ret;
}

static SrcType get_acc_dtyp(SrcType s_typ)
{
    switch (s_typ) {
    case INT64:
    case INT32:
    case INT16:
    case INT8:
        return INT32;
    case UINT32:
    case UINT64:
    case UINT16:
    case UINT8:
    case U4x2:
    case S4x2:
        return UINT32;
    case BF16:
    case E5M2:
    case E4M3:
    case FP16:
    case FP32:
    case FP64:
    case HiF4x2:
    case E2M1x2:
    case E1M2x2:
        return FP32;
    default:
        assert(0);
    }
    return FP32;
}

static uint64_t get_blk_srctyp(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_SRCTYP_START,
                     HEADER_INFO_SRCTYP_LEN);
}

static uint32_t get_dst_tile_type(CPULINXState *env)
{
    uint32_t dat_typ = env->tileop_info.tileop_datatype;
    switch (env->tileop_info.tileop_type) {
    case TILEOP_TCVT:
        dat_typ = get_blk_srctyp(env->header_info);
        break;
    case TILEOP_MAMULB:
    case TILEOP_MAMULBAC:
    case TILEOP_MAMULBMX:
    case TILEOP_MAMULBMXAC:
        dat_typ = get_acc_dtyp(dat_typ);
        break;
    case TILEOP_TLOAD:
        dat_typ = env->tileop_info.tileop_datatype;
        break;
    }
    return dat_typ;
}

void helper_biot(CPULINXState *env, int reg,
    int reg_num, int imm, int reg_type)
{
    if (reg_type == TILE_REG_SRC) {
        uint64_t tile_reg;
        TileAttr attr;
        get_tile_attr(env, reg_num, &attr);
        tile_reg = get_tile_src(env, reg_num);

        switch (env->tile_reg_src_num) {
        case 0:
            env->ta = tile_reg;
            env->ta_a = attr;
            break;
        case 1:
            env->tb = tile_reg;
            env->tb_a = attr;
            break;
        case 2:
            env->tc = tile_reg;
            env->tc_a = attr;
            break;
        case 3:
            env->td = tile_reg;
            env->td_a = attr;
            break;
        case 4:
            env->te = tile_reg;
            env->te_a = attr;
            break;
        case 5:
            env->tf = tile_reg;
            env->tf_a = attr;
            break;
        case 6:
            env->tg = tile_reg;
            env->tg_a = attr;
            break;
        case 7:
            env->th = tile_reg;
            env->th_a = attr;
            break;
        default:
            linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
            break;
        }
        env->tile_reg_src_num++;
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            qemu_log(" input tile reg: 0x%lx, src num: %d\n",
                      tile_reg, env->tile_reg_src_num);
        }
    } else {
        /*
         * B.IOTI [t#3, t#2], group=0, ->t<128B>
         * B.IOTI [t#1], group=1
         * To prevent t#1 in the 2nd B.IOTI from indexing into the output
         * tileReigster(->t) of the 1st B.IOTI, the retrieval of env->to needs
         * to be deferred to the implementation of "jumping to the body"
         */
        env->dsttile[env->tile_reg_dst_num] = reg_num;

        uint32_t dst_size = imm == 0 ? 0 : 32 * (1 << (imm - 1));

        env->tile_attr[env->tile_reg_dst_num].size = dst_size;
        env->tile_attr[env->tile_reg_dst_num].dtyp = get_dst_tile_type(env);
        if (!dst_size) {
            switch(reg_num) {
                case 0: {
                    env->tile_reg_t_idx++;
                } break;
                case 1: {
                    env->tile_reg_u_idx++;
                } break;
                case 2: {
                    env->tile_reg_m_idx++;
                } break;
                case 3: {
                    env->tile_reg_n_idx++;
                } break;
            }
        }
        if (env->tile_attr[env->tile_reg_dst_num].size > TILE_REG_MEM) {
            printf("pc:0x%lx\ndest tile size 0x%x exceeds 32kb!\n",
                    env->pc, env->tile_attr[env->tile_reg_dst_num].size);
            linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
        }
        env->tile_reg_dst_num++;
    }
    return;
}

static void init_tile_outupt(CPULINXState *env)
{
    for (int i = 0; i < env->tile_reg_dst_num; ++i)  {
        uint32_t dsttile = env->dsttile[i];
        TileAttr *attr = &(env->tile_attr[i]);
        switch (i) {
        case 0: {
            env->to = set_tile_src(env, dsttile, attr);
        } break;
        case 1: {
            env->to1 = set_tile_src(env, dsttile, attr);
        } break;
        case 2: {
            env->to2 = set_tile_src(env, dsttile, attr);
        } break;
        case 3: {
            env->to3 = set_tile_src(env, dsttile, attr);
        } break;
        case 4: {
            env->to4 = set_tile_src(env, dsttile, attr);
        } break;
        case 5: {
            env->to5 = set_tile_src(env, dsttile, attr);
        } break;
        case 6: {
            env->to6 = set_tile_src(env, dsttile, attr);
        } break;
        case 7: {
            env->to7 = set_tile_src(env, dsttile, attr);
        } break;
        default:
            assert(0);
        }

        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM) &&
            qemu_log_in_addr_range(env->pc)) {
            qemu_log("output tile reg: 0x%lx, reg_num: %d\n",
                        env->to, env->tile_reg_dst_num);
        }
    }
    return;
}

target_ulong helper_enable_loop(CPULINXState *env)
{
    env->csr_lb_sum = env->csr_lb[0] * env->csr_lb[1] * env->csr_lb[2];
    uint64_t remain_lane = env->csr_lb[0] < env->csr_lb_sum ?
        env->csr_lb[0] : env->csr_lb_sum;
    env->predm = MAKE_64BIT_MASK(0, remain_lane);
    if (!env->csr_lb[0] && !env->csr_lb[1] && !env->csr_lb[2]) {
        return 0;
    }
    env->csr_lc_sum = 0;
    init_tile_outupt(env);
    return 1;
}

target_ulong helper_tile_load(target_ulong addr, int memop)
{
    target_ulong data = 0;
    int is_sign, size;

    size = memop_size(memop);
    is_sign = (memop & MO_SIGN) ? 1 : 0;

    memcpy(&data, (char *)addr, size);
    if (is_sign) {
        return sextract64(data, 0, size * 8);
    } else {
        return extract64(data, 0, size * 8);
    }
}

void helper_tile_store(target_ulong addr, target_ulong data, int size)
{
    memcpy((char *)addr, &data, size);
}

target_ulong helper_reverse_bits(target_ulong src, uint32_t M, uint32_t N)
{
    uint64_t result = 0;
    uint8_t total_bits = 64;
    /* 计算块数 */
    uint8_t blocks = total_bits / M;
    for (uint8_t block = 0; block < blocks; block++) {
        /*  提取当前 M 位块 */
        uint64_t shift = (M >= 64) ? ~0ULL : (1ULL << M) - 1;
        uint64_t block_data = (src >> (block * M)) & shift;
        uint64_t reversed_block = 0;
        /* 翻转当前块内的 N 位单位 */
        for (uint8_t i = 0; i < M; i += N) {
            uint64_t mask = (1ULL << N) - 1;
            uint64_t chunk = (block_data >> i) & mask;
            reversed_block |= (chunk << (M - N - i));
        }
        /* 合并到结果 */
        result |= (reversed_block << (block * M));
    }
    return result;
}

void helper_matmul(CPULINXState *env, uint32_t tsp, uint32_t dst,
                       uint32_t src0, uint32_t src1, uint32_t src2)
{
   return;
}

inline uint64_t get_dr(uint64_t header_info)
{
    return header_info & HEADER_INFO_DR_MASK;
}

inline uint64_t get_aq(uint64_t header_info)
{
    return header_info & HEADER_INFO_AQ_MASK;
}

inline uint64_t get_rl(uint64_t header_info)
{
    return header_info & HEADER_INFO_RL_MASK;
}

inline uint64_t get_blktype(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_BLKTYPE_START,
                     HEADER_INFO_BLKTYPE_LEN);
}

inline uint64_t get_brhtype(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_BRHTYPE_START,
                     HEADER_INFO_BRHTYPE_LEN);
}

inline uint64_t get_blkdcp(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_DECOUPLE_START,
                     HEADER_INFO_DECOUPLE_LEN);
}

inline uint64_t get_blk_atomic(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_ATOMIC_START,
                     HEADER_INFO_ATOMIC_LEN);
}

static uint64_t get_blk_format(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_FORMAT_START,
                     HEADER_INFO_FORMAT_LEN);
}

static uint64_t get_blk_pad(uint64_t header_info)
{
    return extract64(header_info, HEADER_INFO_PAD_START,
                             HEADER_INFO_PAD_LEN);
}

typedef enum {
    FILL_ZERO,
    FILL_MAX,
    FILL_MIN,
    FILL_NULL,
} PadMode;

static uint64_t get_pad_value(uint64_t pad, SrcType dataType)
{
    const uint64_t type_max_num[32] = {0x7FEFFFFFFFFFFFFF, 0x7F7FFFFF, 0, 0,
    0x7BFF, 0, 0, 0x7e, 0x7B, 0, 0, 0, 0, 0, 0, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFF,
     0x7FFF, 0x7F, 0x7, 0, 0, 0, 0, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF, 0xFFFF,
    0xFF, 0xF};
    const uint64_t type_min_num[32] = {0xFFEFFFFFFFFFFFFF, 0xFF7FFFFF, 0, 0,
    0xFBFF, 0, 0, 0xFE, 0xFB, 0, 0, 0, 0, 0, 0, 0x8000000000000000,
    0x80000000, 0x8000, 0x80, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0};
    uint64_t p_val;
    switch (pad) {
    case FILL_ZERO:
    case FILL_NULL:
        p_val = 0;
        break;
    case FILL_MAX: {
        p_val = type_max_num[dataType];
    } break;
    case FILL_MIN: {
        p_val = type_min_num[dataType];
    } break;
    default:
        assert(0);
    }
    return p_val;
}

static uint64_t get_blk_rmode(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_RMODE_START,
                     HEADER_INFO_RMODE_LEN);
}

static uint64_t get_canon(uint64_t header_info)
{
    return extract64(header_info,
                     HEADER_INFO_CANON_START,
                     HEADER_INFO_CANON_LEN);
}

/* the hash equality function for uint64 */
static gboolean uint64_t_equal(gconstpointer v1, gconstpointer v2)
{
  return *((const uint64_t *) v1) == *((const uint64_t *) v2);
}

/* the hash function for uint64 */
static guint uint64_t_hash(gconstpointer v)
{
  return (guint) *(const uint64_t *) v;
}

bool is_first_happened(uint64_t key)
{
    static GHashTable *table;

    if (table == NULL) {
        table = g_hash_table_new(uint64_t_hash, uint64_t_equal);
    }

    uint64_t *value = g_hash_table_lookup(table, &key);
    if (value == NULL) { /* first */
        uint64_t *v = g_new(uint64_t, 1);
        g_hash_table_insert(table, &key, v);
        return true;
    } else {    /* not first */
        return false;
    }
}

void helper_dynamic_reg_valid_check(CPULINXState *env, target_ulong tpc,
                                    uint32_t lane_id)
{
    qemu_log("\nThe register is not valid, tpc: 0x%lx, lane_id: %d\n",
            tpc, lane_id);
    linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
}

void helper_dynamic_reg_width_check(CPULINXState *env, target_ulong tpc,
                                    uint32_t lane_id)
{
    qemu_log("\nThe register width is not same, tpc: 0x%lx, lane_id: %d\n",
            tpc, lane_id);
    linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
}

void helper_handle_exec_and_branch(CPULINXState *env)
{
    switch (get_blktype(env->header_info)) {
    case HEAD_TYPE_STD:
    case HEAD_TYPE_FP:
    case HEAD_TYPE_MCOPY:
    case HEAD_TYPE_MSET:
    case HEAD_TYPE_MPUSH:
    case HEAD_TYPE_MPOP:
    case HEAD_TYPE_FENTRY:
    case HEAD_TYPE_FEXIT:
    case HEAD_TYPE_FRET_RA:
    case HEAD_TYPE_FRET_STK:
    case HEAD_TYPE_SYS:
        switch (get_brhtype(env->header_info)) {
        case BRANCH_DIRECT_LINK:
            env->pc = env->bpc + env->bnext;
            linx_reset_bstate(env);
            break;
        case BRANCH_CALL:
            env->gpr[xA0] = env->next_bpc;
            env->pc = env->bpc + env->bnext;
            linx_reset_bstate_short(env);
            break;
        case BRANCH_CONDITIONAL:
            if (env->carg_flag & CARG_FLAG_PREDICATE) {
                env->pc = env->bpc + env->bnext;
            } else {
                env->pc = env->next_bpc;
            }
            linx_reset_bstate_short(env);
            assert(0);
            break;
        case BRANCH_FALL:
            env->pc = env->next_bpc;
            linx_reset_bstate(env);
            break;
        case BRANCH_IND:
            env->pc = env->carg_tgt;
            linx_reset_bstate_short(env);
            break;
        case BRANCH_INDCALL:
            env->gpr[xA0] = env->next_bpc;
            env->pc = env->carg_tgt;
            linx_reset_bstate_short(env);
            break;
        case BRANCH_RET:
            env->pc = env->carg_tgt;
            linx_reset_bstate_short(env);
            break;
        default:
            linx_debug_not_reached();
        }
        break;
    default:
         linx_debug_not_reached();
    }
}

void helper_jump_to_atomic_context(CPULINXState *env)
{
    cpu_loop_exit_atomic_blk(env_cpu(env), 0);
}

void helper_clear_store_buf(CPULINXState *env)
{
    uint8_t i;

    for (i = 0; i < 8; i++) {
        env->store_buf[i] = 0;
    }
    env->store_addr = 0;
    env->store_addr_valid = 0;
}

void helper_set_next_flags(CPULINXState *env, uint32_t cflags)
{
    CPUState *cs = env_cpu(env);
    cs->cflags_next_tb = curr_cflags(cs) | cflags;
}

/* This helper is just kept here in case we want to add tracker in the exec context */
void helper_log(CPULINXState *env, target_ulong id)
{
    CPUState *cs = env_cpu(env);
    qemu_log("linx-exec-log(0x%lx): cflags_next_tb=%x, tpc1=%lx\n",
            id, cs->cflags_next_tb, env->tpc1);
}

void helper_log_str(target_ulong ptr)
{
    qemu_log("%s\n", (char *)ptr);
}

bool is_cache_aligned(CPULINXState *env, target_ulong addr, int size)
{
    if (addr >= env->store_addr &&
        addr + size <= env->store_addr + LINX_CACHE_LINE_SIZE)
        return true;
    else
        return false;
}

void load_store_prepare(CPULINXState *env, target_ulong addr, int size)
{
    int i;

    if (env->store_addr_valid == 0) {
        env->store_addr_valid = 1;
        env->store_addr = addr & MAKE_64BIT_MASK(LINX_CACHE_LINE_SHIFT,
                                                 64 - LINX_CACHE_LINE_SHIFT);
        for (i = 0; i < LINX_CACHE_LINE_SIZE / 8; i++) {
            env->store_buf[i] = cpu_ldq_data(env, env->store_addr + 8 * i);
        }
    } else {
        if (!is_cache_aligned(env, addr, size)) {
            linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
        }
    }
}

void helper_store_data(CPULINXState *env, target_ulong addr,
                       target_ulong data, int size)
{
    load_store_prepare(env, addr, size);

    /* Note: must be little end here */
    memcpy((uint8_t *)env->store_buf +
           extract64(addr, 0, LINX_CACHE_LINE_SHIFT), &data, size);
}

target_ulong helper_load_data(CPULINXState *env, target_ulong addr, int memop)
{
    int is_sign, size;
    target_ulong tmp = 0;

    size = memop_size(memop);
    is_sign = (memop & MO_SIGN) ? 1 : 0;

    load_store_prepare(env, addr, size);

    /* Note: must be little end here */
    memcpy(&tmp, (uint8_t *)env->store_buf +
           extract64(addr, 0, LINX_CACHE_LINE_SHIFT), size);
    if (is_sign)
        return sextract64(tmp, 0, size * 8);
    else
        return extract64(tmp, 0, size * 8);
}

void helper_write_store_buf_to_mem(CPULINXState *env)
{
    uint64_t addr = env->store_addr;
    uint64_t i;

    if (env->store_addr_valid) {
        for (i = 0; i < LINX_CACHE_LINE_SIZE / 8; i++) {
            cpu_stq_data_ra(env, addr + 8 * i, env->store_buf[i], GETPC());
        }
    }
}

target_ulong helper_ssrswap(CPULINXState *env, target_ulong src, int csr)
{

#ifndef CONFIG_USER_ONLY
    if (extract32(csr, 8, 4) == 0xf) {
        csr = (env->priv << 12) | csr;
    }
#endif

    return helper_csrrw(env, csr, src, -1);
}

target_ulong helper_csrr(CPULINXState *env, int csr)
{
    target_ulong val = 0;
    LINXException ret = linx_csrrw(env, csr, &val, 0, 0);

    if (ret != LINX_EXCP_NONE) {
        linx_raise_exception(env, ret, GETPC());
    }
    return val;
}

void helper_csrw(CPULINXState *env, int csr, target_ulong src)
{
    LINXException ret = linx_csrrw(env, csr, NULL, src, -1);

    if (ret != LINX_EXCP_NONE) {
        linx_raise_exception(env, ret, GETPC());
    }
}

target_ulong helper_csrrw(CPULINXState *env, int csr,
                          target_ulong src, target_ulong write_mask)
{
    target_ulong val = 0;
    LINXException ret = linx_csrrw(env, csr, &val, src, write_mask);

    if (ret != LINX_EXCP_NONE) {
        linx_raise_exception(env, ret, GETPC());
    }
    return val;
}

#ifndef CONFIG_USER_ONLY

void linx_recovery_bstate_by_ebstate(CPULINXState *env, target_ulong priv)
{
    LinxSYSReg *sysreg = &env->sysreg[priv];
    uint64_t hi = env->header_info;
    uint64_t ebarg = env->sysreg[priv].ebarg;
    uint32_t br_typ = get_field(ebarg, EBARG_TYPE);
    uint32_t blk_typ = get_field(ebarg, EBARG_BLOCKTYPE);

    env->bpc = sysreg->ebpc;
    env->tpc = sysreg->etpc;
    env->carg_flag = get_field(ebarg, EBARG_TAKEN);

    hi = set_field(hi, HEADER_INFO_AQ_MASK, get_field(ebarg, EBARG_AQ));
    hi = set_field(hi, HEADER_INFO_RL_MASK, get_field(ebarg, EBARG_RL));
    if (blk_typ == HEAD_TYPE_STD || blk_typ == HEAD_TYPE_FP ||
        blk_typ == HEAD_TYPE_FRET_RA || blk_typ == HEAD_TYPE_FRET_STK) {
        switch (br_typ) {
        case CBRANCH_FALL: {
            br_typ = BRANCH_FALL;
            env->next_bpc = sysreg->ebpcn;
        } break;
        case CBRANCH_DIRECT: {
            br_typ = BRANCH_DIRECT_LINK;
            env->carg_tgt = sysreg->ebpcn;
        } break;
        case CBRANCH_COND: {
            br_typ = BRANCH_CONDITIONAL;
            env->carg_tgt = sysreg->ebpcn;
        } break;
        case CBRANCH_IND: {
            br_typ = BRANCH_IND;
            env->carg_tgt = sysreg->ebpcn;
        } break;
        }
    } else {
        br_typ = BRANCH_FALL;
        env->next_bpc = sysreg->ebpcn;
    }
    env->tpc1 = env->tpc_s;
    env->tileop_info = env->tileop_s;
    hi = set_field(hi, HEADER_INFO_BRHTYPE_MASK, br_typ);

    if (blk_typ == HEAD_TYPE_STD || blk_typ == HEAD_TYPE_SYS ||
        blk_typ == HEAD_TYPE_FP) {
        hi = deposit64(hi, HEADER_INFO_DECOUPLE_START,
                        HEADER_INFO_DECOUPLE_LEN, 0);
    } else {
        hi = deposit64(hi, HEADER_INFO_DECOUPLE_START,
                        HEADER_INFO_DECOUPLE_LEN, 1);
    }

    hi = set_field(hi, HEADER_INFO_BLKTYPE_MASK, blk_typ);
    env->header_info = hi;
    env->dsttile[0] = get_field(ebarg, EBARG_REGDST0);
    env->dsttile[1] = get_field(ebarg, EBARG_REGDST1);
    env->dsttile[2] = get_field(ebarg, EBARG_REGDST2);
    env->dsttile[3] = get_field(ebarg, EBARG_REGDST3);
    return;
}

void linx_save_bstate(CPULINXState *env, target_ulong acr)
{
    LinxSYSReg *sysreg = &env->sysreg[acr];
    uint64_t ebarg = sysreg->ebarg;
    uint32_t br_typ = get_brhtype(env->header_info);
    sysreg->etpc = env->pc;
    sysreg->ebpcn = env->carg_tgt;
    sysreg->ebpc = env->bpc;

    ebarg = set_field(ebarg, EBARG_GROUPID, 1);
    ebarg = set_field(ebarg, EBARG_RL, get_rl(env->header_info));
    ebarg = set_field(ebarg, EBARG_AQ, get_aq(env->header_info));
    ebarg = set_field(ebarg, EBARG_TAKEN, env->carg_flag);
    env->tpc_s = env->tpc1;
    env->tileop_s = env->tileop_info;
    switch (br_typ) {
    case BRANCH_FALL: {
        br_typ = CBRANCH_FALL;
        sysreg->ebpcn = env->next_bpc;
    } break;
    case BRANCH_DIRECT_LINK:
    case BRANCH_CALL: {
        br_typ = CBRANCH_DIRECT;
    } break;
    case BRANCH_CONDITIONAL: {
        br_typ = CBRANCH_COND;
    } break;
    case BRANCH_IND:
    case BRANCH_INDCALL:
    case BRANCH_RET: {
        br_typ = CBRANCH_IND;
    } break;
    default:
        assert(0);
    }

    ebarg = set_field(ebarg, EBARG_TYPE, br_typ);
    ebarg = set_field(ebarg, EBARG_BLOCKTYPE, get_blktype(env->header_info));
    ebarg = set_field(ebarg, EBARG_REGDST0, env->dsttile[0]);
    ebarg = set_field(ebarg, EBARG_REGDST1, env->dsttile[1]);
    ebarg = set_field(ebarg, EBARG_REGDST2, env->dsttile[2]);
    ebarg = set_field(ebarg, EBARG_REGDST3, env->dsttile[3]);
    sysreg->ebarg = ebarg;
    return;
}

void linx_save_bstate_layer1(CPULINXState *env, target_ulong acr)
{
    LinxSYSReg *sysreg = &env->sysreg[acr];
    uint64_t ebarg = sysreg->ebarg;
    uint32_t br_typ = get_brhtype(env->header_info);

    sysreg->etpc = env->pc;
    env->cstate = set_field(env->cstate, CSTATE_IE, 0);

    /* save the tpc as the bpc so that the acre can still return to the bpc. */
    if (!is_valid_linx_addr(env->bpc)) {
        assert(0);
    }

    sysreg->ebpc = env->bpc;
    sysreg->ebpcn = env->carg_tgt;
    if (br_typ == BRANCH_FALL) {
        sysreg->ebpcn = env->next_bpc;
    }

    ebarg = set_field(ebarg, EBARG_TAKEN, env->carg_flag);
    sysreg->ebarg = ebarg;
    sysreg->ecstate = set_field(sysreg->ecstate, ECSTATE_BI, 0);
}

#endif

void linx_reset_bstate(CPULINXState *env)
{
    memset(&env->tileop_info, 0, sizeof(struct TILEOPInfo));
    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->bpc = LINX_ILLEGAL_INSTR_ADDR;
    env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
    env->tpc1 = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
}

void linx_reset_bstate_short(CPULINXState *env)
{
    env->carg_tgt = LINX_ILLEGAL_INSTR_ADDR;
    env->tpc = LINX_ILLEGAL_INSTR_ADDR;
    env->tpc1 = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = false;
}

extern uint64_t cs_call_skipspace_cur;
extern uint64_t cs_call_skipspace;
extern bool cs_call_cpu;
extern bool cs_call_pc;
extern bool cs_call_func;
extern bool cs_call_brhtype;
extern bool cs_call_onlycall;
void helper_blk_cpu_state_dump(CPULINXState *env,
                               uint64_t src_pc, uint64_t dst_pc)
{
#ifdef CONFIG_USER_ONLY
    CPUState *cs = env_cpu(env);
    cs_call_skipspace_cur = (cs_call_skipspace_cur + 1) % cs_call_skipspace;
    if (cs_call_skipspace_cur == 0) {
        /*
         * call, direct, and indirect call indicate function call,
         * ret indicates that the function is returned after function call
         */
        if (cs_call_onlycall &&
            (get_brhtype(env->header_info) != BRANCH_DIRECT_LINK) &&
            (get_brhtype(env->header_info) != BRANCH_CALL) &&
            (get_brhtype(env->header_info) != BRANCH_INDCALL)) {
            return;
        }
        if (cs_call_pc) {
            qemu_log("cur:%lx, dst:%lx ", src_pc, dst_pc);
        }
        if (cs_call_func) {
            qemu_log("%s -> %s", lookup_symbol(src_pc), lookup_symbol(dst_pc));
        }
        if (cs_call_brhtype) {
            qemu_log(" %s", linx_brhtype_names[get_brhtype(env->header_info)]);
        }
        qemu_log("\n");
        if (cs_call_cpu) {
            log_cpu_state(cs, CPU_DUMP_CODE);
        }
    }
#endif
}

void helper_blk_do_recovery(CPULINXState *env)
{
    uint64_t newpc;
    CPUState *cs = env_cpu(env);

    if (env->tpc != LINX_ILLEGAL_INSTR_ADDR) {
        /* bstate is valid, so this is a recovery */
        newpc = env->tpc;
        qemu_log_mask(CPU_LOG_CS,
                      "  block recoverred with header %lx(%lx), TPC=%lx\n",
                      env->bpc, env->tpc1, newpc);

        if (env->tpc == env->bpc) {
            qemu_log_mask(CPU_LOG_CS, "  redo commit, blktype=%lu\n",
                          get_blktype(env->header_info));
            /* redo commit (but we assume sgprs will alwayse commit with the
             * last mini insn without break. Here we just redo the remain
             * activities
             */
            if (get_blktype(env->header_info) != HEAD_TYPE_SYS) {
                linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
            }
            helper_handle_exec_and_branch(env);
            cpu_loop_exit(cs);
        }

        /* the bstate is not this block */
        if (env->tpc < env->tpc1) {
            qemu_log("the bstate is not this block, tpc: %lx, tpc1: %lx\n",
                     env->tpc, env->tpc1);
            g_assert_not_reached();
            linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
        }
    } else {
        newpc = env->tpc1;
    }

    // no irq should be do here to avoid masking the exception
    if (enable_delay_block_intr)
        helper_set_next_flags(env, CF_NOIRQ);

    env->pc = newpc;
}


#ifndef CONFIG_USER_ONLY
void linx_cs_log(CPULINXState *env)
{
    int i;

    if (likely(!qemu_loglevel_mask(CPU_LOG_CS)))
        return;

    qemu_log("  GPRS:          \n");
    for (i = 0; i < sizeof(env->gpr)/sizeof(env->gpr[0]); i++) {
        if ((i+1) % 4 != 0) {
            qemu_log("  " TARGET_FMT_lx " ", env->gpr[i]);
        } else {
            qemu_log("  " TARGET_FMT_lx " \n", env->gpr[i]);
        }
    }
    qemu_log("\n");

    qemu_log("  CARG_FLAG:          " TARGET_FMT_lx "  ", env->carg_flag);
    qemu_log("  CARG_TGT:          " TARGET_FMT_lx "\n", env->carg_tgt);
    qemu_log("  NEXT_BPC:          " TARGET_FMT_lx "  ", env->next_bpc);
    qemu_log("  BRANCH_TYPE:       " TARGET_FMT_lx "\n",
                 get_brhtype(env->header_info));
    qemu_log("  BLOCK_TYPE:        " TARGET_FMT_lx "\n",
                get_blktype(env->header_info));
    qemu_log("lb0:0x%lx, lb1:0x%lx, lb2:0x%lx\n",
                env->csr_lb[0], env->csr_lb[1], env->csr_lb[2]);
    qemu_log("lc0:0x%lx, lc1:0x%lx, lc2:0x%lx\n",
                env->csr_lc[0], env->csr_lc[1], env->csr_lc[2]);
    qemu_log("  predm:0x%lx\n", env->predm);

    qemu_log("  TREGS:         ");
    for (i = 0; i < sizeof(env->blk_t)/sizeof(env->blk_t[0]); i++)
        qemu_log(TARGET_FMT_lx " ", env->blk_t[(env->t_idx+i)%8]);
    qemu_log("\n");
}

static void check_acrc_arg(CPULINXState *env, uint32_t arg)
{
    if (env->priv == ACR1) {
        if (arg != SCT_MAC && arg != SCT_SEC) {
            linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
        }
    } else if (env->priv == ACR2) {
        if (arg >= SCT_MAX_NUMBER) {
            linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
        }
    }
}

void helper_acrc(CPULINXState *env, uint32_t arg)
{
    check_acrc_arg(env, arg);

    env->scall_arg = arg;
    env->tpc = env->pc;
    env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
    env->tpc1 = LINX_ILLEGAL_INSTR_ADDR;
    env->in_body = true;
    linx_raise_exception(env, LINX_EXCP_SCALL, GETPC());
}

target_ulong helper_acre(CPULINXState *env, uint32_t rra)
{
#define ACRE_RRAT_DEFAULT 0
#define ACRE_RRAT_RESTORE 1
    target_ulong prev_acr;
    CPULINXState old_env;
    uint64_t ecstate;
    bool eb_recover = false;
    target_ulong retpc;
    old_env = *env;
    prev_acr = get_field(env->sysreg[env->priv].ecstate, ECSTATE_ACR);

    if (env->priv > ACR1) {
         linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
    }

    ecstate = env->sysreg[env->priv].ecstate;
    env->cstate = ecstate;
    if (env->cstate & ECSTATE_BI) {
        retpc = env->sysreg[env->priv].etpc;
    } else {
        retpc = sextract64(env->sysreg[env->priv].ebpc, 0, 48);
    }
    /* is this a sret in block commit stage? */

    if (rra != ACRE_RRAT_DEFAULT && rra != ACRE_RRAT_RESTORE) {
            qemu_log("error rra for acrc, tpc: %lx, tpc1: %lx\n",
                        env->tpc, env->tpc1);
            linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
    }

    linx_reset_bstate(env); /* reset bstate to default to whatever case */

    if (rra == ACRE_RRAT_RESTORE) {
        linx_recovery_bstate_by_ebstate(env, ACR1);
        retpc = env->tpc;
        eb_recover = true;
        env->tpc = LINX_ILLEGAL_INSTR_ADDR;
        if (env->bpc != retpc) {
            env->in_body = true;
        }
    }

    linx_cpu_set_mode(env, prev_acr);

    if (!(qemu_loglevel_mask(CPU_LOG_CS_NO_M) && (old_env.priv==ACR0 || env->priv==ACR0))) {
        qemu_log_mask(CPU_LOG_CS, "------------- CS_IN%s(" TARGET_FMT_ld ") Addr (%lx=>%lx) Priv(%ld=>%ld)\n",
                      eb_recover ? "_RECOVER" : "",
                      env->lxlcid, old_env.pc, retpc,
                      old_env.priv, env->priv);
        linx_cs_log(env);
    }

    return retpc;
}

void helper_wfi(CPULINXState *env)
{
    CPUState *cs = env_cpu(env);
    if (env->priv > ACR1) {
        linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
    } else {
        cs->halted = 1;
        cs->exception_index = EXCP_HLT;
        cpu_loop_exit(cs);
    }

}

void helper_tlb_flush(CPULINXState *env)
{
    CPUState *cs = env_cpu(env);
    if (env->priv > ACR1) {
        linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
    } else {
        tlb_flush(cs);
    }
}

#endif /* !CONFIG_USER_ONLY */

static void helper_set_dst_regx(CPULINXState *env, int dst_op, target_long val)
{
    if (dst_op > 0 && dst_op < 24) {
        env->gpr[dst_op] = val;
        return;
    }

    if (dst_op == TARGET_REG_T) {
        /* push to T register */
        env->blk_t[env->t_idx % T_REG_SIZE] = val;
        env->t_idx++;
        return;
    } else if (dst_op == TARGET_REG_U) {
        /* push to U register */
        env->blk_u[env->u_idx % U_REG_SIZE] = val;
        env->u_idx++;
        return;
    }

    linx_debug_not_reached();

    return;
}

#define gqm_move_ptr(type, name)                                              \
static void gqm_move_##type(uint64_t *meta_data, uint16_t pos, uint16_t len)  \
{                                                                             \
    uint64_t _meta_data = *meta_data;                                         \
    *meta_data = FIELD_DP64(_meta_data, GQM_META_DATA, name, (pos + 1) % len);\
}
gqm_move_ptr(tail, TAIL)
gqm_move_ptr(head, HEAD)

static void gqm_move_head_ahead(uint64_t *meta_data, uint16_t pos, uint16_t len)
{
    *meta_data = FIELD_DP64(*meta_data, GQM_META_DATA, HEAD,
                            pos == 0 ? (len - 1) : (pos - 1));
}

static int gqm_page_check(uint64_t base, uint64_t data_addr)
{
    if ((base & TARGET_PAGE_MASK) != (data_addr & TARGET_PAGE_MASK)) {
        qemu_log("gqm: fail which is crossed page!\n");
        return -1;
    }

    return 0;
}

static int gqm_meta_data_check(target_ulong base, uint64_t meta_data)
{
    target_ulong gqm_end;
    int space_status, queue_status, head, tail, len;

    len = FIELD_EX64(meta_data, GQM_META_DATA, LEN);
    head = FIELD_EX64(meta_data, GQM_META_DATA, HEAD);
    tail = FIELD_EX64(meta_data, GQM_META_DATA, TAIL);
    space_status = FIELD_EX64(meta_data, GQM_META_DATA, SPACE_STATUS);
    queue_status = FIELD_EX64(meta_data, GQM_META_DATA, QUEUE_STATUS);

    gqm_end = base + len * GQM_ENTRY_BYTE + GQM_META_DATA_BYTE - 1;

    if (head == tail) {
        if (space_status != GQM_SPACE_STATUS_EMPTY ||
            space_status != GQM_SPACE_STATUS_FULL) {
            return -1;
        }
    } else {
        if (space_status != GQM_SPACE_STATUS_NORMAL) {
            return -1;
        }
    }

    if (head > len || tail > len) {
        return -1;
    }

    if (len == 1 && (space_status == GQM_SPACE_STATUS_NORMAL)) {
        return -1;
    }

    if (queue_status >= GQM_QUEUE_STATUS_MAX ||
        space_status >= GQM_SPACE_STATUS_MAX) {
        return -1;
    }

    return gqm_page_check(base, gqm_end);
}

static int gqm_get_remain_element(uint64_t meta_data)
{
    int space_status, head, tail, len;

    head = FIELD_EX64(meta_data, GQM_META_DATA, HEAD);
    tail = FIELD_EX64(meta_data, GQM_META_DATA, TAIL);
    len = FIELD_EX64(meta_data, GQM_META_DATA, LEN);
    space_status = FIELD_EX64(meta_data, GQM_META_DATA, SPACE_STATUS);

    /* Using this function should check meta_data if valid */
    if (space_status == GQM_SPACE_STATUS_EMPTY) {
        return 0;
    } else if (space_status == GQM_SPACE_STATUS_FULL) {
        return len;
    } else {
        return (tail - head + len) % len;
    }
}

static target_ulong gqm_init(CPULINXState *env, target_ulong base,
                             target_ulong len)
{
    int err = 0;
    target_ulong result = 0;
    uint64_t gqm_end, gqm_size, init_meta_data = 0;

    gqm_size = len * GQM_ENTRY_BYTE + GQM_META_DATA_BYTE;
    gqm_end = base + gqm_size - 1;

    err = gqm_page_check(base, gqm_end);
    if (err) {
        result = FIELD_DP64(result, GQM_QMT_RESULT, ERRNO,
                            GQM_QMT_RET_ERRNO_INCORRECT_PARA);
        return result;
    }

    if (len == 0) {
        qemu_log("gqm: length should not be 0!\n");
        result = FIELD_DP64(result, GQM_QMT_RESULT, ERRNO,
                            GQM_QMT_RET_ERRNO_INCORRECT_PARA);
        return result;
    }

    init_meta_data = FIELD_DP64(init_meta_data, GQM_META_DATA, LEN, len);
    init_meta_data = FIELD_DP64(init_meta_data, GQM_META_DATA, SPACE_STATUS,
                                GQM_SPACE_STATUS_EMPTY);
    init_meta_data = FIELD_DP64(init_meta_data, GQM_META_DATA, QUEUE_STATUS,
                                GQM_QUEUE_STATUS_OPEN);
    init_meta_data = FIELD_DP64(init_meta_data, GQM_META_DATA, HEAD, 0);
    init_meta_data = FIELD_DP64(init_meta_data, GQM_META_DATA, TAIL, 0);

    /* gqm meta data init */
    cpu_stq_data_ra(env, base, init_meta_data, GETPC());

    return gqm_size;
}

target_ulong helper_gqm_qmt(CPULINXState *env, target_ulong cmd,
                            target_ulong base, target_ulong src_r)
{
    int len;
    uint64_t meta_data = 0;
    target_ulong result = 0;
    bool init = (cmd >> R_GQM_CMD_I_SHIFT) & 1;
    bool suspend = (cmd >> R_GQM_CMD_S_SHIFT) & 1;
    bool release = (cmd >> R_GQM_CMD_R_SHIFT) & 1;

    if (suspend && release) {
        result = FIELD_DP64(result, GQM_QMT_RESULT, ERRNO,
                            GQM_QMT_RET_ERRNO_INCORRECT_PARA);
        return result;
    }

    if (init) {
        len = src_r & R_GQM_META_DATA_LEN_MASK;
        result =  gqm_init(env, base, len);
    } else {
        meta_data = cpu_ldq_data(env, base);
        result = gqm_get_remain_element(meta_data);
        if (FIELD_EX64(meta_data, GQM_META_DATA, QUEUE_STATUS) ==
            GQM_QUEUE_STATUS_CLOSE) {
            result = FIELD_DP64(result, GQM_QMT_RESULT, STATUS,
                                GQM_QUEUE_STATUS_CLOSE);
        }
    }

    if (meta_data != 0) {
        meta_data = cpu_ldq_data(env, base);
    }

    if (gqm_meta_data_check(base, meta_data))
        return FIELD_DP64(result, GQM_QMT_RESULT, ERRNO,
                          GQM_QMT_RET_ERRNO_DATA_CORRUPTION);

    if (suspend) {
        meta_data = FIELD_DP64(meta_data, GQM_META_DATA, QUEUE_STATUS,
                               GQM_QUEUE_STATUS_CLOSE);
        result = FIELD_DP64(result, GQM_QMT_RESULT, STATUS,
                            GQM_QUEUE_STATUS_CLOSE);
    }

    if (release) {
        meta_data = FIELD_DP64(meta_data, GQM_META_DATA, QUEUE_STATUS,
                               GQM_QUEUE_STATUS_OPEN);
        result = FIELD_DP64(result, GQM_QMT_RESULT, STATUS,
                            GQM_QUEUE_STATUS_OPEN);
    }

    cpu_stq_data_ra(env, base, meta_data, GETPC());

    return result;
}

target_ulong helper_gqm_qpush(CPULINXState *env, target_ulong cmd,
                              target_ulong base, target_ulong data)
{
    target_ulong data_addr = 0, result = 0, remain_ele = 0;
    uint64_t head, tail, length, queue_status, space_status;
    uint64_t meta_data = cpu_ldq_data(env, base);

    length = FIELD_EX64(meta_data, GQM_META_DATA, LEN);
    queue_status = FIELD_EX64(meta_data, GQM_META_DATA, QUEUE_STATUS);
    space_status = FIELD_EX64(meta_data, GQM_META_DATA, SPACE_STATUS);

    if (gqm_meta_data_check(base, meta_data))
        return FIELD_DP64(result, GQM_QPUSH_RESULT, ERRNO,
                          GQM_QPUSH_RET_ERRNO_DATA_CORRUPTION);

    remain_ele = gqm_get_remain_element(meta_data);
    if (queue_status == GQM_QUEUE_STATUS_CLOSE ||
        space_status == GQM_SPACE_STATUS_FULL) {
        result = FIELD_DP64(result, GQM_QPUSH_RESULT, REMAIN, remain_ele);
        result = FIELD_DP64(result, GQM_QPUSH_RESULT, ERRNO,
                            GQM_QPUSH_RET_ERRNO_QUEUE_FULL);
        return result;
    }

    if (cmd & R_GQM_CMD_H_MASK) {
        /* push head */
        head = FIELD_EX64(meta_data, GQM_META_DATA, HEAD);
        gqm_move_head_ahead(&meta_data, head, length);
        data_addr = base + GQM_META_DATA_BYTE +
                    (head == 0 ? (length - 1) : (head - 1)) * GQM_ENTRY_BYTE;
    } else {
        /* push tail */
        tail = FIELD_EX64(meta_data, GQM_META_DATA, TAIL);
        gqm_move_tail(&meta_data, tail, length);
        data_addr = base + GQM_META_DATA_BYTE + tail * GQM_ENTRY_BYTE;
    }

    if (gqm_page_check(base, data_addr))
        return FIELD_DP64(0, GQM_QPUSH_RESULT, ERRNO,
                          GQM_QPUSH_RET_ERRNO_DATA_CORRUPTION);

    remain_ele++;
    result = FIELD_DP64(result, GQM_QPUSH_RESULT, REMAIN, remain_ele);
    if (remain_ele == length)
        /* Queue full */
        meta_data = FIELD_DP64(meta_data, GQM_META_DATA, SPACE_STATUS,
                               GQM_SPACE_STATUS_FULL);
    else
        meta_data = FIELD_DP64(meta_data, GQM_META_DATA, SPACE_STATUS,
                               GQM_SPACE_STATUS_NORMAL);

    cpu_stq_data_ra(env, base, meta_data, GETPC());
    cpu_stq_data_ra(env, data_addr, data, GETPC());

    return result;
}

void helper_gqm_qpop(CPULINXState *env, target_ulong cmd, target_ulong base,
                     target_ulong reg_type)
{
    target_ulong data = 0, data_addr, result = 0;
    uint64_t meta_data, length, head, remain_ele;

    meta_data = cpu_ldq_data(env, base);
    head = FIELD_EX64(meta_data, GQM_META_DATA, HEAD);
    length = FIELD_EX64(meta_data, GQM_META_DATA, LEN);

    if (gqm_meta_data_check(base, meta_data)) {
        helper_set_dst_regx(env, reg_type, data);
        helper_set_dst_regx(env, reg_type, FIELD_DP64(0, GQM_QPOP_RESULT, ERRNO,
                            GQM_QPOP_RET_ERRNO_DATA_CORRUPTION));
    }

    remain_ele = gqm_get_remain_element(meta_data);
    data_addr = base + GQM_META_DATA_BYTE + head * GQM_ENTRY_BYTE;

    if (gqm_page_check(base, data_addr)) {
        helper_set_dst_regx(env, reg_type, data);
        helper_set_dst_regx(env, reg_type, FIELD_DP64(0, GQM_QPOP_RESULT, ERRNO,
            GQM_QPOP_RET_ERRNO_DATA_CORRUPTION));
        return;
    }

    if (reg_type == TARGET_REG_TX2) {
        reg_type = TARGET_REG_T;
    } else if (reg_type == TARGET_REG_UX2) {
        reg_type = TARGET_REG_U;
    } else {
        linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
    }

    if (remain_ele == 0) {
        helper_set_dst_regx(env, reg_type, data);
        helper_set_dst_regx(env, reg_type, FIELD_DP64(0, GQM_QPOP_RESULT, ERRNO,
            GQM_QPOP_RET_ERRNO_QUEUE_EMPTY));
    }

    gqm_move_head(&meta_data, head, length);
    data = cpu_ldq_data(env, data_addr);
    remain_ele--;

    meta_data = FIELD_DP64(meta_data, GQM_META_DATA, LEN, remain_ele);
    if (remain_ele == 0)
        meta_data = FIELD_DP64(meta_data, GQM_META_DATA, SPACE_STATUS,
                               GQM_SPACE_STATUS_EMPTY);
    else
        meta_data = FIELD_DP64(meta_data, GQM_META_DATA, SPACE_STATUS,
                               GQM_SPACE_STATUS_NORMAL);
    cpu_stq_data_ra(env, base, meta_data, GETPC());

    result = FIELD_DP64(0, GQM_QPOP_RESULT, REMAIN, remain_ele);
    helper_set_dst_regx(env, reg_type, data);
    helper_set_dst_regx(env, reg_type, result);
}

#define MATRIX_ADD(E_TYPE, SRC0, SRC1, DST)  \
do {                        \
    for (uint32_t i = 0; i < m; ++i) {      \
        for (uint32_t j = 0; j < n; ++j) {  \
            ((E_TYPE *)DST)[i * n + j] = ((E_TYPE *)SRC0)[i * n + j] + \
                                         ((E_TYPE *)SRC1)[i * n + j];  \
        }   \
    }       \
} while (0)

#define MATRIX_FADD(E_TYPE, SRC0, SRC1, DST, WIDTH)  \
do {                        \
    for (uint32_t i = 0; i < m; ++i) {      \
        for (uint32_t j = 0; j < n; ++j) {  \
            uint64_t rs1 = ((E_TYPE *)SRC0)[i * n + j];     \
            uint64_t rs2 = ((E_TYPE *)SRC1)[i * n + j];     \
            ((E_TYPE *)DST)[i * n + j] =                    \
                        (E_TYPE)helper_fadd_##WIDTH(env, rs1, rs2);   \
        }   \
    }       \
} while (0)

static void matrix_add(CPULINXState *env, uint64_t src0, uint64_t src1, uint64_t dst,
                       uint32_t m, uint32_t n, SrcType typ)
{
    switch (typ) {
    case FP64: {
        MATRIX_FADD(uint64_t, src0, src1, dst, d);
    } break;
    case FP32: {
        MATRIX_FADD(uint32_t, src0, src1, dst, s);
    } break;
    case INT64: {
        MATRIX_ADD(int64_t, src0, src1, dst);
    } break;
    case INT32: {
        MATRIX_ADD(int32_t, src0, src1, dst);
    } break;
    case INT16: {
        MATRIX_ADD(int16_t, src0, src1, dst);
    } break;
    case INT8: {
        MATRIX_ADD(int8_t, src0, src1, dst);
    } break;
    case UINT64: {
        MATRIX_ADD(uint64_t, src0, src1, dst);
    } break;
    case UINT32: {
        MATRIX_ADD(uint32_t, src0, src1, dst);
    } break;
    case UINT16: {
        MATRIX_ADD(uint16_t, src0, src1, dst);
    } break;
    case UINT8: {
        MATRIX_ADD(uint8_t, src0, src1, dst);
    } break;
    case E5M2:
    case E4M3: {
        assert(0);
    } break;
    case FP16: {
        MATRIX_FADD(uint16_t, src0, src1, dst, h);
    } break;
    case E8M0:
    case BF16:
    default:
        assert(0);
    }
    return;
}

#define MATRIX_MUL(E_TYPE, TMP_RES)                         \
do {                                                        \
    for (uint32_t t = 0; t < n; ++t) {                      \
        for (uint32_t i = 0; i < m; ++i) {                  \
            TMP_RES = 0;                                    \
            for (uint32_t j = 0; j < k;  ++j) {             \
                TMP_RES += ((E_TYPE *)src0)[i * k + j] *    \
                           ((E_TYPE *)src1)[j * n + t];     \
            }                                               \
            ((E_TYPE *)dst)[i * n + t] = TMP_RES;           \
        }                                                   \
    }                                                       \
} while (0)

#define MATRIX_FMUL(E_TYPE, WIDTH)  \
do {                                                            \
    for (uint32_t t = 0; t < n; ++t) {                          \
        for (uint32_t i = 0; i < m; ++i) {                      \
            uint64_t rs1 = ((E_TYPE *)src0)[i * k + 0];         \
            uint64_t rs2 = ((E_TYPE *)src1)[0 * n + t];         \
            uint64_t sum = helper_fmul_##WIDTH(env, rs1, rs2);  \
            for (uint32_t j = 1; j < k;  ++j) {                 \
                rs1 = ((E_TYPE *)src0)[i * k + j];              \
                rs2 = ((E_TYPE *)src1)[j * n + t];              \
                sum = helper_fadd_##WIDTH(env, sum,             \
                helper_fmul_##WIDTH(env, rs1, rs2));            \
            }                                                   \
            ((E_TYPE *)dst)[i * n + t] = sum;                   \
        }                                                       \
    }                                                           \
} while (0)

static void matrix_mul(CPULINXState *env, uint64_t src0, uint64_t src1, uint64_t dst,
                       uint32_t m, uint32_t n, uint32_t k, SrcType typ)
{
    uint64_t uint_tmp = 0;
    int64_t   int_tmp = 0;
    switch (typ) {
    case FP64: {
        MATRIX_FMUL(uint64_t, d);
    } break;
    case FP32: {
        MATRIX_FMUL(uint32_t, s);
    } break;
    case INT64: {
        MATRIX_MUL(int64_t, int_tmp);
    } break;
    case INT32: {
        MATRIX_MUL(int32_t, int_tmp);
    } break;
    case INT16: {
        MATRIX_MUL(int16_t, int_tmp);
    } break;
    case INT8: {
        MATRIX_MUL(int8_t, int_tmp);
    } break;
    case UINT64: {
        MATRIX_MUL(uint64_t, uint_tmp);
    } break;
    case UINT32: {
        MATRIX_MUL(uint32_t, uint_tmp);
    } break;
    case UINT16: {
        MATRIX_MUL(uint16_t, uint_tmp);
    } break;
    case UINT8: {
        MATRIX_MUL(uint8_t, uint_tmp);
    } break;
    case E5M2:
    case E4M3: {
        assert(0);
    } break;
    case FP16: {
        MATRIX_FMUL(uint16_t, h);
    } break;
    case E8M0:
    case BF16:
    default:
        assert(0);
    }
    return;
}

/* 模板宏：定义类型特化版本 */
#define DEFINE_MATRIX_TRANSFORM_WITH_PADDING(T, SFX) \
static void row_major_to_Nz_##SFX(const T * input, T * output,    \
                                  int m, int n, int t, int u, uint64_t pad)   \
{ \
    int brs = (m + t - 1) / t, bcs = (n + u - 1) / u, idx = 0;  \
    for (int bc = 0; bc < bcs; ++bc) \
        for (int br = 0; br < brs; ++br) \
            for (int i = 0; i < t; ++i) \
                for (int j = 0; j < u; ++j) { \
                    int r = br * t + i, c = bc * u + j; \
                    output[idx++] = (r < m && c < n) ?  \
                                    input[r * n + c] : (T)pad; \
                } \
} \
static void row_major_to_Zn_##SFX(const T *input, T *output,    \
                                  int m, int n, int t, int u, uint64_t pad) \
{ \
    int brs = (m + t - 1) / t, bcs = (n + u - 1) / u, idx = 0; \
    for (int br = 0; br < brs; ++br) \
        for (int bc = 0; bc < bcs; ++bc) \
            for (int j = 0; j < u; ++j) \
                for (int i = 0; i < t; ++i) { \
                    int r = br * t + i, c = bc * u + j; \
                    output[idx++] = (r < m && c < n) ?  \
                                    input[r * n + c] : (T)pad; \
                } \
} \
static void Nz_to_row_major_##SFX(const T *input, T *output,  \
                                  int m, int n, int t, int u, uint64_t pad) \
{ \
    int brs = (m + t - 1) / t, bcs = (n + u - 1) / u, idx = 0;  \
    for (int bc = 0; bc < bcs; ++bc) \
        for (int br = 0; br < brs; ++br) \
            for (int i = 0; i < t; ++i) \
                for (int j = 0; j < u; ++j) { \
                    int r = br * t + i, c = bc * u + j; \
                    output[r * bcs * u + c] =   \
                        (r < m && c < n) ? input[idx] : (T)pad; \
                    ++idx; \
                } \
} \
static void Zn_to_row_major_##SFX(const T *input, T *output,    \
                                  int m, int n, int t, int u, uint64_t pad) \
{ \
    int brs = (m + t - 1) / t, bcs = (n + u - 1) / u, idx = 0; \
    for (int br = 0; br < brs; ++br) \
        for (int bc = 0; bc < bcs; ++bc) \
            for (int j = 0; j < u; ++j) \
                for (int i = 0; i < t; ++i) { \
                    int r = br * t + i, c = bc * u + j; \
                    output[r * bcs * u + c] = \
                        (r < m && c < n) ? input[idx] : (T)pad; \
                    ++idx; \
                } \
} \
static void Zz_to_row_major_##SFX(const T *input, T *output,    \
                                  int m, int n, int t, int u, uint64_t pad) \
{ \
    int brs = (m + t - 1) / t, bcs = (n + u - 1) / u, idx = 0; \
    for (int br = 0; br < brs; ++br) \
        for (int bc = 0; bc < bcs; ++bc) \
            for (int i = 0; i < t; ++i)  \
                for (int j = 0; j < u; ++j) { \
                    int r = br * t + i, c = bc * u + j; \
                    output[r * bcs * u + c] = \
                        (r < m && c < n) ? input[idx] : (T)pad; \
                    ++idx; \
                } \
} \
static void Nn_to_row_major_##SFX(const T *input, T *output,  \
                                  int m, int n, int t, int u, uint64_t pad) \
{ \
    int brs = (m + t - 1) / t, bcs = (n + u - 1) / u, idx = 0;  \
    for (int bc = 0; bc < bcs; ++bc) \
        for (int br = 0; br < brs; ++br) \
            for (int j = 0; j < u; ++j) \
                for (int i = 0; i < t; ++i) { \
                    int r = br * t + i, c = bc * u + j; \
                    output[r * bcs * u + c] =   \
                        (r < m && c < n) ? input[idx] : (T)pad; \
                    ++idx; \
                } \
}

DEFINE_MATRIX_TRANSFORM_WITH_PADDING(int8_t,  int8)
DEFINE_MATRIX_TRANSFORM_WITH_PADDING(int16_t, int16)
DEFINE_MATRIX_TRANSFORM_WITH_PADDING(int32_t, int32)
DEFINE_MATRIX_TRANSFORM_WITH_PADDING(int64_t, int64)

static void row_major_to_Nz(const void *input, void *output,
    int m, int n, int t, int u, int elem_size, uint64_t p)
{
    switch (elem_size) {
    case 1:
        row_major_to_Nz_int8(input, output, m, n, t, u, p);
        break;
    case 2:
        row_major_to_Nz_int16(input, output, m, n, t, u, p);
        break;
    case 4:
        row_major_to_Nz_int32(input, output, m, n, t, u, p);
        break;
    case 8:
        row_major_to_Nz_int64(input, output, m, n, t, u, p);
        break;
    default:
        fprintf(stderr, "Unsupported element size %d\n", elem_size); exit(1);
    }
}

static void row_major_to_Zn(const void *input, void *output,
    int m, int n, int t, int u, int elem_size, uint64_t pad)
{
    switch (elem_size) {
    case 1:
        row_major_to_Zn_int8(input, output, m, n, t, u, pad);
        break;
    case 2:
        row_major_to_Zn_int16(input, output, m, n, t, u, pad);
        break;
    case 4:
        row_major_to_Zn_int32(input, output, m, n, t, u, pad);
        break;
    case 8:
        row_major_to_Zn_int64(input, output, m, n, t, u, pad);
        break;
    default:
        fprintf(stderr, "Unsupported element size %d\n", elem_size);
        exit(1);
    }
}

static void Nz_to_row_major(const void *input, void *output,
    int m, int n, int t, int u, int elem_size, uint64_t pad)
{
    switch (elem_size) {
    case 1:
        Nz_to_row_major_int8(input, output, m, n, t, u, pad);
        break;
    case 2:
        Nz_to_row_major_int16(input, output, m, n, t, u, pad);
        break;
    case 4:
        Nz_to_row_major_int32(input, output, m, n, t, u, pad);
        break;
    case 8:
        Nz_to_row_major_int64(input, output, m, n, t, u, pad);
        break;
    default:
        fprintf(stderr, "Unsupported element size %d\n", elem_size);
        exit(1);
    }
}

static void Zz_to_row_major(const void *input, void *output,
    int m, int n, int t, int u, int elem_size, uint64_t pad)
{
    switch (elem_size) {
    case 1:
        Zz_to_row_major_int8(input, output, m, n, t, u, pad);
        break;
    case 2:
        Zz_to_row_major_int16(input, output, m, n, t, u, pad);
        break;
    case 4:
        Zz_to_row_major_int32(input, output, m, n, t, u, pad);
        break;
    case 8:
        Zz_to_row_major_int64(input, output, m, n, t, u, pad);
        break;
    default:
        fprintf(stderr, "Unsupported element size %d\n", elem_size);
        exit(1);
    }
}

static void Nn_to_row_major(const void *input, void *output,
    int m, int n, int t, int u, int elem_size, uint64_t pad)
{
    switch (elem_size) {
    case 1:
        Nn_to_row_major_int8(input, output, m, n, t, u, pad);
        break;
    case 2:
        Nn_to_row_major_int16(input, output, m, n, t, u, pad);
        break;
    case 4:
        Nn_to_row_major_int32(input, output, m, n, t, u, pad);
        break;
    case 8:
        Nn_to_row_major_int64(input, output, m, n, t, u, pad);
        break;
    default:
        fprintf(stderr, "Unsupported element size %d\n", elem_size);
        exit(1);
    }
}

static void Zn_to_row_major(const void *input, void *output,
    int m, int n, int t, int u, int elem_size, uint64_t pad)
{
    switch (elem_size) {
    case 1:
        Zn_to_row_major_int8(input, output, m, n, t, u, pad);
        break;
    case 2:
        Zn_to_row_major_int16(input, output, m, n, t, u, pad);
        break;
    case 4:
        Zn_to_row_major_int32(input, output, m, n, t, u, pad);
        break;
    case 8:
        Zn_to_row_major_int64(input, output, m, n, t, u, pad);
        break;
    default:
        fprintf(stderr, "Unsupported element size %d\n", elem_size);
        exit(1);
    }
}

static bool colmajor_to_rowmajor(const void *src, void *dst,
    size_t rows, size_t cols, size_t elem_size)
{
#define COLMAJOR_TO_ROWMAJOR(T)                     \
do {                                                \
    const T *s = (const T *)src;                    \
    T *d = (T *)dst;                                \
    for (size_t i = 0; i < rows; ++i) {             \
        T *d_row = d + i * cols;                    \
        for (size_t j = 0; j < cols; ++j) {         \
            d_row[j] = s[j * rows + i];             \
        }                                           \
    }                                               \
} while (0)
    if (!src || !dst || rows == 0 || cols == 0) {
        return false;
    }
    switch (elem_size) {
    case 1: {
        COLMAJOR_TO_ROWMAJOR(uint8_t);
    } break;
    case 2: {
        COLMAJOR_TO_ROWMAJOR(uint16_t);
    } break;
    case 4: {
        COLMAJOR_TO_ROWMAJOR(uint32_t);
    } break;
    case 8: {
        COLMAJOR_TO_ROWMAJOR(uint64_t);
    } break;
    default:
        return false;
    }
    return true;
}


static bool rowmajor_to_colmajor(const void *src, void *dst,
                         size_t rows, size_t cols, size_t elem_size)
{
#define ROWMAJOR_TO_COLMAJOR(T)                     \
do {                                                \
    const T *s = (const T *)src;                    \
    T *d = (T *)dst;                                \
    for (size_t i = 0; i < rows; ++i) {             \
        for (size_t j = 0; j < cols; ++j) {         \
            T *d_row = d + j * rows + i;            \
            *d_row = s[i * cols + j];               \
        }                                           \
    }                                               \
} while (0)
    if (!src || !dst || rows == 0 || cols == 0) {
        return false;
    }
    switch (elem_size) {
    case 1: {
        ROWMAJOR_TO_COLMAJOR(uint8_t);
    } break;
    case 2: {
        ROWMAJOR_TO_COLMAJOR(uint16_t);
    } break;
    case 4: {
        ROWMAJOR_TO_COLMAJOR(uint32_t);
    } break;
    case 8: {
        ROWMAJOR_TO_COLMAJOR(uint64_t);
    } break;
    default:
        return false;
    }
    return true;
}


/*
 * void print_matrix(const int32_t* mat, int m, int n) {
 *     for (int i = 0; i < m; ++i) {
 *         for (int j = 0; j < n; ++j)
 *             printf("%4d", mat[i * n + j]);
 *         printf("\n");
 *     }
 * }
 */

static int getElemSize(SrcType typ)
{
    switch (typ) {
    case E5M2:
    case E4M3:
    case E8M0:
    case INT8:
    case UINT8:
    case S4x2:
    case E1M2x2:
    case E2M1x2:
    case U4x2:
        return 1;
    case BF16:
    case INT16:
    case FP16:
    case UINT16:
        return 2;
    case INT32:
    case FP32:
    case UINT32:
        return 4;
    case UINT64:
    case INT64:
    case FP64:
        return 8;
    default: {
        qemu_log("warnning element type!");
        return 1;
    };

    }
}

#define FOR_BEGIN()                     \
    for (int i = 0; i < m; ++i) {       \
        for (int j = 0; j < n; ++j) {

#define FOR_END()                       \
    }                                   \
    }

static uint32_t bfloat16_to_float32(uint16_t bf16_val)
{
    return ((uint32_t)bf16_val) << 16;
}


static uint32_t e8m0_to_f32(uint8_t v)
{
    float res;
    uint32_t f32;
    if (v == 0xFF) {
        assert(0);
    }
    res = powf(2.0f, v - 127);
    memcpy(&f32, &res,  4);
    return f32;
}

static uint32_t e6m2_to_f32(uint8_t v)
{
    float res;
    uint32_t f32;
    res = powf(2.0f, ((v >> 2) & 0x3f) - 48);
    res *= 1.0 + ((v & 0b10) >> 1) * 0.5 + (v & 0b1) * 0.25;
    memcpy(&f32, &res,  4);
    return f32;
}

static uint32_t e1m2_to_f32(uint8_t v)
{
    float res;
    uint32_t f32;
    float s, e, m;
    s = (v & 0b1000) == 0 ? 1 : -1;
    e = (v & 0b100) == 0 ? 0.5 : 1.0;
    m = 1.0 + ((v & 0b10) >> 1) * 0.5 + (v & 0b1) * 0.25;
    res = s * e * m;
    memcpy(&f32, &res,  4);
    return f32;
}

static uint32_t e2m1_to_f32(uint8_t v)
{
    float res;
    uint32_t f32;
    float s, e, m;
    s = ((v & 0b1000) == 0) ? 1 : -1;
    e = powf(2.0, (((v >> 1) & 0b11) - 1));
    m = 1 + (v & 0b1) * 0.5;

    res = s * e * m;
    memcpy(&f32, &res,  4);
    return f32;
}

static uint32_t HiF4_to_f32(uint8_t v)
{
    float res;
    uint32_t f32;
    float s, e, m;
    s = (v & 0b1000) == 0 ? 1 : -1;
    e = (v & 0b100) == 0 ? 0.5 : 1.0;
    m = 1.0 + ((v & 0b10) >> 1) * 0.5 + (v & 0b1) * 0.25;
    if (v == 0xF) {
        assert(0);
    }
    res = s * e * m;
    memcpy(&f32, &res,  4);
    return f32;
}

static uint32_t s4_to_int32(uint8_t v)
{
    int32_t res = v;
    uint32_t u32;
    res = v << 28;
    res = res >> 28;
    memcpy(&u32, &res, sizeof(u32));
    return u32;
}

static bool matrix_elem_trans(CPULINXState *env, uint64_t *src, uint64_t *dst,
    uint32_t m, uint32_t n, SrcType s_typ, SrcType d_typ)
{
    bool ret = true;
    if (s_typ == d_typ) {
        memcpy(dst, src, getElemSize(s_typ) * m * n);
        return ret;
    }

#define INT_TO_INT(ST, DT)                                  \
do {                                                        \
    FOR_BEGIN()                                             \
        ((DT *)dst)[i * n + j] = ((ST *)src)[i * n + j];    \
    FOR_END()                                               \
} while (0)
    switch (s_typ) {
    case INT8: {
        switch (d_typ) {
        case INT32: {
            INT_TO_INT(int8_t, int32_t);
            break;
        }
        case INT64: {
            INT_TO_INT(int8_t, int64_t);
            break;
        }
        case UINT32: {
            INT_TO_INT(int8_t, uint32_t);
            break;
        }
        case UINT64: {
            INT_TO_INT(int8_t, uint64_t);
            break;
        }
        default:
            ret = false;
        }
    } break;
    case UINT8: {
        switch (d_typ) {
        case INT32: {
            INT_TO_INT(int8_t, int32_t);
            break;
        }
        case INT64: {
            INT_TO_INT(int8_t, int64_t);
            break;
        }
        case UINT32: {
            INT_TO_INT(int8_t, uint32_t);
            break;
        }
        case UINT64: {
            INT_TO_INT(int8_t, uint64_t);
            break;
        }
        default:
            ret = false;
        }
    } break;
    case INT16: {
        switch (d_typ) {
        case INT32: {
            INT_TO_INT(uint8_t, int32_t);
            break;
        }
        case INT64: {
            INT_TO_INT(uint8_t, int64_t);
            break;
        }
        case UINT32: {
            INT_TO_INT(uint8_t, uint32_t);
            break;
        }
        case UINT64: {
            INT_TO_INT(uint8_t, uint64_t);
            break;
        }
        default:
            ret = false;
        }
    } break;
    case INT64: {
        switch (d_typ) {
        case INT32: {
            INT_TO_INT(int64_t, int32_t);
            break;
        }
        default:
            ret = false;
        }
    } break;
    case E5M2: {
        switch (d_typ) {
        case FP32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = (uint32_t)
                    helper_fcvt_s_lb(env, ((uint8_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case E4M3: {
        switch (d_typ) {
        case FP32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = (uint32_t)
                    helper_fcvt_s_b(env, ((uint8_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case E2M1x2: {
        switch (d_typ) {
        case FP32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = (uint32_t)
                    e2m1_to_f32(((uint8_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case E1M2x2: {
        switch (d_typ) {
        case FP32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = (uint32_t)
                    e1m2_to_f32(((uint8_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case HiF4x2: {
        switch (d_typ) {
        case FP32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = (uint32_t)
                    HiF4_to_f32(((uint8_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case U4x2: {
        switch (d_typ) {
        case UINT32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = ((uint8_t *)src)[i * n + j];
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case S4x2: {
        switch (d_typ) {
        case INT32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] =
                        s4_to_int32(((uint8_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case BF16: {
        switch (d_typ) {
        case FP32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = (uint32_t)
                    bfloat16_to_float32(((uint16_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case FP16: {
        switch (d_typ) {
        case FP32: {
            FOR_BEGIN()
                ((uint32_t *)dst)[i * n + j] = (uint32_t)
                    helper_fcvt_s_h(env, ((uint16_t *)src)[i * n + j]);
            FOR_END()
        } break;
        case FP64: {
            FOR_BEGIN()
                ((uint64_t *)dst)[i * n + j] = (uint64_t)
                    helper_fcvt_d_h(env, ((uint16_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case INT32: {
        switch (d_typ) {
        case UINT32: {
            INT_TO_INT(int32_t, uint32_t);
            break;
        }
        case UINT64: {
            INT_TO_INT(int32_t, uint64_t);
            break;
        }
        case INT64: {
            INT_TO_INT(int32_t, int64_t);
            break;
        }
        default:
            ret = false;
        }
    } break;
    case UINT32: {
        switch (d_typ) {
        case UINT32: {
            INT_TO_INT(uint32_t, uint32_t);
            break;
        }
        case UINT64: {
            INT_TO_INT(uint32_t, uint64_t);
            break;
        }
        case INT64: {
            INT_TO_INT(uint32_t, int64_t);
            break;
        }
        default:
            ret = false;
        }
    } break;
    case FP32: {
        switch (d_typ) {
        case FP32: {
            memcpy(dst, src, sizeof(uint32_t) * m * n);
        } break;
        case FP64: {
            FOR_BEGIN()
                ((uint64_t *)dst)[i * n + j] = (uint64_t)
                    helper_fcvt_d_s(env, ((uint32_t *)src)[i * n + j]);
            FOR_END()
        } break;
        default:
            ret = false;
        }
    } break;
    case HF32: {
        switch (d_typ) {
        case FP32: {
            assert(0);
        } break;
        default:
            ret = false;
        }
    } break;
    default:
        ret = false;
    }
    if (!ret) {
        printf("not support type convert!\n"
               "src type:%d, dest type:%d\n", s_typ, d_typ);
        assert(0);
    }
    return ret;
}

const int BUFFER_SIZE = 512;
const int ACC_BUFFER_SIZE = 1024;

static void mamulb(CPULINXState *env)
{
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst = (uint64_t *)env->to;

    uint32_t m = env->csr_lb[0];
    uint32_t n = env->csr_lb[1];
    uint32_t k = env->csr_lb[2];
    uint32_t s_typ = env->tileop_info.tileop_datatype;
    SrcType d_typ = get_acc_dtyp(s_typ);
    uint32_t elem_sz = 4;
    uint32_t tm, tn;
    tm = 16;
    tn = (BUFFER_SIZE / getElemSize(s_typ) + tm - 1) / tm;
    assert(m * k * elem_sz <= TILE_REG_MEM_TMP &&
           k * n * elem_sz <= TILE_REG_MEM_TMP &&
           m * n * elem_sz <= TILE_REG_MEM);
    uint64_t *t_src0 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *t_src1 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *tmp = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint32_t e_m, e_n, e_k;
    e_m = ((m + tm - 1) / tm) * tm;
    e_k = ((k + tn - 1) / tn) * tn;

    matrix_elem_trans(env, src0, tmp, e_m, e_k, s_typ, d_typ);
    Nz_to_row_major(tmp, t_src0, m, k, tm, tn, elem_sz, 0);
    tn = 16;
    tm = (BUFFER_SIZE / getElemSize(s_typ) + tn - 1) / tn;
    e_k = ((k + tm - 1) / tm) * tm;
    e_n = ((n + tn - 1) / tn) * tn;

    matrix_elem_trans(env, src1, tmp, e_k, e_n, s_typ, d_typ);

    Zn_to_row_major(tmp, t_src1, k, n, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)t_src0, e_m, e_k, d_typ, READ,
                          "Mamulb\nRow Major src0:");
    helper_matrix_memprnt(env, (uint64_t)t_src1, e_k, e_n, d_typ, READ,
                          "Row Major src1:");
    matrix_mul(env, (uint64_t)t_src0, (uint64_t)t_src1,
                    (uint64_t)tmp, e_m, e_n, e_k, d_typ);
    helper_matrix_memprnt(env, (uint64_t)tmp, e_m, e_n, d_typ, WRITE,
                          "Row Major result:");
    tm = tn = 16;
    row_major_to_Nz(tmp, dst, e_m, e_n, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)dst, e_m, e_n, d_typ, WRITE,
                          "Nz format result:");
    env->acc_data_typ = d_typ;

    free((uint8_t *)t_src0);
    free((uint8_t *)t_src1);
    free((uint8_t *)tmp);
    return;
}

static void mamulb_ac(CPULINXState *env)
{
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *src2 = (uint64_t *)env->tc;
    uint64_t *dst = (uint64_t *)env->to;

    uint32_t m = env->csr_lb[0];
    uint32_t n = env->csr_lb[1];
    uint32_t k = env->csr_lb[2];
    uint32_t s_typ = env->tileop_info.tileop_datatype;
    SrcType d_typ = get_acc_dtyp(s_typ);
    uint32_t elem_sz = 4;

    uint32_t tm, tn;
    tm = 16;
    tn = (BUFFER_SIZE / getElemSize(s_typ) + tm - 1) / tm;
    assert(m * k * elem_sz <= TILE_REG_MEM_TMP &&
           k * n * elem_sz <= TILE_REG_MEM_TMP &&
           m * n * elem_sz <= TILE_REG_MEM);
    uint64_t *t_src0 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *t_src1 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *tmp = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint32_t e_m, e_n, e_k;
    e_m = ((m + tm - 1) / tm) * tm;
    e_k = ((k + tn - 1) / tn) * tn;

    matrix_elem_trans(env, src0, tmp, e_m, e_k, s_typ, d_typ);
    Nz_to_row_major(tmp, t_src0, m, k, tm, tn, elem_sz, 0);

    helper_matrix_memprnt(env, (uint64_t)t_src0, e_m, e_k, s_typ, READ,
                          "MatrixAcc Src0:");
    tn = 16;
    tm = (BUFFER_SIZE / getElemSize(s_typ) + tn - 1) / tn;
    e_k = ((k + tm - 1) / tm) * tm;
    e_n = ((n + tn - 1) / tn) * tn;

    matrix_elem_trans(env, src1, tmp, e_k, e_n, s_typ, d_typ);
    Zn_to_row_major(tmp, t_src1, k, n, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)t_src1, e_k, e_n, s_typ, READ,
                          "Src1:");

    uint64_t *t_dst = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    matrix_mul(env, (uint64_t)t_src0, (uint64_t)t_src1,
                    (uint64_t)tmp, e_m, e_n, e_k, d_typ);
    tn = 16;
    tm = 16;
    row_major_to_Nz(tmp, t_dst, e_m, e_n, tm, tn, elem_sz, 0);
    if (env->tileop_info.tileop_type == TILEOP_MAMULBACC) {
        env->acc_data_typ = d_typ;
    }
    matrix_add(env, (uint64_t)t_dst, (uint64_t)src2,
               (uint64_t)dst, e_m, e_n, d_typ);
    Nz_to_row_major(t_dst, tmp, e_m, e_n, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)tmp, e_m, e_n, s_typ, READ,
                          "dst:");
    helper_matrix_memprnt(env, (uint64_t)dst, e_m, e_n, d_typ, WRITE, "");
    free((uint8_t *)t_dst);
    free((uint8_t *)t_src0);
    free((uint8_t *)t_src1);
    free((uint8_t *)tmp);
    return;
}
static void matrix_scale_a(CPULINXState *env, void *src_mat, void * scl_mat,
    uint32_t m, uint32_t n)
{
    int i, j, scl_i, scl_j;
    int tn;
    tn = n / 32;
    uint32_t *src = (uint32_t *)src_mat;
    uint8_t  *scl = (uint8_t *)scl_mat;
    for (i = 0; i < m; ++i) {
        for (j = 0; j < n; ++j) {
            scl_i = i;
            scl_j = j / 32;
            src[i * n + j] = (uint32_t)helper_fmul_s(env,
                src[i * n + j], e8m0_to_f32(scl[scl_i * tn + scl_j]));
        }
    }
    return;
}

static uint32_t scale_hif4(uint8_t *dat, int col_idx)
{
    float f32;
    uint32_t ret, ofb, ofc;
    int eb, ec;
    ofb = col_idx / 8;
    ofc = col_idx / 4;

    ret = e6m2_to_f32(*dat);
    memcpy(&f32, &ret, sizeof(f32));

    eb = (*(dat + 1) >> ofb) & 0b1;
    ec = (((uint16_t *)dat)[1] >> ofc) & 0b1;

    f32 *= powf(2.0, eb + ec);
    memcpy(&ret, &f32, sizeof(ret));
    return ret;
}


static void matrix_scale_a_hif4(CPULINXState *env, void *src_mat,
    void *scl_mat, uint32_t m, uint32_t n)
{
    int i, j, scl_i, scl_j;
    int tn;
    tn = n / 64 * 4;
    uint32_t *src = (uint32_t *)src_mat;
    uint8_t *scl = (uint8_t *)scl_mat;
    for (i = 0; i < m; ++i) {
        for (j = 0; j < n; ++j) {
            scl_i = i;
            scl_j = j / 64 * 4;
            src[i * n + j] = (uint32_t)helper_fmul_s(env,
                src[i * n + j], scale_hif4(&(scl[scl_i * tn + scl_j]), j));
        }
    }
    return;
}

static void matrix_scale_b_hif4(CPULINXState *env, void *src_mat,
    void *scl_mat, uint32_t m, uint32_t n)
{
    int i, j, scl_i, scl_j;
    int tn;
    uint8_t a, b, c, d;
    uint32_t dat;
    tn = n;
    uint32_t *src = (uint32_t *)src_mat;
    uint8_t  *scl = (uint8_t *)scl_mat;
    for (i = 0; i < m; ++i) {
        for (j = 0; j < n; ++j) {
            scl_i = i / 64;
            scl_j = j;
            dat = 0;
            a = scl[scl_i * tn + scl_j];
            b = scl[(scl_i + 1) * tn + scl_j];
            c = scl[(scl_i + 2) * tn + scl_j];
            d = scl[(scl_i + 3) * tn + scl_j];
            dat = ((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | a;
            src[i * n + j] = (uint32_t)helper_fmul_s(env,
                src[i * n + j], scale_hif4(((uint8_t *)&dat), i));
        }
    }
    return;
}

static void matrix_scale_b(CPULINXState *env, void *src_mat, void * scl_mat,
    uint32_t m, uint32_t n)
{
    int i, j, scl_i, scl_j;
    int tn;
    tn = n;
    uint32_t *src = (uint32_t *)src_mat;
    uint8_t  *scl = (uint8_t *)scl_mat;
    for (i = 0; i < m; ++i) {
        for (j = 0; j < n; ++j) {
            scl_i = i / 32;
            scl_j = j;
            src[i * n + j] = (uint32_t)helper_fmul_s(env,
                src[i * n + j], e8m0_to_f32(scl[scl_i * tn + scl_j]));
        }
    }
    return;
}


static void matrix_x2_split(uint8_t *src, uint8_t *dst, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        dst[2 * i] = src[i] & 0xf;
        dst[2 * i + 1] = (src[i] >> 4) & 0xf;
    }
    return;
}

static bool is_x2(SrcType typ)
{
    switch (typ) {
    case E1M2x2: return true;
    case E2M1x2: return true;
    case HiF4x2: return true;
    case U4x2: return true;
    case S4x2: return true;
    default:
    }
    return false;
}


static void mamulbmx(CPULINXState *env)
{

    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst = (uint64_t *)env->to;

    uint64_t *scl_a = NULL;
    uint64_t *scl_b = NULL;

    uint32_t m = env->csr_lb[0];
    uint32_t n = env->csr_lb[1];
    uint32_t k = env->csr_lb[2];
    uint32_t a_typ = env->tileop_info.tileop_datatype;
    uint32_t b_typ = get_blk_srctyp(env->header_info);
    bool x2 = is_x2(a_typ);
    SrcType d_typ = get_acc_dtyp(a_typ);
    uint32_t elem_sz = 4;
    uint32_t tm, tn;
    b_typ = b_typ == INVALID ? a_typ : b_typ;
    tm = 16;
    tn = (BUFFER_SIZE / getElemSize(a_typ) + tm - 1) / tm;
    if (x2) {
        tn *= 2;
    }
    assert(m * k * elem_sz <= TILE_REG_MEM_TMP &&
           k * n * elem_sz <= TILE_REG_MEM_TMP &&
           m * n * elem_sz <= TILE_REG_MEM);
    uint64_t *t_src0 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *t_src1 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *x2_t = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *tmp = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *tmp1 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint32_t e_m, e_n, e_k;
    e_m = ((m + tm - 1) / tm) * tm;
    e_k = ((k + tn - 1) / tn) * tn;

    if (x2) {
        matrix_x2_split((uint8_t *)src0, (uint8_t *)x2_t, e_m * e_k / 2);
        src0 = x2_t;
    }

    matrix_elem_trans(env, src0, tmp, e_m, e_k, a_typ, d_typ);
    Nz_to_row_major(tmp, t_src0, m, k, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)t_src0, e_m, e_k, d_typ, READ,
                          "MatrixMx Src0:");
    if (a_typ != FP16 && a_typ != BF16) {
        scl_a = (uint64_t *)env->tb;
        src1 = (uint64_t *)env->tc;
        scl_b =  (uint64_t *)env->td;
        uint32_t scl_m, scl_n, scl_tm, scl_tn, scl_elem_sz;

        scl_m = e_m;

        if (a_typ == HiF4x2) {
            scl_n = (e_k / 64) * 4;
        } else {
            scl_n = e_k / 32;
        }
        scl_elem_sz = 1;
        scl_tm = 16;
        scl_tn = 2;
        Zz_to_row_major(scl_a, tmp1, scl_m, scl_n,
                        scl_tm, scl_tn, scl_elem_sz, 0);
        helper_matrix_memprnt(env, (uint64_t)tmp1, scl_m, scl_n, E8M0, READ,
                        "scale_a:");
        if (a_typ == HiF4x2) {
            matrix_scale_a_hif4(env, t_src0, tmp1, e_m, e_k);
        } else {
            matrix_scale_a(env, t_src0, tmp1, e_m, e_k);
        }
        helper_matrix_memprnt(env, (uint64_t)t_src0, e_m, e_k, d_typ, READ,
                        "MatrixMx Scaled Src0:");
    } else {
        src1 = (uint64_t *)env->tb;
        scl_b =  (uint64_t *)env->tc;
    }

    x2 = is_x2(b_typ);
    tn = 16;
    tm = (BUFFER_SIZE / getElemSize(b_typ) + tn - 1) / tn;
    e_k = ((k + tm - 1) / tm) * tm;
    e_n = ((n + tn - 1) / tn) * tn;

    if (x2) {
        matrix_x2_split((uint8_t *)src1, (uint8_t *)x2_t, e_k * e_n / 2);
        src1 = x2_t;
    }

    matrix_elem_trans(env, src1, tmp, e_k, e_n, b_typ, d_typ);

    Zn_to_row_major(tmp, t_src1, k, n, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)t_src1, e_k, e_n, d_typ, READ,
                          "MatrixMx Src1:");
    if (b_typ != FP16 && b_typ != BF16) {
        uint32_t scl_m, scl_n, scl_tm, scl_tn, scl_elem_sz;

        if (b_typ == HiF4x2) {
            scl_m = (e_k / 64) * 4;
        } else {
            scl_m = e_k / 32;
        }
        scl_n = e_n;

        scl_elem_sz = 1;
        scl_tm = 2;
        scl_tn = 16;
        Nn_to_row_major(scl_b, tmp1, scl_m, scl_n,
                        scl_tm, scl_tn, scl_elem_sz, 0);
        helper_matrix_memprnt(env, (uint64_t)tmp1, scl_m, scl_n, E8M0, READ,
                "scale_b:");
        if (b_typ == HiF4x2) {
            matrix_scale_b_hif4(env, t_src1, tmp1, e_k, e_n);
        } else {
            matrix_scale_b(env, t_src1, tmp1, e_k, e_n);
        }
        helper_matrix_memprnt(env, (uint64_t)t_src1, e_k, e_n, d_typ, READ,
                        "MatrixMx Scaled Src1:");
    }

    matrix_mul(env, (uint64_t)t_src0, (uint64_t)t_src1,
                    (uint64_t)tmp, e_m, e_n, e_k, d_typ);
    helper_matrix_memprnt(env, (uint64_t)tmp, e_m, e_n, d_typ, READ,
                          "MatrixMx Result:");
    tm = tn = 16;
    row_major_to_Nz(tmp, dst, e_m, e_n, tm, tn, elem_sz, 0);
    env->acc_data_typ = d_typ;

    free((uint8_t *)t_src0);
    free((uint8_t *)t_src1);
    free((uint8_t *)x2_t);
    free((uint8_t *)tmp);
    free((uint8_t *)tmp1);
    return;
}


static void mamulbmx_ac(CPULINXState *env)
{

    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *src2 = (uint64_t *)env->tc;
    uint64_t *dst = (uint64_t *)env->to;

    uint64_t *scl_a = NULL;
    uint64_t *scl_b = NULL;

    uint32_t a_typ = env->tileop_info.tileop_datatype;
    uint32_t b_typ = get_blk_srctyp(env->header_info);
    SrcType d_typ = get_acc_dtyp(a_typ);
    b_typ = b_typ == INVALID ? a_typ : b_typ;
    bool x2 = is_x2(a_typ);

    uint32_t m = env->csr_lb[0];
    uint32_t n = env->csr_lb[1];
    uint32_t k = env->csr_lb[2];

    uint32_t elem_sz = 4;

    uint32_t tm, tn;
    tm = 16;
    tn = (BUFFER_SIZE / getElemSize(a_typ) + tm - 1) / tm;
    if (x2) {
        tn *= 2;
    }
    assert(m * k * elem_sz <= TILE_REG_MEM_TMP &&
           k * n * elem_sz <= TILE_REG_MEM_TMP &&
           m * n * elem_sz <= TILE_REG_MEM);
    uint64_t *t_src0 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *t_src1 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *x2_t = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *tmp = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint64_t *tmp1 = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    uint32_t e_m, e_n, e_k;
    e_m = ((m + tm - 1) / tm) * tm;
    e_k = ((k + tn - 1) / tn) * tn;
    if (x2) {
        matrix_x2_split((uint8_t *)src0, (uint8_t *)x2_t, e_m * e_k / 2);
        src0 = x2_t;
    }
    matrix_elem_trans(env, src0, tmp, e_m, e_k, a_typ, d_typ);
    Nz_to_row_major(tmp, t_src0, m, k, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)t_src0, e_m, e_k, d_typ, READ,
                        "MatrixMx Acc Src0:");
    if (a_typ != FP16 && a_typ != BF16) {
        scl_a = (uint64_t *)env->tb;
        src1 = (uint64_t *)env->tc;
        scl_b =  (uint64_t *)env->td;
        src2 =  (uint64_t *)env->te;
        uint32_t scl_m, scl_n, scl_tm, scl_tn, scl_elem_sz;

        scl_m = e_m;
        if (a_typ == HiF4x2) {
            scl_n = (e_k / 64) * 4;
        } else {
            scl_n = e_k / 32;
        }

        scl_elem_sz = 1;
        scl_tm = 16;
        scl_tn = 2;
        Zz_to_row_major(scl_a, tmp1, scl_m, scl_n,
                        scl_tm, scl_tn, scl_elem_sz, 0);
        if (a_typ == HiF4x2) {
            matrix_scale_a_hif4(env, t_src0, tmp1, e_m, e_k);
        } else {
            matrix_scale_a(env, t_src0, tmp1, e_m, e_k);
        }

        helper_matrix_memprnt(env, (uint64_t)t_src0, e_m, e_k, d_typ, READ,
                            "MatrixMx Acc Scaled Src0:");
    } else {
        src1 = (uint64_t *)env->tb;
        scl_b =  (uint64_t *)env->tc;
        src2 = (uint64_t *)env->td;
        assert(0);
    }

    x2 = is_x2(b_typ);
    tn = 16;
    tm = (BUFFER_SIZE / getElemSize(b_typ) + tn - 1) / tn;
    e_k = ((k + tm - 1) / tm) * tm;
    e_n = ((n + tn - 1) / tn) * tn;
    if (x2) {
        matrix_x2_split((uint8_t *)src1, (uint8_t *)x2_t, e_k * e_n / 2);
        src1 = x2_t;
    }
    matrix_elem_trans(env, src1, tmp, e_k, e_n, b_typ, d_typ);
    Zn_to_row_major(tmp, t_src1, k, n, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)t_src1, e_k, e_n, d_typ, READ,
                          "MatrixMx Acc Src1:");

    if (b_typ != FP16 && b_typ != BF16) {
        uint32_t scl_m, scl_n, scl_tm, scl_tn, scl_elem_sz;

        if (b_typ == HiF4x2) {
            scl_m = (e_k / 64) * 4;
        } else {
            scl_m = e_k / 32;
        }
        scl_n = e_n;

        scl_elem_sz = 1;
        scl_tm = 2;
        scl_tn = 16;
        Nn_to_row_major(scl_b, tmp1, scl_m, scl_n,
                        scl_tm, scl_tn, scl_elem_sz, 0);
        if (b_typ == HiF4x2) {
            matrix_scale_b_hif4(env, t_src1, tmp1, e_k, e_n);
        } else {
            matrix_scale_b(env, t_src1, tmp1, e_k, e_n);
        }
        helper_matrix_memprnt(env, (uint64_t)t_src1, e_k, e_n, d_typ, READ,
                            "MatrixMx Acc Scaled Src1:");
    } else {
        src2 = (uint64_t *)env->tc;
        assert(0);
    }
    uint64_t *t_dst = (uint64_t *)malloc(TILE_REG_MEM_TMP);
    matrix_mul(env, (uint64_t)t_src0, (uint64_t)t_src1,
                    (uint64_t)tmp, e_m, e_n, e_k, d_typ);
    tn = 16;
    tm = 16;
    row_major_to_Nz(tmp, t_dst, e_m, e_n, tm, tn, elem_sz, 0);
    env->acc_data_typ = d_typ;
    helper_matrix_memprnt(env, (uint64_t)src2, e_m, e_n, d_typ, READ,
                "src2:");

    matrix_add(env, (uint64_t)t_dst, (uint64_t)src2,
               (uint64_t)dst, e_m, e_n, d_typ);
    Nz_to_row_major(t_dst, tmp, e_m, e_n, tm, tn, elem_sz, 0);
    helper_matrix_memprnt(env, (uint64_t)tmp, e_m, e_n, d_typ, READ,
                          "dst:");
    helper_matrix_memprnt(env, (uint64_t)dst, e_m, e_n, d_typ, WRITE, "");
    free((uint8_t *)t_dst);
    free((uint8_t *)t_src0);
    free((uint8_t *)t_src1);
    free((uint8_t *)x2_t);
    free((uint8_t *)tmp);
    free((uint8_t *)tmp1);
    return;
}

static void canon_trans(const void *input, void *output,
    int m, int n,  int elem_sz)
{
    uint32_t tm = 16, tn = 16;
    uint32_t *tmp;

    tmp = (uint32_t *)malloc(TILE_REG_MEM);
    Nz_to_row_major(input, tmp, m, n, tm, tn, elem_sz, 0);
    row_major_to_Nz(tmp, output, m, n, tm, tn / 2, elem_sz, 0);
    free(tmp);
    return;
}

static bool matrix_nz_trans(CPULINXState *env, uint64_t *src, uint64_t *dst,
    uint32_t m, uint32_t n, uint32_t k, uint32_t elem_sz,
    MatrixTransTyp trans_typ, uint64_t pad, uint32_t buffer_size)
{
    uint32_t tm = 16;
    uint32_t tn = (buffer_size / elem_sz + tm - 1) / tm;
    void *t = NULL;
    assert(k == 1);

    switch (trans_typ) {
    case NORMAL:
        memcpy(dst, src, m * n * k * elem_sz);
        break;
    case ND2Nz:
        row_major_to_Nz(src, dst, m, n, tm, tn, elem_sz, pad);
        break;
    case Nz2ND:
        Nz_to_row_major(src, dst, m, n, tm, tn, elem_sz, pad);
        break;
    case Zn2ND:
        tn = 16;
        tm = (buffer_size / elem_sz + tn - 1) / tn;
        Zn_to_row_major(src, dst, m, n, tm, tn, elem_sz, pad);
        break;
    case ND2Zn:
        tn = 16;
        tm = (buffer_size / elem_sz + tn - 1) / tn;
        row_major_to_Zn(src, dst, m, n, tm, tn, elem_sz, pad);
        break;
    case CANON:
        canon_trans(src, dst, m, n, elem_sz);
        break;
    case DN2Nz: {
        t = malloc(m * n * elem_sz);
        colmajor_to_rowmajor(src, t, m, n, elem_sz);
        row_major_to_Nz(t, dst, m, n, tm, tn, elem_sz, pad);
        free(t);
    } break;
    case DN2Zn: {
        tn = 16;
        tm = (buffer_size / elem_sz + tn - 1) / tn;
        t = malloc(m * n * elem_sz);
        colmajor_to_rowmajor(src, t, m, n, elem_sz);
        row_major_to_Zn(t, dst, m, n, tm, tn, elem_sz, pad);
        free(t);
    } break;
    case Nz2DN: {
        t = malloc(TILE_REG_MEM_TMP);
        Nz_to_row_major(src, t, m, n, tm, tn, elem_sz, pad);
        rowmajor_to_colmajor(t, dst, m, n, elem_sz);
        free(t);
    } break;
    case Zn2DN: {
        t = malloc(TILE_REG_MEM_TMP);
        tn = 16;
        tm = (buffer_size / elem_sz + tn - 1) / tn;
        Zn_to_row_major(src, t, m, n, tm, tn, elem_sz, pad);
        rowmajor_to_colmajor(t, dst, m, n, elem_sz);
        free(t);
    } break;
    default:
        printf("matrix trans type %d is not supported!\n", trans_typ);
        assert(0);
        return false;
    }
    return true;
}

#ifndef CONFIG_USER_ONLY
static uint64_t read_by_size_vm(CPULINXState *env, void *base, int offset, int size)
{
    uint64_t addr = (uint64_t)base + size * offset;
    switch (size) {
    case 1: {
        return cpu_ldub_data(env, addr);
    }
    case 2: {
        return cpu_lduw_be_data(env, addr);
    }
    case 4:{
        return cpu_ldl_be_data(env, addr);
    }
    case 8:{
        return cpu_ldq_data(env, addr);
    }
    default:
        assert(0);
    }
    return 0;
}

static void write_by_size_vm(CPULINXState *env, void *base,
int offset, int size, uint64_t val)
{
    uint64_t addr = (uint64_t)base + size * offset;
    switch (size) {
    case 1: {
        cpu_stb_data_ra(env, addr, val, GETPC());
    } break;
    case 2: {
        cpu_stw_be_data_ra(env, addr, val, GETPC());
    } break;
    case 4:{
        cpu_stl_be_data_ra(env, addr, val, GETPC());
    } break;
    case 8:{
        cpu_stq_be_data_ra(env, addr, val, GETPC());
    } break;
    default:
        assert(0);
    }
}
#endif

static uint64_t read_by_size(void *base, int offset, int size)
{
    switch (size) {
    case 1: {
        return ((uint8_t *)base)[offset];
    }
    case 2: {
        return ((uint16_t *)base)[offset];
    }
    case 4:{
        return ((uint32_t *)base)[offset];
    }
    case 8:{
        return ((uint64_t *)base)[offset];
    }
    default:
        assert(0);
    }
    return 0;
}

static void write_by_size(void *base, int offset, int size, uint64_t val)
{
    switch (size) {
    case 1: {
        ((uint8_t *)base)[offset] = (uint8_t)val;
    } break;
    case 2: {
        ((uint16_t *)base)[offset] = (uint16_t)val;
    } break;
    case 4:{
        ((uint32_t *)base)[offset] = (uint32_t)val;
    } break;
    case 8:{
        ((uint64_t *)base)[offset] = val;
    } break;
    default:
        assert(0);
    }
}

static void split_output_tile(CPULINXState *env, void *src, uint32_t row,
    uint32_t col, MatrixStTyp s_typ, MatrixTransTyp layout,
    uint32_t elem_size)
{
    /* output sizes must be the same. */
    uint32_t offset = 0;
    uint32_t size = env->tile_attr[0].size;
    int num = env->tile_reg_dst_num;
    int s_row, s_col;
    void *tmp = malloc(TILE_REG_MEM_TMP);
    if (s_typ == ND) {
        s_row = row;
        s_col = col / num;
        assert(col % num == 0);
    } else {
        s_row = row / num;
        s_col = col;
        assert(row % num == 0);
    }
#define TILE_OUTPUT(cnd, out)                                               \
do {                                                                        \
    if (num >= cnd) {                                                       \
        if (s_typ == ND) {                                                  \
            colmajor_to_rowmajor((uint8_t *)src + offset,                   \
                tmp, s_row, s_col, elem_size);                              \
        } else {                                                            \
            rowmajor_to_colmajor((uint8_t *)src + offset,                   \
                tmp, s_row, s_col, elem_size);                              \
        }                                                                   \
        matrix_nz_trans(env, tmp, (uint64_t *)out, s_row, s_col, 1,         \
            elem_size, layout, 0, BUFFER_SIZE);                             \
        offset += size;                                                     \
    }                                                                       \
} while (0)
    TILE_OUTPUT(1, env->to);
    TILE_OUTPUT(2, env->to1);
    TILE_OUTPUT(3, env->to2);
    TILE_OUTPUT(4, env->to3);
    TILE_OUTPUT(5, env->to4);
    TILE_OUTPUT(6, env->to5);
    TILE_OUTPUT(7, env->to6);
    TILE_OUTPUT(8, env->to7);
    free(tmp);
    return;
}

static void unified_input_tile(CPULINXState *env, void *dst,
    uint32_t row, uint32_t col, MatrixTransTyp layout, uint32_t elem_size)
{
    int num = env->tile_reg_src_num;
    int size = env->ta_a.size, offset = 0;
    void *tmp = malloc(TILE_REG_MEM_TMP * 8);
    int s_row, s_col;
    MatrixStTyp d_typ = DN;
    if (layout == Zn2ND || layout == Nz2ND) {
        d_typ = ND;
    }

    if (d_typ == ND) {
        s_row = row;
        s_col = col / num;
        assert(col % num == 0);
    } else {
        s_row = row / num;
        s_col = col;
        assert(row % num == 0);
    }
#define UNIFIED_INPUT(cnd, in)                                              \
do {                                                                        \
    if (num >= cnd) {                                                       \
        matrix_nz_trans(env, (uint64_t *)in,                                \
            (uint64_t *)((uint8_t *)tmp + offset),                          \
            s_row, s_col, 1, elem_size, layout, 0, BUFFER_SIZE);            \
        memcpy((uint8_t *)tmp + offset, (uint8_t *)in, size);               \
        memcpy((uint8_t *)dst + offset, (uint8_t *)tmp + offset, size);     \
        offset += size;                                                     \
    }                                                                       \
} while (0)
    UNIFIED_INPUT(1, env->ta);
    UNIFIED_INPUT(2, env->tb);
    UNIFIED_INPUT(3, env->tc);
    UNIFIED_INPUT(4, env->td);
    UNIFIED_INPUT(5, env->te);
    UNIFIED_INPUT(6, env->tf);
    UNIFIED_INPUT(7, env->tg);
    UNIFIED_INPUT(8, env->th);


    return;
}

const uint64_t type_max_num[32] = {0x7FEFFFFFFFFFFFFF, 0x7F7FFFFF, 0, 0, 0x7BFF,
0, 0, 0x7e, 0x7B, 0, 0, 0, 0, 0, 0, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x7FFF,
0x7F, 0x7, 0, 0, 0, 0, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF, 0xFFFF, 0xFF, 0xF};
const uint64_t type_min_num[32] = {0xFFEFFFFFFFFFFFFF, 0xFF7FFFFF, 0, 0, 0xFBFF,
0, 0, 0xFE, 0xFB, 0, 0, 0, 0, 0, 0, 0x8000000000000000, 0x80000000, 0x8000,
0x80, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

static void tileop_memcopy(CPULINXState *env, int func, uint32_t dst_nu)
{
    const uint32_t BASE_ADDR_IDX = 0;
    const uint32_t STRIDE_IDX = 1;
    /*
     * Source matrix stores rows or columns first.
     * 0: row-major - ND, 1: column-major - DN
     */
    MatrixStTyp st_typ = ND;

    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t elm_sz = getElemSize(dataType);
    MatrixTransTyp layout = get_blk_format(env->header_info);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t colValid = env->csr_lb[0];
    uint64_t rowValid = env->csr_lb[1];
    uint64_t col = env->csr_lb[2] == 1 ? colValid : env->csr_lb[2];
    uint64_t row = func ?
        env->tile_attr[dst_nu].size * env->tile_reg_dst_num / (col * elm_sz) :
        env->ta_a.size * env->tile_reg_src_num / (col * elm_sz);
    uint64_t baseAddr = helper_get_ri_gpr(env, BASE_ADDR_IDX);

    if (layout == DN2Nz || layout == DN2Zn) {
        st_typ = DN;
    }

    uint64_t stride = st_typ == DN ? rowValid : colValid;

    if (env->ri_idx > STRIDE_IDX) {
        stride = helper_get_ri_gpr(env, STRIDE_IDX);
        assert(stride % elm_sz == 0);
        stride /= elm_sz;
    }

    if (st_typ == DN) {
        assert(stride >= rowValid);
    } else {
        assert(stride >= colValid);
    }

    if (colValid > col || rowValid > row) {
        printf("pc:0x%lx, argument invalid\n"
               "ColValid: %ld, Col:%ld, RowValid: %ld, Row: %ld\n",
               env->pc, colValid, col, rowValid, row);
        linx_raise_exception(env, LINX_EXCP_INSN_ILLEGAL, GETPC());
    }
    uint8_t *src = (uint8_t *)baseAddr;
#ifdef CONFIG_USER_ONLY
    src += guest_base;
#endif
    void *t_dst = malloc(TILE_REG_MEM_TMP);
    void *dst = malloc(TILE_REG_MEM_TMP);
    uint64_t val = 0;
    /*  TLOAD Global Memory -> Tile Register */
    if (func) {
        if (st_typ == ND) {
            for (int i = 0; i < row; ++i) {
                for (int j = 0; j < col; ++j) {
                    if (j < colValid && i < rowValid) {
#ifdef CONFIG_USER_ONLY
                        val = read_by_size(src, i * stride + j,
                                           elm_sz);
#else
                        val = read_by_size_vm(env, src, i * stride + j,
                                           elm_sz);
#endif
                    } else if (j >= colValid || i >= rowValid) {
                        val = p_val;
                    }
                    write_by_size(t_dst, i * col + j, elm_sz, val);
                }
            }
            rowmajor_to_colmajor(t_dst, dst, row, col, elm_sz);
        } else {
            for (int i = 0; i < col; ++i) {
                for (int j = 0; j < row; ++j) {
                    if (i < colValid && j < rowValid) {
#ifdef CONFIG_USER_ONLY
                        val = read_by_size(src, i * stride + j,
                                           elm_sz);
#else
                        val = read_by_size_vm(env, src, i * stride + j,
                                           elm_sz);
#endif
                    } else if (i >= colValid || j >= rowValid) {
                        val = p_val;
                    }
                    write_by_size(t_dst, i * row + j, elm_sz, val);
                }
            }
            colmajor_to_rowmajor(t_dst, dst, row, col, elm_sz);
        }
        helper_matrix_memprnt(env, (uint64_t)t_dst, row, col, dataType, READ,
                              "TLoad Instr");
        split_output_tile(env, dst, row, col, st_typ, layout, elm_sz);
        helper_matrix_memprnt(env, (uint64_t)dst, row, col, dataType, READ, "");
    } else {
        if (layout == Zn2ND || layout == Nz2ND) {
            st_typ = ND;
        } else {
            st_typ = DN;
        }
        /* TSTORE */
        void *ld_base = malloc(env->tile_reg_src_num * TILE_REG_MEM_TMP);
        unified_input_tile(env, ld_base, row, col, layout, elm_sz);
        void *st_base = (uint8_t *) baseAddr;
#ifdef CONFIG_USER_ONLY
        st_base += guest_base;
#endif
        helper_matrix_memprnt(env, (uint64_t)ld_base, row, col, dataType, READ,
                              "TStore:");
        matrix_nz_trans(env, ld_base, t_dst, row, col, 1,
                elm_sz, layout, p_val, BUFFER_SIZE);
        helper_matrix_memprnt(env, (uint64_t)t_dst, row, col, dataType, READ,
                              "TStore trans:");
        for (int i = 0; i < rowValid; ++i) {
            for (int j = 0; j < colValid; ++j) {
                val = read_by_size(t_dst, i * col + j, elm_sz);
#ifdef CONFIG_USER_ONLY
                write_by_size(st_base, i * stride + j, elm_sz, val);
#else
                write_by_size_vm(env, st_base, i * stride + j, elm_sz, val);
#endif
            }
        }
        free(ld_base);
    }
    free(t_dst);
    free(dst);
    return;
}
#ifndef CONFIG_USER_ONLY
static void tileop_esave(CPULINXState *env)
{
    uint8_t *writer = (uint8_t *)env->to;
    uint64_t ebarg = env->sysreg[env->priv].ebarg;
    /* save lb0/1/2 */
    for (int i = 0; i < 3; ++i) {
        ((uint16_t *)writer)[i] = (uint16_t)(env->csr_lb[i]);
    }
    writer += 8;

    for (int i = 0; i < 16; ++i) {
        if (i < 12) {
            ((uint64_t *)writer)[i] = env->blk_ri[i];
        } else {
            ((uint64_t *)writer)[i] = env->blk_ro[i - 12];
        }
    }
    writer += 128;

    ((uint64_t *)writer)[0] = env->pc;
    writer += 8;

    /* save lc0/1/2 */
    for (int i = 0; i < 3; ++i) {
        ((uint16_t *)writer)[i] = (uint16_t)(env->csr_lc[i]);
    }
    writer += 8;

    ((uint64_t *)writer)[0] = env->predm;
    writer += 8;

    for (int i = 0; i < 8; i++) {
        if (i < 4) {
            ((uint64_t *)writer)[i] = env->blk_t[4 - i];
        } else {
            ((uint64_t *)writer)[i] = env->blk_u[8 - i];
        }
    }
    writer += 64;

    for (int i = 0; i < 16; i++) {
        if (i < 4) {
            write_by_size(writer, 256 * i, 8, env->fvec_t[0][4 - i]);
        } else if (i < 8) {
            write_by_size(writer, 256 * i, 8, env->fvec_u[0][8 - i]);
        } else if (i < 12) {
            write_by_size(writer, 256 * i, 8, env->fvec_m[0][12 - i]);
        } else {
            write_by_size(writer, 256 * i, 8, env->fvec_n[0][16 - i]);
        }
    }
    writer += 4096;
    uint64_t *dst = (uint64_t *)get_tile_src(env,
                        get_field(ebarg, EBARG_REGDST0));
    memcpy(writer, dst, TILE_REG_MEM);
    writer += TILE_REG_MEM;

    dst = (uint64_t *)get_tile_src(env, get_field(ebarg, EBARG_REGDST1));
    memcpy(writer, dst, TILE_REG_MEM);
    writer += TILE_REG_MEM;

    dst = (uint64_t *)get_tile_src(env, get_field(ebarg, EBARG_REGDST2));
    memcpy(writer, dst, TILE_REG_MEM);
    writer += TILE_REG_MEM;

    dst = (uint64_t *)get_tile_src(env, get_field(ebarg, EBARG_REGDST3));
    memcpy(writer, dst, TILE_REG_MEM);
    return;
}

static void tileop_ercov(CPULINXState *env)
{
    uint64_t ebarg = env->sysreg[env->priv].ebarg;
    uint8_t *reader = (uint8_t *)env->ta;
    for (int i = 0; i < 3; ++i) {
        env->csr_lb[i] = ((uint16_t *)reader)[i];
    }
    reader += 8;

    for (int i = 0; i < 16; ++i) {
        if (i < 12) {
            env->blk_ri[i] = ((uint64_t *)reader)[i];
        } else {
            env->blk_ro[i - 12] = ((uint64_t *)reader)[i];
        }
    }
    reader += 128;

    env->pc = ((uint64_t *)reader)[0];
    reader += 8;

    /* recover lc0/1/2 */
    for (int i = 0; i < 3; ++i) {
        env->csr_lc[i] = ((uint16_t *)reader)[i];
    }
    reader += 8;

    env->predm = ((uint64_t *)reader)[0];
    reader += 8;

    for (int i = 0; i < 8; i++) {
        if (i < 4) {
            env->blk_t[4 - i] = ((uint64_t *)reader)[i];
        } else {
            env->blk_u[8 - i] = ((uint64_t *)reader)[i];
        }
    }
    reader += 64;

    for (int i = 0; i < 16; i++) {
        if (i < 4) {
            env->fvec_t[0][4 - i] = read_by_size(reader, 256 * i, 8);
        } else if (i < 8) {
            env->fvec_t[0][8 - i] = read_by_size(reader, 256 * i, 8);
        } else if (i < 12) {
            env->fvec_t[0][12 - i] = read_by_size(reader, 256 * i, 8);
        } else {
            env->fvec_t[0][16 - i] = read_by_size(reader, 256 * i, 8);
        }
    }
    reader += 4096;
    uint64_t *dst = (uint64_t *)get_tile_src(env,
                        get_field(ebarg, EBARG_REGDST0));
    memcpy(dst, reader, TILE_REG_MEM);
    reader += TILE_REG_MEM;

    dst = (uint64_t *)get_tile_src(env, get_field(ebarg, EBARG_REGDST1));
    memcpy(dst, reader, TILE_REG_MEM);
    reader += TILE_REG_MEM;

    dst = (uint64_t *)get_tile_src(env, get_field(ebarg, EBARG_REGDST2));
    memcpy(dst, reader, TILE_REG_MEM);
    reader += TILE_REG_MEM;

    dst = (uint64_t *)get_tile_src(env, get_field(ebarg, EBARG_REGDST3));
    memcpy(dst, reader, TILE_REG_MEM);
    return;
}

#else

static void tileop_esave(CPULINXState *env) {return; }
static void tileop_ercov(CPULINXState *env) {return; }

#endif

static uint32_t data_scale(uint32_t dat, uint64_t scal, SrcType typ)
{
    float fa = 0, fb = 0;
    int ia = 0, ib = 0;
    uint32_t ua, ub;
    switch (typ) {
    case UINT32:
        ua = dat;
        ub = (uint32_t)scal;
        ua *= ub;
        return ua;
    case INT32:
        ia = (int)dat;
        ib = (int)scal;
        ia *= ib;
        return ia;
    case FP32:
        fa = *((float *)&dat);
        fb = *((float *)&scal);
        fa *= fb;
        return *((uint32_t *)&fa);
    default:
        assert(0);
    }
    return -1;
}

static void matrix_rowmax(const uint32_t *mat, uint32_t *dst,
    uint64_t row, uint64_t col, SrcType typ)
{
    float fmax;
    uint32_t umaxn;
    int imax, i, j;
    switch (typ) {
    case UINT32:
        for (i = 0; i < row; ++i) {
            umaxn = mat[0];
            for (j = 0; j < col; ++j) {
                umaxn = umaxn > mat[i * col + j] ? umaxn : mat[i * col + j];
            }
            dst[i] = umaxn;
        } break;
    case INT32:
        for (i = 0; i < row; ++i) {
            imax = ((int *)mat)[0];
            for (j = 0; j < col; ++j) {
                imax = imax > ((int *)mat)[i * col + j] ?
                       imax : ((int *)mat)[i * col + j];
            }
            dst[i] = (uint32_t)imax;
        } break;
    case FP32:
        for (i = 0; i < row; ++i) {
            fmax = ((float *)mat)[0];
            for (j = 0; j < col; ++j) {
                fmax = fmax > ((float *)mat)[i * col + j] ?
                       fmax : ((float *)mat)[i * col + j];
            }
            dst[i] = *((uint32_t *)&fmax);
        } break;
    default:
        assert(0);
    }
    return;
}

static void acccvt(CPULINXState *env)
{
    uint64_t lb0 = env->csr_lb[0];
    uint64_t lb1 = env->csr_lb[1];
    uint64_t lb2 = env->csr_lb[2];
    uint64_t scal = env->ri_idx > 0 ? env->gpr[env->blk_ri[0]] : 1;
    uint64_t *tmp = (uint64_t *) malloc(TILE_REG_MEM_TMP);
    uint64_t *tmp1 = (uint64_t *) malloc(TILE_REG_MEM_TMP);
    uint64_t *acc_cvt =  (uint64_t *) malloc(TILE_REG_MEM_TMP);
    uint32_t buffer_size = BUFFER_SIZE * 2;
    SrcType dat_typ = env->acc_data_typ;
    memcpy(acc_cvt, env->acc, TILE_REG_MEM);

    helper_matrix_memprnt(env, (uint64_t)acc_cvt, lb0,
                          lb1, dat_typ, READ, "acc:");

    if (get_canon(env->header_info)) {
        matrix_nz_trans(env, (uint64_t *)env->acc, acc_cvt,
                    lb0, lb1, lb2, getElemSize(dat_typ),
                    CANON, 0, buffer_size);
        buffer_size /= 2;
        helper_matrix_memprnt(env, (uint64_t)acc_cvt, lb0, lb1,
                              dat_typ, READ, "after canon:");
    }

    /* element scaling part */
    assert(dat_typ == get_blk_srctyp(env->header_info));
    if (env->ri_idx > 0) {
        for (int i = 0; i < lb0 * lb1; i++) {
            ((uint32_t *)tmp)[i] =
                data_scale(((uint32_t *)acc_cvt)[i], scal, dat_typ);
        }
    } else {
        /* uscaled */
        memcpy(tmp, acc_cvt, TILE_REG_MEM_TMP);
    }

    if (env->tile_reg_dst_num == 2) {
        /* Solve and output rowmax */
        matrix_nz_trans(env, tmp, tmp1, lb0, lb1, lb2,
                        getElemSize(dat_typ), Nz2ND, 0, buffer_size);
        uint32_t *tmp2 = (uint32_t *)malloc(lb0 * sizeof(uint32_t));
        matrix_rowmax((uint32_t *)tmp1, tmp2, lb0, lb1, dat_typ);
        matrix_elem_trans(env, (uint64_t *)tmp2, (uint64_t *)env->to1, lb0,
                        lb1, dat_typ, env->tileop_info.tileop_datatype);
    }


    /* NZ type conversion and element type conversion, and output. */
    matrix_nz_trans(env, tmp, acc_cvt, lb0, lb1, lb2,
                    getElemSize(dat_typ),
                    get_blk_format(env->header_info), 0,
                    buffer_size);
    helper_matrix_memprnt(env, (uint64_t)acc_cvt, lb0,
                          lb1, dat_typ, READ, "after nz:");

    matrix_elem_trans(env, acc_cvt, (uint64_t *)env->to, lb0,
                      lb1, dat_typ, env->tileop_info.tileop_datatype);

    free(acc_cvt);
    free(tmp);
    return;
}

static void mem_gather(CPULINXState *env)
{
    uint64_t col = env->csr_lb[0];
    uint64_t row = env->csr_lb[1];
    uint32_t elm_sz = getElemSize(env->tileop_info.tileop_datatype);
    uint64_t *off_base = (uint64_t *)env->ta;
    uint32_t of_elm_sz = getElemSize(env->ta_a.dtyp);
    void *dst = (void *)env->to;
    void *dat_base = (void *)env->gpr[env->blk_ri[0]];
    uint64_t val = 0;
    uint32_t offset = 0;
#ifdef CONFIG_USER_ONLY
    dat_base += guest_base;
#endif
    for (int i = 0; i < row; i++) {
        for (int j = 0; j < col; j++) {
            offset = read_by_size(off_base, i * col + j, of_elm_sz);
            assert(offset % elm_sz == 0);
            offset /= elm_sz;
#ifdef CONFIG_USER_ONLY
            val = read_by_size(dat_base, offset, elm_sz);
#else
            val = read_by_size_vm(env, dat_base, offset, elm_sz);
#endif
            write_by_size(dst, i * col + j, elm_sz, val);
        }
    }
    helper_matrix_memprnt(env, (uint64_t)dst, row, col, E8M0, READ,
                                    "mem_gather:");
    return;
}

static void mem_scatter(CPULINXState *env)
{
    uint64_t col = env->csr_lb[0];
    uint64_t row = env->csr_lb[1];
    uint32_t elm_sz = getElemSize(env->tileop_info.tileop_datatype);
    void *dat_base = (void *)env->ta;
    uint16_t *off_base = (uint16_t *)env->tb;
    uint32_t of_elm_sz = getElemSize(env->tb_a.dtyp);
    void *dst_base = (void *)env->gpr[env->blk_ri[0]];
    uint64_t val = 0;
#ifdef CONFIG_USER_ONLY
    dst_base += guest_base;
#endif
    for (int i = 0; i < row; i++) {
        for (int j = 0; j < col; j++) {
            uint32_t offset =
                read_by_size(off_base, i * col + j, of_elm_sz);
            val = read_by_size(dat_base, i * col + j, elm_sz);
            assert(offset % elm_sz == 0);
            offset /= elm_sz;
#ifdef CONFIG_USER_ONLY
            write_by_size(dst_base, offset, elm_sz, val);
#else
            write_by_size_vm(env, dst_base, offset, elm_sz, val);
#endif
        }
    }
    return;
}

static void tadd(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;

    for (i = 0; i < col; ++i) {
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[i];
            uint32_t b = ((uint32_t *)src1)[i];
            if (i >= colvalid || j >= rowvalid) {
                a = b = p_val;
            }
            uint32_t c = (uint32_t)helper_fadd_s(env, a, b);
            ((uint32_t *)dst)[i] = c;
        }
    }
    return;
}

static void tmax(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;

    for (i = 0; i < col; ++i) {
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[i];
            uint32_t b = ((uint32_t *)src1)[i];
            if (i >= colvalid || j >= rowvalid) {
                a = b = p_val;
            }
            uint32_t c = (uint32_t)helper_fmax_s(env, a, b);
            ((uint32_t *)dst)[i] = c;
        }
    }
    return;
}

static void tsub(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;

    for (i = 0; i < col; ++i) {
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[i];
            uint32_t b = ((uint32_t *)src1)[i];
            if (i >= colvalid || j >= rowvalid) {
                a = b = p_val;
            }
            uint32_t c = (uint32_t)helper_fsub_s(env, a, b);
            ((uint32_t *)dst)[i] = c;
        }
    }
    return;
}

static void texp(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;

    for (i = 0; i < col; ++i) {
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[i];
            if (i >= colvalid || j >= rowvalid) {
                a = p_val;
            }
            uint32_t c = (uint32_t)helper_fexp_s(env, a);
            ((uint32_t *)dst)[i] = c;
        }
    }
    return;
}

static void tmul(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;

    for (i = 0; i < col; ++i) {
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[i];
            uint32_t b = ((uint32_t *)src1)[i];
            if (i >= colvalid || j >= rowvalid) {
                a = b = p_val;
            }
            uint32_t c = (uint32_t)helper_fmul_s(env, a, b);
            ((uint32_t *)dst)[i] = c;
        }
    }
    return;
}


static void trowmax(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < row; ++i) {
        uint32_t max = ((uint32_t *)src0)[i * col];
        if (i >= colvalid) {
            ((uint32_t *)dst)[i] = p_val;
            continue;
        }
        for (j = 0; j < col; ++j) {
            uint32_t a = ((uint32_t *)src0)[i * col + j];
            if (j >= colvalid || i >= rowvalid) {
                a = p_val;
            }
            max = (uint32_t)helper_fmax_s(env, a, max);
        }
        ((uint32_t *)dst)[i] = max;
    }
    return;
}


static void tmuls(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t s     = env->gpr[env->blk_ri[0]];
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < row; ++i) {
        for (j = 0; j < col; ++j) {
            uint32_t a = ((uint32_t *)src0)[i * col + j];
            if (i >= colvalid || j >= rowvalid) {
                a = p_val;
            }
            uint32_t res = (uint32_t)helper_fmul_s(env, a, s);
            ((uint32_t *)dst)[i * col + j] = res;
        }
    }
    return;
}

static void trowsum(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < row; ++i) {
        uint32_t sum = 0;
        for (j = 0; j < col; ++j) {
            uint32_t a = ((uint32_t *)src0)[i * col + j];
            if (j >= colvalid || i >= rowvalid) {
                a = p_val;
            }
            sum = (uint32_t)helper_fadd_s(env, a, sum);

        }
        ((uint32_t *)dst)[i] = sum;
    }
    return;
}


static void trecip(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j, fone;
    float ft = 1;
    fone = *((uint32_t *)(&ft));
    for (i = 0; i < col; ++i) {
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[i];
            if (i >= colvalid || j >= rowvalid) {
                a = p_val;
            }
            uint32_t c = (uint32_t)helper_fdiv_s(env, fone, a);
            ((uint32_t *)dst)[i] = c;
        }
    }
    return;
}

static void texpands(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->tile_attr[0].size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t s     = env->gpr[env->blk_ri[0]];
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < row; ++i) {
        for (j = 0; j < col; ++j) {
            if (i >= rowvalid || j >= colvalid) {
                ((uint32_t *)dst)[i * col + j] = p_val;
            } else {
                ((uint32_t *)dst)[i * col + j] = s;
            }
        }
    }
    return;
}

static void trow_expand(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < row; ++i) {
        uint32_t s = ((uint32_t *)src0)[i * col];
        for (j = 0; j < col; ++j) {
            if (i >= rowvalid || j >= colvalid) {
                ((uint32_t *)dst)[i * col + j] = p_val;
            } else {
                ((uint32_t *)dst)[i * col + j] = s;
            }
        }
    }
    return;
}

static void tcolmax(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < col; ++i) {
        uint32_t max = ((uint32_t *)src0)[i];
        if (i >= colvalid) {
            ((uint32_t *)dst)[i] = p_val;
            continue;
        }
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[j * col + i];
            if (j >= rowvalid) {
                a = p_val;
            }
            max = (uint32_t)helper_fmax_s(env, a, max);
        }
        ((uint32_t *)dst)[i] = max;
    }
    return;
}

static void tcolsum(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < col; ++i) {
        uint32_t sum = 0;
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[j * col + i];
            if (i >= colvalid || j >= rowvalid) {
                a = p_val;
            }
            sum = (uint32_t)helper_fadd_s(env, a, sum);
        }
        ((uint32_t *)dst)[i] = sum;
    }
    return;
}

static void tcol_expand_sub(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < col; ++i) {
        uint32_t s = ((uint32_t *)src1)[i];
        if (i >= colvalid) {
            s = p_val;
        }
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[j * col + i];
            if (j >= rowvalid) {
                a = p_val;
            }
            uint32_t res = (uint32_t)helper_fsub_s(env, a, s);
            ((uint32_t *)dst)[j * col + i] = res;
        }
    }
    return;
}

static void tcol_expand_mul(CPULINXState *env)
{
    SrcType dataType = env->tileop_info.tileop_datatype;
    uint64_t src_siz = env->ta_a.size;
    uint32_t elem_sz = getElemSize(dataType);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col      = env->csr_lb[2];
    uint64_t row      = src_siz / (col * elem_sz);
    uint32_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dataType);
    uint64_t *src0 = (uint64_t *)env->ta;
    uint64_t *src1 = (uint64_t *)env->tb;
    uint64_t *dst  = (uint64_t *)env->to;
    assert(dataType == FP32);

    uint32_t i, j;
    for (i = 0; i < col; ++i) {
        uint32_t s = ((uint32_t *)src1)[i];
        if (i >= colvalid) {
                s = p_val;
        }
        for (j = 0; j < row; ++j) {
            uint32_t a = ((uint32_t *)src0)[j * col + i];
            if (j >= rowvalid) {
                a = p_val;
            }
            uint32_t res = (uint32_t)helper_fmul_s(env, a, s);
            ((uint32_t *)dst)[j * col + i] = res;
        }
    }
    return;
}

static FloatRoundMode get_real_round_mode(uint64_t val)
{
    enum {NONE = 0, RNE = 1, RTZ, RDN, RUP, RNA, RTO, RHB, InvalidR};
    switch (val) {
    case NONE:
    case RNE: return float_round_nearest_even;
    case RTZ: return float_round_to_zero;
    case RDN: return float_round_down;
    case RUP: return float_round_up;
    case RNA: return float_round_ties_away;
    case RTO: return float_round_to_odd;
    case RHB: return float_round_nearest_even;
    }
    assert(0);
    return 0;
}

static void tcvt(CPULINXState *env)
{
    SrcType srcTyp = env->tileop_info.tileop_datatype;
    SrcType dstTyp = get_blk_srctyp(env->header_info);
    uint32_t s_elem_sz = getElemSize(srcTyp);
    uint32_t d_elem_sz = getElemSize(dstTyp);
    uint64_t colvalid = env->csr_lb[0];
    uint64_t rowvalid = env->csr_lb[1];
    uint64_t col = env->csr_lb[2];
    uint64_t row = env->ta_a.size / (col * s_elem_sz);
    uint64_t pad = get_blk_pad(env->header_info);
    uint64_t p_val = get_pad_value(pad, dstTyp);
    uint64_t rmode = get_blk_rmode(env->header_info);
    uint64_t *src = (uint64_t *)env->ta;
    uint64_t *dst = (uint64_t *)env->to;

    helper_set_rounding_mode(env, get_real_round_mode(rmode));

    matrix_elem_trans(env, src, dst, col, row, srcTyp, dstTyp);
    for (int i = 0; i < row; ++i) {
        for (int j = 0; j < col; ++j) {
            if (i >= rowvalid || j > colvalid) {
                switch (d_elem_sz) {
                case 1: {
                    ((uint8_t *)dst)[i * col + j] = (uint8_t)p_val;
                    } break;
                case 2: {
                    ((uint16_t *)dst)[i * col + j] = (uint16_t)p_val;
                } break;
                case 4: {
                    ((uint32_t *)dst)[i * col + j] = (uint32_t)p_val;
                } break;
                case 8: {
                    ((uint64_t *)dst)[i * col + j] = (uint64_t)p_val;
                } break;
                }
            }
        }
    }
    helper_set_rounding_mode(env, float_round_nearest_even);
    return;
}

static void tmov(CPULINXState *env)
{
    SrcType datTyp = env->tileop_info.tileop_datatype;
    uint32_t elem_sz = getElemSize(datTyp);
    MatrixTransTyp layout = get_blk_format(env->header_info);
    uint64_t col = env->csr_lb[2];
    col = col == 1 ? env->csr_lb[0] : col;
    uint64_t row = env->ta_a.size / (col * elem_sz);
    uint64_t pad = get_blk_pad(env->header_info);
    uint64_t *src = (uint64_t *)env->ta;
    uint64_t *dst = (uint64_t *)env->to;
    uint64_t p_val = get_pad_value(pad, datTyp);
    uint64_t size = env->tile_attr[0].size;
    matrix_nz_trans(env, src, dst, row, col, 1, elem_sz, layout, p_val, size);

    return;
}

static uint8_t round_mantissa(uint8_t mant, bool guard, bool round, bool sticky,
                              bool sign, FloatRoundMode mode)
{
    bool increment = false;
    switch (mode) {
        case float_round_nearest_even:
            /* ties to even: increment if guard=1 and (round|sticky|mant LSB=1) */
            increment = guard && (round || sticky || (mant & 1));
            break;
        case float_round_to_zero:
            increment = false;
            break;
        case float_round_up:
            increment = (!sign) && (guard || round || sticky);
            break;
        case float_round_down:
            increment = sign && (guard || round || sticky);
            break;
        default:
            /* float_round_to_zero */
            increment = false;
            break;
    }
    return mant + (increment ? 1 : 0);
}

static uint8_t bf16_to_fp4(uint16_t x, FloatRoundMode mode)
{
    // Extract fields
    uint16_t sign = x >> 15;
    uint16_t exp  = (x >> 7) & 0xFF;
    uint16_t frac = x & 0x7F;

    /* Handle special cases: NaN / Inf / zero / subnorm */
    if (exp == 0xFF) {
        if (frac != 0) {
            /*  map NaN: sign bit preserved, exponent=1 (max), mantissa=11 (all 1) */
            return (sign << 3) | 0b111;
        } else {
            return (sign << 3) | 0b100;
        }
    }

    if (exp == 0 && frac == 0) {
        return (sign << 3);
    }

    int32_t e = (int32_t)exp - 127;
    int32_t fp4_exp = e + 1;

    uint32_t implicit_one = (exp != 0) ? 1 : 0;

    uint32_t full_mant = (implicit_one << 7) | frac;

    int32_t fp_exp;
    uint32_t fp_mant;

    if (fp4_exp >= 2) {
        return (sign << 3) | 0b100;
    } else if (fp4_exp <= 0) {
        int shift = 1 - fp4_exp;
        if (shift > 8) {
            return (sign << 3);
        }
        uint32_t shifted = full_mant >> shift;
        bool guard = (full_mant >> (shift - 1)) & 1;
        bool round = (shift >= 2) ? ((full_mant >> (shift - 2)) & 1) : 0;
        bool sticky = (shift > 2) ? ((full_mant & ((1u << (shift - 2)) - 1)) != 0) : 0;
        fp_mant = shifted & 0x3;
        uint8_t new_mant = round_mantissa(fp_mant, guard, round, sticky,
                                          sign, mode);
        if (new_mant >= 4) {
            fp_exp = 1;
            fp_mant = 0;
        } else {
            fp_exp = 0;
            fp_mant = new_mant;
        }
    } else {
        fp_exp = fp4_exp;
        uint32_t mant_main = full_mant >> 5;
        uint8_t mant2 = mant_main & 0x3;
        bool guard = (full_mant >> 4) & 1;
        bool round = (full_mant >> 3) & 1;
        bool sticky = (full_mant & 0x7) != 0;
        uint8_t new_mant = round_mantissa(mant2, guard, round, sticky,
                                          sign, mode);
        if (new_mant >= 4) {
            fp_exp += 1;
            new_mant = 0;
            if (fp_exp >= 2) {
                return (sign << 3) | 0b100;
            }
        }
        fp_mant = new_mant;
    }

    return (sign << 3) | ((fp_exp & 0x1) << 2) | (fp_mant & 0x3);
}

uint64_t helper_bf_to_hif4x2(uint64_t srcl, uint64_t srcr, int32_t rm)
{
    uint8_t src_h = bf16_to_fp4(srcl, get_real_round_mode(rm));
    uint8_t src_l = bf16_to_fp4(srcr, get_real_round_mode(rm));
    return (src_h << 4 | src_l);
}

uint64_t helper_bf_to_bf16x2(uint64_t srcl, uint64_t srcr)
{
    uint64_t src_h = srcl & 0xFFFF;
    uint64_t src_l = srcr & 0xFFFF;
    return (src_h << 16) | src_l;
}

void helper_tileop(CPULINXState *env)
{
    init_tile_outupt(env);
    switch (env->tileop_info.tileop_type) {
    case TILEOP_TADD: {
        tadd(env);
    } break;
    case TILEOP_TMAX: {
        tmax(env);
    } break;
    case TILEOP_TSUB: {
        tsub(env);
    } break;
    case TILEOP_TEXP: {
        texp(env);
    } break;
    case TILEOP_TMUL: {
        tmul(env);
    } break;
    case TILEOP_TROWMAX: {
        trowmax(env);
    } break;
    case TILEOP_TMULS: {
        tmuls(env);
    } break;
    case TILEOP_TROWSUM: {
        trowsum(env);
    } break;
    case TILEOP_TRECIP: {
        trecip(env);
    } break;
    case TILEOP_TEXPANDS: {
        texpands(env);
    } break;
    case TILEOP_TCOLMAX: {
        tcolmax(env);
    } break;
    case TILEOP_TCOLEXPANDSUB: {
        tcol_expand_sub(env);
    } break;
    case TILEOP_TCOLSUM: {
        tcolsum(env);
    } break;
    case TILEOP_TCOLEXPANDMUL: {
        tcol_expand_mul(env);
    } break;

    case TILEOP_TROWEXPAND: {
        trow_expand(env);
    } break;
    case TILEOP_MAMULB:{
        mamulb(env);
    } break;
    case TILEOP_MAMULBAC:{
        mamulb_ac(env);
    } break;
    case TILEOP_MAMULBACC:{
        env->tc = (uint64_t)env->acc;
        mamulb_ac(env);
    } break;
    case TILEOP_MAMULBMX:{
        mamulbmx(env);
    } break;
    case TILEOP_MAMULBMXAC:{
        mamulbmx_ac(env);
    } break;
    case TILEOP_MAMULBMXACC:{
        env->te = (uint64_t)env->acc;
        mamulbmx_ac(env);
    } break;
    case TILEOP_TMOV: {
        tmov(env);
    } break;
    case TILEOP_TCVT: {
        tcvt(env);
    } break;
    case TILEOP_MGATHER: {
        mem_gather(env);
    } break;
    case TILEOP_MSCATTER: {
        mem_scatter(env);
    } break;
    case TILEOP_ACCCVT: {
        acccvt(env);
    } break;
    case TILEOP_TLOAD: {
        tileop_memcopy(env, 1, 0);
    } break;
    case TILEOP_TSTORE: {
        tileop_memcopy(env, 0, 0);
    } break;
    case TILEOP_ESAVE: {
        tileop_esave(env);
    } break;
    case TILEOP_ERCOV: {
        tileop_ercov(env);
    } break;
    default:
        printf("pc:0x%lx, tileop:%d is not supported!\n",
                env->pc, env->tileop_info.tileop_type);
        assert(0);
    }
    return;
}

void helper_block_constraint(CPULINXState *env, uint32_t c_id)
{
    switch (c_id) {
    case CONSTRAINT_A6_4: {

    } break;
    default:
        assert(0);
    }
    return;
}
