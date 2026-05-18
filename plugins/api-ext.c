#include "qemu/compiler.h"
#include "qemu/osdep.h"
#include "qemu/plugin.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg.h"
#include "exec/exec-all.h"
#include "exec/ram_addr.h"
#include "cpu.h"

#include "qemu/qemu-plugin-ext.h"

#ifdef TARGET_LINX
#include "linx_block_def.h"
#include "cpu_bits.h"
#endif

int qemu_plugin_ext_vcpu_get_header_info(uint32_t *header_info)
{
#ifdef TARGET_LINX
    CPUState *cs = current_cpu;
    CPULINXState *env = cs->env_ptr;
    *header_info = env->header_info;
    return *header_info;
#else
    return 0;
#endif
}

plugin_tileop_info qemu_plugin_ext_vcpu_get_tileop_info(
    plugin_tileop_info *tileop_info)
{
#ifdef TARGET_LINX
    CPUState *cs = current_cpu;
    CPULINXState *env = cs->env_ptr;
    tileop_info->type = env->tileop_info.tileop_type;
    tileop_info->datatype_bstart = env->tileop_info.tileop_datatype;
    tileop_info->dsttile_size = env->tile_attr[0].size;
    tileop_info->tile_reg_src_num = env->tile_reg_src_num;
    tileop_info->tile_reg_dst_num = env->tile_reg_dst_num;
    tileop_info->m = env->csr_lb[0];
    tileop_info->n = env->csr_lb[1];
    tileop_info->k = env->csr_lb[2];
    tileop_info->ta = env->ta;
    tileop_info->tb = env->tb;
    tileop_info->tc = env->tc;
    tileop_info->acc = (uint64_t)env->acc;
    tileop_info->to = env->to;
    return *tileop_info;
#else
    return *tileop_info;
#endif
}

int qemu_plugin_ext_vcpu_memory_read_vaddr(uint32_t cpu_index, uint64_t addr,
                                           void *ptr, uint64_t len)
{
    CPUState *cs = qemu_get_cpu(cpu_index);
    return cpu_memory_rw_debug(cs, addr, ptr, len, false);
}

int qemu_plugin_ext_vcpu_get_simt_predm(uint32_t cpu_index, uint64_t *value)
{
#ifdef TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    *value = env->predm;
    return *value;
#else
    return 0;
#endif
}

int qemu_plugin_ext_vcpu_get_src_reg(uint32_t cpu_index, int src_enc,
                                     uint64_t *blk)
{
#ifdef TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    int idx = 0;
    uint32_t offset = src_enc & 0x3;
    if (src_enc >= 0 && src_enc < 24) {
        *blk = env->gpr[src_enc];
    } else if (src_enc >= 24 && src_enc < 28) {
        /* t register */
        idx = (env->t_idx - offset - 1 + T_REG_SIZE) % T_REG_SIZE;
        *blk = env->blk_t[idx];
    } else if (src_enc >= 28 && src_enc < 32) {
        /* u register */
        idx = (env->u_idx - offset - 1 + U_REG_SIZE) % U_REG_SIZE;
        *blk = env->blk_u[idx];
    }
    return *blk;
#else
    return 0;
#endif
}

