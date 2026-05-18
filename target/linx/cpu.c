/*
 * QEMU LINX CPU
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
#include "qemu/qemu-print.h"
#include "qemu/timer.h"
#include "qemu/ctype.h"
#include "qemu/log.h"
#include "cpu.h"
#include "internals.h"
#include "exec/exec-all.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "fpu/softfloat-helpers.h"
#include "disas/disas.h"

const char * const linx_brhtype_names[BRANCH_TYPE_NUM] = {
    [BRANCH_FALL]           =       "fall through",
    [BRANCH_IND]            =       "indlink",
    [BRANCH_INDCALL]        =       "indcall",
    [BRANCH_RET]            =       "ret",
    [BRANCH_DIRECT_LINK]    =       "direct",
    [BRANCH_CONDITIONAL]    =       "conditional",
    [BRANCH_CALL]           =       "call",
};

const char * const linx_int_regnames[] = {
  "r0/zero",   "r1/sp",   "r2/a0",   "r3/a1",
  "r4/a2",   "r5/a3",   "r6/a4",   "r7/a5",
  "r8/a6",   "r9/a7",   "r10/ra",  "r11/s0",
  "r12/s1",  "r13/s2",  "r14/s3",  "r15/s4",
  "r16/s5",  "r17/s6",  "r18/s7",  "r19/s8",
  "r20/x0",  "r21/x1",  "r22/x2",  "r23/x3",
};

const char * const blk_ri_regnames[] = {
  "ri0", "ri1", "ri2", "ri3", "ri4", "ri5",
  "ri6", "ri7", "ri8", "ri9", "ri10", "ri11"
};

const char * const blk_ro_regnames[] = {
  "ro0", "ro1", "ro2", "ro3"
};

const char * const linx_int_local_regnames[] = {
  "lg-r0/zero",   "lg-r1/sp",    "lg-r2/a0",   "lg-r3/a1",
  "lg-r4/a2",   "lg-r5/a3",   "lg-r6/a4",   "lg-r7/a5",
  "lg-r8/a6",   "lg-r9/a7",   "lg-r10/ra",  "lg-r11/s0",
  "lg-r12/s1",  "lg-r13/s2",  "lg-r14/s3",  "lg-r15/s4",
  "lg-r16/s5",  "lg-r17/s6",  "lg-r18/s7",  "lg-r19/s8",
  "lg-r20/x0",  "lg-r21/x1",  "lg-r22/x2",  "lg-r23/x3",
};

const char * const linx_int_blk_tnames[] = {
  "TR1", "TR2",  "TR3",  "TR4"
};

const char * const linx_int_blk_unames[] = {
  "UR1",  "UR2",  "UR3", "UR4"
};

const char * const linx_lbnames[] = {
  "LB0",  "LB1",  "LB2",
};

const char * const linx_lcnames[] = {
  "LC0",  "LC1",  "LC2",
};

const char * const linx_fvec_tnames[CPU_NB_LANE_NUM][FVEC_REG_SIZE] = {
    {"lane0_TR1", "lane0_TR2", "lane0_TR3", "lane0_TR4"},
    {"lane1_TR1", "lane1_TR2", "lane1_TR3", "lane1_TR4"},
    {"lane2_TR1", "lane2_TR2", "lane2_TR3", "lane2_TR4"},
    {"lane3_TR1", "lane3_TR2", "lane3_TR3", "lane3_TR4"},
    {"lane4_TR1", "lane4_TR2", "lane4_TR3", "lane4_TR4"},
    {"lane5_TR1", "lane5_TR2", "lane5_TR3", "lane5_TR4"},
    {"lane6_TR1", "lane6_TR2", "lane6_TR3", "lane6_TR4"},
    {"lane7_TR1", "lane7_TR2", "lane7_TR3", "lane7_TR4"},
};

const char * const linx_fvec_unames[CPU_NB_LANE_NUM][FVEC_REG_SIZE] = {
    {"lane0_UR1", "lane0_UR2",  "lane0_UR3",  "lane0_UR4"},
    {"lane1_UR1", "lane1_UR2",  "lane1_UR3",  "lane1_UR4"},
    {"lane2_UR1", "lane2_UR2",  "lane2_UR3",  "lane2_UR4"},
    {"lane3_UR1", "lane3_UR2",  "lane3_UR3",  "lane3_UR4"},
    {"lane4_UR1", "lane4_UR2",  "lane4_UR3",  "lane4_UR4"},
    {"lane5_UR1", "lane5_UR2",  "lane5_UR3",  "lane5_UR4"},
    {"lane6_UR1", "lane6_UR2",  "lane6_UR3",  "lane6_UR4"},
    {"lane7_UR1", "lane7_UR2",  "lane7_UR3",  "lane7_UR4"},
};

const char * const linx_fvec_mnames[CPU_NB_LANE_NUM][FVEC_REG_SIZE] = {
    {"lane0_MR1", "lane0_MR2",  "lane0_MR3",  "lane0_MR4"},
    {"lane1_MR1", "lane1_MR2",  "lane1_MR3",  "lane1_MR4"},
    {"lane2_MR1", "lane2_MR2",  "lane2_MR3",  "lane2_MR4"},
    {"lane3_MR1", "lane3_MR2",  "lane3_MR3",  "lane3_MR4"},
    {"lane4_MR1", "lane4_MR2",  "lane4_MR3",  "lane4_MR4"},
    {"lane5_MR1", "lane5_MR2",  "lane5_MR3",  "lane5_MR4"},
    {"lane6_MR1", "lane6_MR2",  "lane6_MR3",  "lane6_MR4"},
    {"lane7_MR1", "lane7_MR2",  "lane7_MR3",  "lane7_MR4"},
};

const char * const linx_fvec_nnames[CPU_NB_LANE_NUM][FVEC_REG_SIZE] = {
    {"lane0_NR1", "lane0_NR2",  "lane0_NR3",  "lane0_NR4"},
    {"lane1_NR1", "lane1_NR2",  "lane1_NR3",  "lane1_NR4",},
    {"lane2_NR1", "lane2_NR2",  "lane2_NR3",  "lane2_NR4"},
    {"lane3_NR1", "lane3_NR2",  "lane3_NR3",  "lane3_NR4"},
    {"lane4_NR1", "lane4_NR2",  "lane4_NR3",  "lane4_NR4"},
    {"lane5_NR1", "lane5_NR2",  "lane5_NR3",  "lane5_NR4"},
    {"lane6_NR1", "lane6_NR2",  "lane6_NR3",  "lane6_NR4"},
    {"lane7_NR1", "lane7_NR2",  "lane7_NR3",  "lane7_NR4"},
};

const char * const sebstate_blk_reg[] = {
  "seTPC", "seTR1", "seTR2",  "seTR3",  "seTR4",  "seTR5",  "seTR6",  "seTR7", "seTR8",
  "seSR0", "seSR1", "seSR2", "seSR3", "seSR4", "seSR5", "seSR6", "seSR7", "seSR8", "seSR9", "seSR10",
  "seSR11", "seSR12", "seSR13", "seSR14", "seSR15", "seSR16", "seSR17", "seSR18", "seSR19","seSR20",
  "seSR21", "seSR22", "seSR23", "seSR24", "seSR25", "seSR26", "seSR27", "seSR28", "seSR29", "seSR30",
  "seSR31", "seCARG"
};

static const char * const linx_excp_names[] = {
    [LINX_EXCP_INSN_ACCESS] = "insn_access",
    [LINX_EXCP_INSN_TRANSLATION] = "insn_transfault",
    [LINX_EXCP_INSN_MISALIGNED] = "insn_misaligned",
    [LINX_EXCP_INSN_ILLEGAL] = "insn_illegal",
    [LINX_EXCP_INSN_PERMISSION] = "insn_permission",
    [LINX_EXCP_INSN_PAGEFAULT] = "insn_pagefault",

    [LINX_EXCP_DATA_LD_ACCESS] = "load_access",
    [LINX_EXCP_DATA_LD_MISALIGNED] = "load_misaligned",
    [LINX_EXCP_DATA_LD_PAGEFAULT] = "load_pagefault",
    [LINX_EXCP_DATA_ST_ACCESS] = "store_access",
    [LINX_EXCP_DATA_ST_MISALIGNED] = "store_misaligned",
    [LINX_EXCP_DATA_ST_PAGEFAULT] = "store_pagefault",

    [LINX_EXCP_SCALL] = "scall_trap",

    [LINX_EXCP_BLK_IVLD_SET] = "blk_invalid_set",
    [LINX_EXCP_BLK_IVLD_GET] = "blk_invalid_get",
    [LINX_EXCP_BLK_IVLD_PARM] = "blk_invalid_para",
    [LINX_EXCP_BLK_DUP_SET] = "blk_duplicated_set",
    [LINX_EXCP_BLK_IVLD_FIXUP] = "blk_invalid_fixup",

    [LINX_EXCP_ILLSSR] = "illssr",
};

static const char * const linx_intr_names[] = {
    "acr0_external",
    "acr0_timer",
    "acr0_software",
    "acr1_external",
    "acr1_timer",
    "acr1_software",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved"
};

const char *linx_cpu_get_trap_name(target_ulong cause, bool async)
{
    if (!async) {
        return (cause < ARRAY_SIZE(linx_intr_names)) ?
               linx_intr_names[cause] : "(unknown)";
    } else {
        return (linx_excp_names[cause] != NULL) ?
               linx_excp_names[cause] : "(unknown)";
    }
}

static void set_feature(CPULINXState *env, int feature)
{
    env->features |= (1ULL << feature);
}

static void set_resetvec(CPULINXState *env, target_ulong resetvec)
{
#ifndef CONFIG_USER_ONLY
    env->resetvec = resetvec;
#endif
}

static void linx_any_cpu_init(Object *obj)
{
    /* todo: init cpu configures: hardware version, extension and so on */
}


