#ifndef LINX_TARGET_CPU_H
#define LINX_TARGET_CPU_H

static inline void cpu_clone_regs_child(CPULINXState *env, target_ulong newsp,
                                        unsigned flags)
{
    if (newsp) {
        env->gpr[xSP] = newsp;
    }

    env->gpr[xA0] = 0;
}

static inline void cpu_clone_regs_parent(CPULINXState *env, unsigned flags)
{
}

static inline void cpu_set_tls(CPULINXState *env, target_ulong newtls)
{
    env->csr_tp = newtls;
}

static inline abi_ulong get_sp_from_cpustate(CPULINXState *state)
{
   return state->gpr[xSP];
}
#endif
