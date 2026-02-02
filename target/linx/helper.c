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
        /* Unknown semihosting operation - treat as breakpoint */
        qemu_log_mask(LOG_GUEST_ERROR, 
                      "Linx: Unknown EBREAK imm=%d at PC=0x%lx\n",
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