static ObjectClass *linx_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;
    char **cpuname;

    cpuname = g_strsplit(cpu_model, ",", 1);
    typename = g_strdup_printf(LINX_CPU_TYPE_NAME("%s"), cpuname[0]);
    oc = object_class_by_name(typename);
    g_strfreev(cpuname);
    g_free(typename);
    if (!oc || !object_class_dynamic_cast(oc, TYPE_LINX_CPU) ||
        object_class_is_abstract(oc)) {
        return NULL;
    }
    return oc;
}

static void linx_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    int i;

    qemu_fprintf(f, " %s " TARGET_FMT_lx "\n", "pc      ", env->pc);
#ifndef CONFIG_USER_ONLY
    {
        static const int dump_csrs[] = {
            CSTATE,
            LXLCID,
            TIME,
            A0_ECSTATE,
            A0_EVBASE,
            A0_ECAUSE,
            A0_EARG0,
            A0_ETEMP,
            A0_FUTO,
            A0_IENABLE,
            A0_IPENDING,
            A0_EOIEI,
            A0_EBPC,
            A0_EBARG,
            A0_ETPC,
            A0_MMTBASE,
            A0_MMCONFIG,
            A0_TIME,
            A0_TIMECMP,
            A1_ECSTATE,
            A1_EVBASE,
            A1_ECAUSE,
            A1_EARG0,
            A1_ETEMP,
            A1_FUTO,
            A1_IENABLE,
            A1_IPENDING,
            A1_EOIEI,
            A1_EBPC,
            A1_EBARG,
            A1_ETPC,
            A1_MMTBASE,
            A1_MMCONFIG,
            A1_TIME,
            A1_TIMECMP,
        };

        for (int i = 0; i < ARRAY_SIZE(dump_csrs); ++i) {
            int csrno = dump_csrs[i];
            target_ulong val = 0;
            LINXException res = linx_csrrw_debug(env, csrno, &val, 0, 0);

            /*
             * Rely on the smode, hmode, etc, predicates within csr.c
             * to do the filtering of the registers that are present.
             */
            if (res == LINX_EXCP_NONE) {
                qemu_fprintf(f, " %-11s " TARGET_FMT_lx,
                             csr_ops[csrno].name, val);
                if ((i & 3) == 3) {
                    qemu_fprintf(f, "\n");
                }
            }
        }
        qemu_fprintf(f, "\n");
    }
