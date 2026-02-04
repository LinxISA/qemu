/*
 * LinxISA helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "accel/tcg/cpu-ldst.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"

/* Semihosting operations via EBREAK immediate */
#define LINX_SEMIHOST_EXIT      0  /* Exit program */
#define LINX_SEMIHOST_PUTCHAR   1  /* a0 = character to output */
#define LINX_SEMIHOST_WRITE     2  /* a0 = fd, a1 = buf, a2 = len -> a0 = bytes written */
#define LINX_SEMIHOST_READ      3  /* a0 = fd, a1 = buf, a2 = len -> a0 = bytes read */

void HELPER(linx_ebreak)(CPULinxState *env, uint32_t imm)
{
    CPUState *cs = env_cpu(env);
    
    qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK imm=%d, a0=0x%lx, a1=0x%lx, a2=0x%lx\n",
                  imm, (unsigned long)env->gpr[LINX_REG_A0],
                  (unsigned long)env->gpr[LINX_REG_A1],
                  (unsigned long)env->gpr[LINX_REG_A2]);
    
    switch (imm) {
    case LINX_SEMIHOST_EXIT:
        /* Exit program - graceful shutdown */
        qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK EXIT at PC=0x%lx\n",
                      (unsigned long)env->pc);
        cs->exception_index = LINX_EXCP_BREAKPOINT;
        cpu_loop_exit_restore(cs, GETPC());
        break;
        
    case LINX_SEMIHOST_PUTCHAR: {
        /* Output single character from a0 */
        int ch = env->gpr[LINX_REG_A0] & 0xff;
        qemu_log_mask(CPU_LOG_INT, "Linx: PUTCHAR '%c' (0x%02x)\n", 
                      (ch >= 32 && ch < 127) ? ch : '.', ch);
        /* Write to stderr for immediate visibility */
        fputc(ch, stderr);
        fflush(stderr);
        env->gpr[LINX_REG_A0] = ch;  /* Return the character */
        return;  /* Continue execution */
    }
        
    case LINX_SEMIHOST_WRITE: {
        /* Write buffer: a0=fd (ignored, always stderr), a1=buf, a2=len */
        uint64_t buf_addr = env->gpr[LINX_REG_A1];
        uint64_t len = env->gpr[LINX_REG_A2];
        uint64_t i;
        
        qemu_log_mask(CPU_LOG_INT, "Linx: WRITE buf=0x%lx len=%lu\n",
                      (unsigned long)buf_addr, (unsigned long)len);
        
        /* Read and output each byte from guest memory */
        for (i = 0; i < len; i++) {
            uint8_t ch = cpu_ldub_data(env, buf_addr + i);
            fputc(ch, stderr);
        }
        fflush(stderr);
        env->gpr[LINX_REG_A0] = len;  /* Return bytes written */
        return;  /* Continue execution */
    }
        
    case LINX_SEMIHOST_READ: {
        /* Read not implemented for now - return 0 */
        env->gpr[LINX_REG_A0] = 0;
        return;
    }
        
    default:
        /* Unhandled semihosting operation - treat as breakpoint */
        qemu_log_mask(LOG_GUEST_ERROR, 
                      "Linx: Unhandled EBREAK imm=%d at PC=0x%lx\n",
                      imm, (unsigned long)env->pc);
        cs->exception_index = LINX_EXCP_BREAKPOINT;
        cpu_loop_exit_restore(cs, GETPC());
        break;
    }
}

void HELPER(raise_exception)(CPULinxState *env, uint32_t exception)
{
    CPUState *cs = env_cpu(env);
    cs->exception_index = exception;
    cpu_loop_exit_restore(cs, GETPC());
}


static unsigned linx_insn_len(uint16_t hw)
{
    if ((hw & 0x1) == 0) {
        return ((hw & 0xf) == 0xe) ? 6 : 2;
    }
    return ((hw & 0xf) == 0xf) ? 8 : 4;
}