int qemu_plugin_ext_vcpu_get_src_fvec_reg(uint32_t cpu_index,
                                          int src, uint64_t *blk, int lane_id)
{
#ifdef TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    src = extract16(src, 0, 7);
    uint32_t offset = src & FVEC_REG_IDX_MASK;
    if ((src >= SRC_FVEC_VT_1 && src <= SRC_FVEC_VT_4) ||
        (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4)) {
        *blk = env->fvec_t[lane_id][offset];
    } else if ((src >= SRC_FVEC_VU_1 && src <= SRC_FVEC_VU_4) ||
        (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4)) {
        *blk = env->fvec_u[lane_id][offset];
    } else if ((src >= SRC_FVEC_VM_1 && src <= SRC_FVEC_VM_4) ||
        (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4)) {
        *blk = env->fvec_m[lane_id][offset];
    } else if ((src >= SRC_FVEC_VN_1 && src <= SRC_FVEC_VN_4) ||
        (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4)) {
        *blk = env->fvec_n[lane_id][offset];
    } else if (src >= SRC_FVEC_RI0 && src <= SRC_FVEC_RI11) {
        uint64_t ri_idx = src - SRC_FVEC_RI0;
        *blk = env->gpr[env->blk_ri[ri_idx]];
    } else if (src >= SRC_FVEC_T_1 && src <= SRC_FVEC_U_4) {
        return qemu_plugin_ext_vcpu_get_src_reg(cpu_index,
                                                src & SRC_FVRC_REG_MASK, blk);
    } else {
        switch (src) {
        case SRC_FVEC_LC0:
            helper_update_lcreg(env, lane_id);
            *blk = env->csr_lc[0];
            break;
        case SRC_FVEC_LC1:
            helper_update_lcreg(env, lane_id);
            *blk = env->csr_lc[1];
            break;
        case SRC_FVEC_LC2:
            helper_update_lcreg(env, lane_id);
            *blk = env->csr_lc[2];
            break;
        case SRC_FVEC_LB0:
            *blk = env->csr_lb[0];
            break;
        case SRC_FVEC_LB1:
            *blk = env->csr_lb[1];
            break;
        case SRC_FVEC_LB2:
            *blk = env->csr_lb[2];
            break;
        case SRC_FVEC_TA:
            *blk = env->ta;
            break;
        case SRC_FVEC_TB:
            *blk = env->tb;
            break;
        case SRC_FVEC_TC:
            *blk = env->tc;
            break;
        case SRC_FVEC_TO:
            *blk = env->to;
            break;
        case SRC_FVEC_PRED:
            *blk = env->predm;
            break;
        case SRC_FVEC_ZERO:
            *blk = 0;
            break;
        default:
            break;
        }
    }

    return *blk;
#else
    return 0;
#endif
}

int qemu_plugin_ext_vcpu_get_src_fvec_ri_gpr_idx(uint32_t cpu_index, int src,
                                                 uint64_t *blk)
{
#ifdef TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    src = extract16(src, 0, 7);
    if (src >= SRC_FVEC_RI0 && src <= SRC_FVEC_RI11) {
        uint64_t ri_idx = src - SRC_FVEC_RI0;
        *blk = env->blk_ri[ri_idx];
    }
    return *blk;
#else
    return 0;
#endif
}

int qemu_plugin_ext_vcpu_get_dst_fvec_ro_gpr_idx(uint32_t cpu_index, int src,
                                                 uint64_t *blk)
{
#ifdef TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    src = extract16(src, 0, 7);
    if (src >= DST_FVEC_RO0 && src <= DST_FVEC_RO3) {
        uint64_t ro_idx = src - DST_FVEC_RO0;
        *blk = env->blk_ro[ro_idx];
    }
    return *blk;
#else
    return 0;
#endif
}

int qemu_plugin_ext_vcpu_get_dst_fvec_ro_reg(uint32_t cpu_index, int src,
                                             uint64_t *blk)
{
#ifdef TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    src = extract16(src, 0, 7);
    if (src >= DST_FVEC_RO0 && src <= DST_FVEC_RO3) {
        uint64_t ro_idx = src - DST_FVEC_RO0;
        *blk = env->gpr[env->blk_ro[ro_idx]];
    }
    return *blk;
#else
    return 0;
#endif
}

int qemu_plugin_ext_vcpu_get_src_fvec_sys_reg(uint32_t cpu_index, int src,
                                              uint64_t *blk)
{
#ifdef TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    src = src + SRC_FVEC_LC0;
    switch (src) {
        case SRC_FVEC_LC0:
            *blk = env->csr_lc[0];
            break;
        case SRC_FVEC_LC1:
            *blk = env->csr_lc[1];
            break;
        case SRC_FVEC_LC2:
            *blk = env->csr_lc[2];
            break;
        case SRC_FVEC_LB0:
            *blk = env->csr_lb[0];
            break;
        case SRC_FVEC_LB1:
            *blk = env->csr_lb[1];
            break;
        case SRC_FVEC_LB2:
            *blk = env->csr_lb[2];
            break;
        default:
            break;
    }
    return *blk;
#else
    return 0;
#endif
}