#endif

    for (i = 0; i < GPR_REG_SIZE; i++) {
        qemu_fprintf(f, " %-11s " TARGET_FMT_lx,
                     linx_int_regnames[i], env->gpr[i]);
        if ((i & 3) == 3) {
            qemu_fprintf(f, "\n");
        }
    }
    for (i = 0; i < 4; i++) {
        qemu_fprintf(f, " %-11s " TARGET_FMT_lx,
                    linx_int_blk_tnames[i], env->blk_t[i]);
    }
    qemu_fprintf(f, "\n");

    for (i = 0; i < 4; i++) {
        qemu_fprintf(f, " %-11s " TARGET_FMT_lx,
                    linx_int_blk_unames[i], env->blk_u[i]);
    }
    qemu_fprintf(f, "\n");
    qemu_fprintf(f, " %-11s " TARGET_FMT_lx, "PREDM", env->predm);
    qemu_fprintf(f, " %-11s " TARGET_FMT_lx, "BPC", env->bpc);
    qemu_fprintf(f, " %-11s " TARGET_FMT_lx, "CARG_FLAG", env->carg_flag);
    qemu_fprintf(f, " %-11s " TARGET_FMT_lx, "CARG_TGT", env->carg_tgt);
    qemu_fprintf(f, "\n");
    qemu_fprintf(f, " %-11s " TARGET_FMT_lx, "TP", env->csr_tp);
    qemu_fprintf(f, "\n");
    qemu_fprintf(f, " %-11s " TARGET_FMT_lx, "GP", env->csr_gp);
    qemu_fprintf(f, "\n");
}