static bool linx_is_bstart_at_addr(CPULinxState *env, uint64_t pc)
{
    CPUState *cs = env_cpu(env);
    uint8_t buf[8];

    if (cpu_memory_rw_debug(cs, pc, buf, 2, 0) != 0) {
        return false;
    }

    const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    const unsigned len = linx_insn_len(hw);

    if (len == 2) {
        /* C.BSTART.STD / C.BSTART.FP: mask=0xc7ff, BrType in bits [13:11] */
        if ((hw & 0xc7ff) == 0x0000 || (hw & 0xc7ff) == 0x0080) {
            const uint8_t brtype = (hw >> 11) & 0x7;
            if (brtype != 0) {
                return true;
            }
        }

        /* C.BSTART DIRECT/COND: distinguish by low nibble */
        if ((hw & 0x000f) == 0x0002 || (hw & 0x000f) == 0x0004) {
            return true;
        }

        /* Common fixed fall-through markers for non-STD block types. */
        switch (hw) {
        case 0x0840: /* C.BSTART.SYS FALL */
        case 0x08c0: /* C.BSTART.MPAR FALL */
        case 0x48c0: /* C.BSTART.MSEQ FALL */
        case 0x88c0: /* C.BSTART.VPAR FALL */
        case 0xc8c0: /* C.BSTART.VSEQ FALL */
            return true;
        default:
            return false;
        }
    }

    if (len == 4) {
        if (cpu_memory_rw_debug(cs, pc, buf, 4, 0) != 0) {
            return false;
        }
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

        /* BSTART.*: low byte 0x01, branch kind in bits [14:12] is non-zero. */
        if ((insn & 0xff) == 0x01 && ((insn >> 12) & 0x7) != 0) {
            return true;
        }

        /* Template blocks: FENTRY/FEXIT/FRET.* share opcode bits[6:0]=0x41. */
        if ((insn & 0x7f) == 0x41 && ((insn >> 12) & 0x7) <= 3) {
            return true;
        }

        return false;
    }

    if (len == 6) {
        if (cpu_memory_rw_debug(cs, pc, buf, 6, 0) != 0) {
            return false;
        }

        const uint16_t prefix = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        const uint32_t main32 = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8) |
                                ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24);
        if ((prefix & 0xf) != 0xe) {
            return false;
        }

        /* HL.BSTART.*: encoded as a 16-bit prefix + 32-bit BSTART main part. */
        if ((main32 & 0xff) == 0x01 && ((main32 >> 12) & 0x7) != 0) {
            return true;
        }
        return false;
    }

    return false;
}

void HELPER(linx_check_bstart_target)(CPULinxState *env, uint64_t target)
{
    /*
     * This helper is on the hot path for indirect control flow (RET/IND/ICALL
     * and template returns). Cache the most recently-validated targets to avoid
     * re-reading guest memory for tight call/return loops.
     *
     * Note: This cache is conservative for typical bare-metal workloads (code
     * is not self-modifying). If guest code changes, TB invalidation will
     * naturally trigger re-translation, but this cache may still accept a
     * previously-validated address until reset.
     */
    for (size_t i = 0; i < ARRAY_SIZE(env->bstart_cache); i++) {
        if (env->bstart_cache[i] == target) {
            return;
        }
    }

    if (linx_is_bstart_at_addr(env, target)) {
        env->bstart_cache[env->bstart_cache_next & (ARRAY_SIZE(env->bstart_cache) - 1)] = target;
        env->bstart_cache_next++;
        return;
    }

    CPUState *cs = env_cpu(env);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: invalid branch target 0x%" PRIx64 " (not a block start marker)\n",
                  target);
    cs->exception_index = LINX_EXCP_BAD_BRANCH_TARGET;
    cpu_loop_exit_restore(cs, GETPC());
}

/*
 * Immediate exit helper - called when guest requests exit via EBREAK imm=0.
 * This function ensures QEMU terminates immediately by:
 * 1. Requesting a graceful shutdown
 * 2. Calling cpu_loop_exit to break out of the execution loop
 */
void HELPER(linx_exit)(CPULinxState *env)
{
    CPUState *cs = env_cpu(env);
    
    qemu_log_mask(CPU_LOG_INT, "Linx: EXIT request at PC=0x%lx\n",
                  (unsigned long)env->pc);
    
    /* Request graceful shutdown of the VM */
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    
    /* Exit the CPU execution loop - this never returns */
    cpu_loop_exit(cs);
}