int qemu_plugin_ext_vcpu_read_gpr(uint32_t cpu_index, uint32_t gpr_idx,
                                  uint64_t *gpr_value)
{
    CPUState *cs = qemu_get_cpu(cpu_index);
#if defined TARGET_AARCH64
    CPUARMState *env = cs->env_ptr;
    // fixme: arm32 in aarch64 mode.
    if (gpr_idx < 32) {
        *gpr_value = env->xregs[gpr_idx];
        return 0;
    }
    return -1;
#elif defined TARGET_RISCV64 || defined TARGET_RISCV32
    CPURISCVState *env = cs->env_ptr;
    if (gpr_idx < 32) {
        *gpr_value = env->gpr[gpr_idx];
        return 0;
    }
    return -1;
#elif defined TARGET_LINX
    CPULINXState *env = cs->env_ptr;
    if (gpr_idx < 24) {
        *gpr_value = env->gpr[gpr_idx];
        return 0;
    }
    return -1;
#else
    // not supported
    return -1;
#endif
}

int qemu_plugin_ext_vcpu_read_sys_reg(uint32_t cpu_index, int reg_idx,
                                  uint64_t *reg_value)
{
#if defined TARGET_AARCH64
    reg_value = 0;
    return 0;
#endif
#if defined TARGET_RISCV64 || defined TARGET_RISCV32
    reg_value = 0;
    return 0;
#endif
#if defined TARGET_LINX
    CPUState *cs = qemu_get_cpu(cpu_index);
    CPULINXState *env = cs->env_ptr;
    linx_csrrw_debug(env,reg_idx,reg_value,0,0);
#endif
return 0;
}


int qemu_plugin_ext_vcpu_get_system_status(uint32_t cpu_index,
                                           enum system_status_idx sys_idx,
                                           uint64_t *sys_value)
{
#ifdef CONFIG_USER_ONLY
    *sys_value = 0;
    return 0;

#else
    CPUState *cs = qemu_get_cpu(cpu_index);
     uint32_t asid;
#if defined TARGET_AARCH64
    CPUARMState *env = cs->env_ptr;
    switch (sys_idx) {
    case QEMU_SYS_PRIVILEGE_LEVEL:
        *sys_value = arm_current_el(env);
        break;
    case QEMU_SYS_HW_CONTEXT_ID: {
        uint32_t asid = env->cp15.ttbr0_el[1] >> 48;
        uint32_t vmid = env->cp15.vttbr_el2 >> 48;
        *sys_value = asid | (vmid << 16);
        break;
    }
    default:
        return -1;
    }
    return 0;

#elif defined TARGET_RISCV64
    CPURISCVState *env = cs->env_ptr;
    switch (sys_idx) {
    case QEMU_SYS_PRIVILEGE_LEVEL:
        *sys_value = env->priv;
        break;
    case QEMU_SYS_HW_CONTEXT_ID:
        // only considered user level asid
        asid = (env->satp & SATP64_ASID) >> 44;
        *sys_value = asid;
        break;
    default:
        return -1;
    }
    return 0;
#elif defined TARGET_LINX
    CPULINXState *env = cs->env_ptr;
    switch (sys_idx) {
    case QEMU_SYS_PRIVILEGE_LEVEL:
        *sys_value = env->priv;
        break;
    case QEMU_SYS_HW_CONTEXT_ID:
        // only considered user level asid
        asid = (env->sysreg[ACR1].mmtbase & MMTBASE_ASID) >> 44;
        *sys_value = asid;
        break;
    default:
        return -1;
    }
    return 0;
#else
    return -1;
#endif // end of target arch
#endif // not CONFIG_USER_ONLY
}