static void linx_cpu_set_pc(CPUState *cs, vaddr value)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    env->pc = value;
}

static void linx_cpu_synchronize_from_tb(CPUState *cs,
                                          const TranslationBlock *tb)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    env->pc = tb->pc;
}

#ifndef CONFIG_USER_ONLY
static bool linx_is_irq_pending(CPULINXState *env)
{
    target_ulong pending = 0;
    /* TODO: Integrates all interrupt bits into the pending variable, not only ACR0, ACR1. */
    pending = (env->sysreg[ACR0].ipending & env->sysreg[ACR0].econfig) |
              ((env->sysreg[ACR1].ipending & env->sysreg[ACR1].econfig) <<
               PER_ACR_IRQ_NUM);
    return pending;
}
#endif

static bool linx_cpu_has_work(CPUState *cs)
{
#ifndef CONFIG_USER_ONLY
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    /*
     * Definition of the WFI instruction requires it to ignore the privilege
     * mode and delegation registers, but respect individual enables
     */
    return linx_is_irq_pending(env);

#else
    return true;
#endif
}

void restore_state_to_opc(CPULINXState *env, TranslationBlock *tb,
                          target_ulong *data)
{
    env->pc = data[0];
}

static void linx_cpu_reset(DeviceState *dev)
{
    CPUState *cs = CPU(dev);
    LINXCPU *cpu = LINX_CPU(cs);
    LINXCPUClass *mcc = LINX_CPU_GET_CLASS(cpu);
    CPULINXState *env = &cpu->env;
    static bool first_flag = true;

    mcc->parent_reset(dev);
#ifndef CONFIG_USER_ONLY
    env->priv = ACR0;
    env->pc = env->resetvec;
    memset(env->sysreg, 0, sizeof(env->sysreg));
    env->sysreg[ACR0].mmtbase = 0x0;
    env->sysreg[ACR1].mmtbase = 0x0;
    env->sysreg[ACR0].acr_param = set_field(env->sysreg[ACR0].acr_param,
        ACR_PARAM_EBS_SZ, EBSTATE_MAX_VALID_REG);
    env->sysreg[ACR1].acr_param = set_field(env->sysreg[ACR1].acr_param,
        ACR_PARAM_EBS_SZ, EBSTATE_MAX_VALID_REG);
#endif
    cs->exception_index = LINX_EXCP_NONE;
    env->linx_load_res = -1;

    env->cstk = g_queue_new();

    set_default_nan_mode(1, &env->fp_status);
    set_float_rounding_mode(float_round_nearest_even, &env->fp_status);

    if (first_flag) {
        first_flag = false;
        env->acc = malloc(TILE_REG_MEM);
        /* s register max space is 512Ki Bytes */
        env->s =   malloc(16 * TILE_REG_MEM);
        if (qemu_loglevel_mask(CPU_LOG_LINX_MEM)) {
            qemu_log("tileRegister ACC:%p\n", env->acc);
        }
        uint8_t *base = malloc(4 * TILE_REG_SIZE * TILE_REG_MEM);
        memset(base, 0, 4 * TILE_REG_SIZE * TILE_REG_MEM);
        env->tile_reg_t[0] = (uint64_t *)base;
        env->tile_reg_u[0] =
            (uint64_t *)(base + 1 * TILE_REG_SIZE * TILE_REG_MEM);
        env->tile_reg_m[0] =
            (uint64_t *)(base + 2 * TILE_REG_SIZE * TILE_REG_MEM);
        env->tile_reg_n[0] =
            (uint64_t *)(base + 3 * TILE_REG_SIZE * TILE_REG_MEM);
        for (int i = 0; i < TILE_REG_SIZE; i++) {
            env->tile_reg_t[i] =
                (uint64_t *)((uint8_t *)env->tile_reg_t[0] + i * TILE_REG_MEM);
            env->tile_reg_u[i] =
                (uint64_t *)((uint8_t *)env->tile_reg_u[0] + i * TILE_REG_MEM);
            env->tile_reg_m[i] =
                (uint64_t *)((uint8_t *)env->tile_reg_m[0] + i * TILE_REG_MEM);
            env->tile_reg_n[i] =
                (uint64_t *)((uint8_t *)env->tile_reg_n[0] + i * TILE_REG_MEM);
            if (qemu_loglevel_mask(CPU_LOG_LINX_MEM)) {
                qemu_log("T/U/M/N[%d]:%p, %p, %p, %p\n", i, env->tile_reg_t[i],
                env->tile_reg_u[i], env->tile_reg_m[i], env->tile_reg_n[i]);
            }
        }
    }

    linx_reset_bstate(env);
}

static void linx_cpu_disas_set_info(CPUState *s, disassemble_info *info)
{
    info->print_insn = print_insn_linx;
    info->private_data = s;
}

#ifndef CONFIG_USER_ONLY
static void linx_gt_timer_acr0_cb(void *opaque)
{
    LINXCPU *cpu = opaque;
    qemu_set_irq(cpu->gt_timer_output[GTIMER_ACR0], 1);
}

static void linx_gt_timer_acr1_cb(void *opaque)
{
    LINXCPU *cpu = opaque;
    qemu_set_irq(cpu->gt_timer_output[GTIMER_ACR1], 1);
}
#endif

static void linx_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    LINXCPU *cpu = LINX_CPU(dev);
    CPULINXState *env = &cpu->env;
    LINXCPUClass *mcc = LINX_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

#ifndef CONFIG_USER_ONLY
    env->gt_timer[GTIMER_ACR0] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                              &linx_gt_timer_acr0_cb, cpu);
    env->gt_timer[GTIMER_ACR1] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                              &linx_gt_timer_acr1_cb, cpu);
#endif

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    if (cpu->cfg.mmu) {
        set_feature(env, LINX_FEATURE_MMU);
    }

    set_resetvec(env, cpu->cfg.resetvec);

    linx_cpu_register_gdb_regs_for_features(cs);

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    mcc->parent_realize(dev, errp);
}

#ifndef CONFIG_USER_ONLY
static void linx_cpu_set_irq(void *opaque, int irq, int level)
{
    LINXCPU *cpu = LINX_CPU(opaque);

    switch (irq) {
    case ACR0_EI:
    case ACR0_SI:
    case ACR0_TI:
    case ACR1_EI:
    case ACR1_SI:
    case ACR1_TI:
        linx_cpu_update_ipending(cpu, 1 << (irq % PER_ACR_IRQ_NUM),
                                 BOOL_TO_MASK(level), irq / PER_ACR_IRQ_NUM);
        break;
    default:
        g_assert_not_reached();
    }
}
#endif /* CONFIG_USER_ONLY */

static void linx_cpu_init(Object *obj)
{
    LINXCPU *cpu = LINX_CPU(obj);

    cpu_set_cpustate_pointers(cpu);

#ifndef CONFIG_USER_ONLY
    qdev_init_gpio_in(DEVICE(cpu), linx_cpu_set_irq, 6);
    qdev_init_gpio_out(DEVICE(cpu), cpu->gt_timer_output, NUM_GTIMERS);
#endif /* CONFIG_USER_ONLY */
}

static Property linx_cpu_properties[] = {
    /* Defaults for standard extensions */
    DEFINE_PROP_BOOL("mmu", LINXCPU, cfg.mmu, true),

    DEFINE_PROP_UINT64("resetvec", LINXCPU, cfg.resetvec, DEFAULT_RSTVEC),
    DEFINE_PROP_END_OF_LIST(),
};

static gchar *linx_gdb_arch_name(CPUState *cs)
{
    /* todo: I think "linx:g" is better, but compiler team use this name for
     * now */
    return g_strdup("linx");
}

static const char *linx_gdb_get_dynamic_xml(CPUState *cs, const char *xmlname)
{
    LINXCPU *cpu = LINX_CPU(cs);

    if (strcmp(xmlname, "linx-csr.xml") == 0) {
        return cpu->dyn_csr_xml;
    }

    return NULL;
}

#ifndef CONFIG_USER_ONLY
#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps linx_sysemu_ops = {
    .get_phys_page_debug = linx_cpu_get_phys_page_debug,
    .write_elf64_note = linx_cpu_write_elf_note,
    .legacy_vmsd = &vmstate_linx_cpu,
};
#endif

#include "hw/core/tcg-cpu-ops.h"

static const struct TCGCPUOps linx_tcg_ops = {
    .initialize = linx_translate_init,
    .synchronize_from_tb = linx_cpu_synchronize_from_tb,

#ifndef CONFIG_USER_ONLY
    .tlb_fill = linx_cpu_tlb_fill,
    .cpu_exec_interrupt = linx_cpu_exec_interrupt,
    .do_interrupt = linx_cpu_do_interrupt,
    .do_transaction_failed = linx_cpu_do_transaction_failed,
    .do_unaligned_access = linx_cpu_do_unaligned_access,
#endif /* !CONFIG_USER_ONLY */
};

static void linx_cpu_class_init(ObjectClass *c, void *data)
{
    LINXCPUClass *mcc = LINX_CPU_CLASS(c);
    CPUClass *cc = CPU_CLASS(c);
    DeviceClass *dc = DEVICE_CLASS(c);

    device_class_set_parent_realize(dc, linx_cpu_realize,
                                    &mcc->parent_realize);

    device_class_set_parent_reset(dc, linx_cpu_reset, &mcc->parent_reset);

    cc->class_by_name = linx_cpu_class_by_name;
    cc->has_work = linx_cpu_has_work;
    cc->dump_state = linx_cpu_dump_state;
    cc->set_pc = linx_cpu_set_pc;
    cc->gdb_read_register = linx_cpu_gdb_read_register;
    cc->gdb_write_register = linx_cpu_gdb_write_register;
    cc->gdb_num_core_regs = 33;
    cc->gdb_core_xml_file = "linx-64bit-cpu.xml";
    cc->gdb_stop_before_watchpoint = true;
    cc->disas_set_info = linx_cpu_disas_set_info;
#ifndef CONFIG_USER_ONLY
    cc->sysemu_ops = &linx_sysemu_ops;
#endif
    cc->gdb_arch_name = linx_gdb_arch_name;
    cc->gdb_get_dynamic_xml = linx_gdb_get_dynamic_xml;
    cc->tcg_ops = &linx_tcg_ops;

    device_class_set_props(dc, linx_cpu_properties);
}

static gint linx_cpu_list_compare(gconstpointer a, gconstpointer b)
{
    ObjectClass *class_a = (ObjectClass *)a;
    ObjectClass *class_b = (ObjectClass *)b;
    const char *name_a, *name_b;

    name_a = object_class_get_name(class_a);
    name_b = object_class_get_name(class_b);
    return strcmp(name_a, name_b);
}

static void linx_cpu_list_entry(gpointer data, gpointer user_data)
{
    const char *typename = object_class_get_name(OBJECT_CLASS(data));
    int len = strlen(typename) - strlen(LINX_CPU_TYPE_SUFFIX);

    qemu_printf("%.*s\n", len, typename);
}

void linx_cpu_list(void)
{
    GSList *list;

    list = object_class_get_list(TYPE_LINX_CPU, false);
    list = g_slist_sort(list, linx_cpu_list_compare);
    g_slist_foreach(list, linx_cpu_list_entry, NULL);
    g_slist_free(list);
}

#define DEFINE_CPU(type_name, initfn)      \
    {                                      \
        .name = type_name,                 \
        .parent = TYPE_LINX_CPU,          \
        .instance_init = initfn            \
    }

static const TypeInfo linx_cpu_type_infos[] = {
    {
        .name = TYPE_LINX_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(LINXCPU),
        .instance_align = __alignof__(LINXCPU),
        .instance_init = linx_cpu_init,
        .abstract = true,
        .class_size = sizeof(LINXCPUClass),
        .class_init = linx_cpu_class_init,
    },
    DEFINE_CPU(TYPE_LINX_CPU_ANY,              linx_any_cpu_init),
};

DEFINE_TYPES(linx_cpu_type_infos)
