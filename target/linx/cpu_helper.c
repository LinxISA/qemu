/*
 * LINX CPU helpers for qemu.
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
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "tcg/tcg-op.h"
#include "trace.h"
#include "semihosting/common-semi.h"
#include <assert.h>

linx_exception_route exception_route_table[LINX_EXCP_TABLESIZE] = {
    [LINX_EXCP_INSN_ACCESS]          = {{ 0, 1, 1}},
    [LINX_EXCP_INSN_TRANSLATION]     = {{ 0, 1, 1}},
    [LINX_EXCP_INSN_MISALIGNED]      = {{ 0, 1, 1}},
    [LINX_EXCP_INSN_ILLEGAL]         = {{ 0, 1, 1}},
    [LINX_EXCP_INSN_PERMISSION]      = {{ 0, 1, 1}},
    [LINX_EXCP_INSN_PAGEFAULT]       = {{ 0, 1, 1}},
    [LINX_EXCP_DATA_LD_ACCESS]       = {{ 0, 1, 1}},
    [LINX_EXCP_DATA_LD_MISALIGNED]   = {{ 0, 1, 1}},
    [LINX_EXCP_DATA_LD_PAGEFAULT]    = {{ 0, 1, 1}},
    [LINX_EXCP_DATA_ST_ACCESS]       = {{ 0, 1, 1}},
    [LINX_EXCP_DATA_ST_MISALIGNED]   = {{ 0, 1, 1}},
    [LINX_EXCP_DATA_ST_PAGEFAULT]    = {{ 0, 1, 1}},
    [LINX_EXCP_BLK_IVLD_SET]         = {{ 0, 1, 1}},
    [LINX_EXCP_BLK_IVLD_GET]         = {{ 0, 1, 1}},
    [LINX_EXCP_BLK_IVLD_PARM]        = {{ 0, 1, 1}},
    [LINX_EXCP_BLK_DUP_SET]          = {{ 0, 1, 1}},
    [LINX_EXCP_BLK_IVLD_FIXUP]       = {{ 0, 1, 1}},
    [LINX_EXCP_BREAKPOINT]           = {{ 0, 1, 1}},
    [LINX_EXCP_ILLSSR]               = {{ 0, 1, 1}},
};

static inline int linx_get_inst_len_from_opcode(uint16_t opcode)
{
    if (extract64(opcode, 0, 1) == 0) {
        return extract16(opcode, 1, 3) == 0b111 ? 6 : 2;
    }
    return extract64(opcode, 0, 4) == 0b1111 ? 8 : 4;
}

int linx_cpu_mmu_index(CPULINXState *env, bool ifetch)
{
#ifdef CONFIG_USER_ONLY
    return 0;
#else
    return env->priv;
#endif
}

void cpu_get_tb_cpu_state(CPULINXState *env, target_ulong *pc,
                          target_ulong *cs_base, uint32_t *pflags)
{
    uint32_t flags = 0;

    *pc = env->pc;
    *cs_base = 0;

#ifndef CONFIG_USER_ONLY
    flags |= cpu_mmu_index(env, 0);
#endif

    flags = FIELD_DP32(flags, TB_FLAGS, TIDX_INDEX, env->t_idx & 0x3);
    flags = FIELD_DP32(flags, TB_FLAGS, UIDX_INDEX, env->u_idx & 0x3);

    *pflags = flags;
}

#ifndef CONFIG_USER_ONLY
static int linx_cpu_local_irq_pending(CPULINXState *env)
{
    target_ulong pending = 0;
    /* Integrates all interrupt bits into the pending variable, need to do. */
    pending = (env->sysreg[ACR0].ipending & env->sysreg[ACR0].econfig) |
              ((env->sysreg[ACR1].ipending & env->sysreg[ACR1].econfig) <<
               PER_ACR_IRQ_NUM);

    /* Lower-order interrupts have a higher priority.  */
    if (pending) {
        return ctz64(pending); /* since non-zero */
    } else {
        return LINX_EXCP_NONE; /* indicates no pending interrupt */
    }
}

bool linx_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        LINXCPU *cpu = LINX_CPU(cs);
        CPULINXState *env = &cpu->env;
        int interruptno = linx_cpu_local_irq_pending(env);
        bool enable_interrupt = get_field(env->cstate, CSTATE_IE);
        uint32_t cur_acr = env->cstate & CSTATE_ACR;
        /* TO DO : This problem does not occur when only acr0/1/2 is
         * configured and needs to be modified later.
         */
        if (!enable_interrupt || ((interruptno / PER_ACR_IRQ_NUM) > cur_acr)) {
            return true;
        }

        if (interruptno >= 0) {
            cs->exception_index = LINX_EXCP_INT_FLAG | interruptno;
            linx_cpu_do_interrupt(cs);
            return true;
        }
    }
    return false;
}

static void log_csr_ipending(CPULINXState *env, uint32_t value)
{
    /* TO DO */
}

uint32_t linx_cpu_update_ipending(LINXCPU *cpu, uint32_t mask,
                                   uint32_t value, uint32_t acr_num)
{
    CPULINXState *env = &cpu->env;
    CPUState *cs = CPU(cpu);
    uint32_t old = env->sysreg[acr_num].ipending;
    bool locked = false;

    if (!qemu_mutex_iothread_locked()) {
        locked = true;
        qemu_mutex_lock_iothread();
    }

    log_csr_ipending(env, value);

    env->sysreg[acr_num].ipending =
        (env->sysreg[acr_num].ipending & ~mask) | (value & mask);

    if (env->sysreg[acr_num].ipending) {
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }

    if (locked) {
        qemu_mutex_unlock_iothread();
    }

    return old;
}

void linx_cpu_set_rdtime_fn(CPULINXState *env, uint64_t (*fn)(uint32_t),
                             uint32_t arg)
{
    env->rdtime_fn = fn;
    env->rdtime_fn_arg = arg;
}

void linx_cpu_set_mode(CPULINXState *env, target_ulong newpriv)
{
    if (newpriv > ACR2) {
        g_assert_not_reached();
    }

    /* tlb_flush is unnecessary as mode is contained in mmu_idx */
    env->priv = newpriv;
    env->cstate = set_field(env->cstate, CSTATE_ACR, newpriv);

    /*
     * Clear the load reservation - otherwise a reservation placed in one
     * context/process can be used by another, resulting in an SC succeeding
     * incorrectly. Version 2.2 of the ISA specification explicitly requires
     * this behaviour, while later revisions say that the kernel "should" use
     * an SC instruction to force the yielding of a load reservation on a
     * preemptive context switch. As a result, do both.
     */
    env->linx_load_res = -1;
}



static struct {
    int vm;
    const char *name;
} vm_name[] = {
    { VM_0_VA36_VA39, "VM_0_VA36_VA39" },
    { VM_1_VA44_VA48, "VM_1_VA44_VA48" },
    { VM_2_VA52_VA57, "VM_2_VA52_VA57" }
};

static const char *get_vm_name(int vm)
{
    for (int i = 0; i < sizeof(vm_name) / sizeof(vm_name[0]); i++) {
        if (vm_name[i].vm == vm)
            return vm_name[i].name;
    }
    return "unknown pt format";
}

/* get_physical_address - get the physical address for this virtual address
 *
 * Do a page table walk to obtain the physical address corresponding to a
 * virtual address. Returns 0 if the translation was successful
 *
 * Adapted from Spike's mmu_t::translate and mmu_t::walk
 *
 * @env: CPULINXState
 * @physical: This will be set to the calculated physical address
 * @prot: The returned protection attributes
 * @addr: The virtual address to be translated
 * @fault_pte_addr: If not NULL, this will be set to fault pte address
 *                  when a error occurs on pte address translation.
 *                  This will already be shifted to match htval.
 * @access_type: The type of MMU access
 * @mmu_idx: Indicates current privilege level
 * @first_stage: Are we in first stage translation?
 *               Second stage is used for hypervisor guest translation
 * @two_stage: Are we going to perform two stage translation
 * @is_debug: Is this access from a debugger or the monitor?
 */
static int get_physical_address(CPULINXState *env, hwaddr *physical,
                                int *prot, target_ulong addr,
                                target_ulong *fault_pte_addr,
                                int access_type, int mmu_idx,
                                bool first_stage, bool two_stage,
                                bool is_debug)
{
    /* NOTE: the env->pc value visible here will not be
     * correct, but the value visible to the exception handler
     * (linx_cpu_do_interrupt) is correct
     */
    MemTxResult res;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    int priv = mmu_idx & TB_FLAGS_PRIV_MMU_MASK;

    /* if EN == 1, Address translation enable, default value is 0 */
    int en = get_field(env->sysreg[ACR1].mmconfig, MMCONFIG_EN);
    bool watch_addr =
        addr == 0x342 || addr == 0xcc || addr == 0x10000000 ||
        addr == 0x1803ff8 || addr == 0x1806510 || addr == 0x1806518 ||
        addr == 0x0001000000000000ULL || addr == 0x0001000000000004ULL ||
        addr == 0xffffffff81746350ULL || addr == 0xffffffff81746363ULL ||
        addr == 0xffffffff81810ad0ULL || addr == 0xffffffff81810af0ULL ||
        addr == 0xffffffff80a00278ULL || addr == 0xffffffff800126a2ULL;

    if (watch_addr) {
        fprintf(stderr,
                "LINX_WALK_START addr=%#" PRIx64 " priv=%d en=%d base=%#" PRIx64
                " mmtbase=%#" PRIx64 " mmconfig=%#" PRIx64 "\n",
                (uint64_t)addr, priv, en,
                (uint64_t)get_field(env->sysreg[ACR1].mmtbase, MMTBASE_TN0PB) << PGSHIFT,
                (uint64_t)env->sysreg[ACR1].mmtbase,
                (uint64_t)env->sysreg[ACR1].mmconfig);
    }

    if (priv == ACR0 || en == 0) {
        *physical = addr;
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        if (watch_addr) {
            fprintf(stderr,
                    "LINX_WALK_BYPASS addr=%#" PRIx64 " priv=%d en=%d\n",
                    (uint64_t)addr, priv, en);
        }
        return TRANSLATE_SUCCESS;
    }
    *prot = 0;
    hwaddr base;
    int levels, ptidxbits, ptesize, vm, widened, is_qtne;


    /* The current version only needs to support one stage */
    assert(first_stage == true);

    if (first_stage == true) {
        /* MMTBASE.TN0PB,14'd0 */
        base = (hwaddr)get_field(env->sysreg[ACR1].mmtbase, MMTBASE_TN0PB)
                                                                   << PGSHIFT;
        vm = get_field(env->sysreg[ACR1].mmconfig, MMCONFIG_M);
        is_qtne = get_field(env->sysreg[ACR1].mmconfig, MMCONFIG_Q);
        widened = 0;
    }
    hwaddr tmp_base = base;

    if (watch_addr) {
        fprintf(stderr,
                "LINX_WALK_START addr=%#" PRIx64 " priv=%d en=%d base=%#" PRIx64
                " mmtbase=%#" PRIx64 " mmconfig=%#" PRIx64 " vm=%d q=%d\n",
                (uint64_t)addr, priv, en, (uint64_t)base,
                (uint64_t)env->sysreg[ACR1].mmtbase,
                (uint64_t)env->sysreg[ACR1].mmconfig, vm, is_qtne);
    }

    switch (vm) {
    case VM_0_VA36_VA39:
      levels = 3; ptidxbits = 9 - is_qtne; ptesize = 8 + 8 * is_qtne; break;
    case VM_1_VA44_VA48:
      levels = 4; ptidxbits = 9 - is_qtne; ptesize = 8 + 8 * is_qtne; break;
    case VM_2_VA52_VA57:
      levels = 5; ptidxbits = 9 - is_qtne; ptesize = 8 + 8 * is_qtne; break;
    default:
      g_assert_not_reached();
    }

    CPUState *cs = env_cpu(env);
    int va_bits = PGSHIFT + levels * ptidxbits + widened;
    target_ulong mask, masked_msbs;

    if (TARGET_LONG_BITS > (va_bits - 1)) {
        mask = (1L << (TARGET_LONG_BITS - (va_bits - 1))) - 1;
    } else {
        mask = 0;
    }
    masked_msbs = (addr >> (va_bits - 1)) & mask;

    if (masked_msbs != 0 && masked_msbs != mask) {
        return TRANSLATE_FAIL;
    }

    int ptshift = (levels - 1) * ptidxbits;
    int i;

#if !TCG_OVERSIZED_GUEST
restart:
#endif
    for (i = 0; i < levels; i++, ptshift -= ptidxbits) {
        target_ulong idx;
        if (i == 0) {
            idx = (addr >> (PGSHIFT + ptshift)) &
                           ((1 << (ptidxbits + widened)) - 1);
        } else {
            idx = (addr >> (PGSHIFT + ptshift)) &
                           ((1 << ptidxbits) - 1);
        }

        /* check that physical address of PTE is legal */
        hwaddr pte_addr;
        pte_addr = base + idx * ptesize;

        target_ulong pte;
        pte = address_space_ldq(cs->as, pte_addr, attrs, &res);

        qemu_log_mask(CPU_LOG_MMU, "page table %s Q is %d "
                      "walk level %d: pte(%lx)=%lx, base: 0x%lx\n",
                      get_vm_name(vm), is_qtne ? 1 : 0,
                      i, pte_addr, pte, tmp_base);

        if (res != MEMTX_OK) {
            if (watch_addr) {
                fprintf(stderr,
                        "LINX_WALK_PTE_FAULT addr=%#" PRIx64 " level=%d pte_addr=%#" PRIx64 "\n",
                        (uint64_t)addr, i, (uint64_t)pte_addr);
            }
            return TRANSLATE_FAIL;
        }

        if (watch_addr) {
            fprintf(stderr,
                    "LINX_WALK_LEVEL addr=%#" PRIx64 " level=%d base=%#" PRIx64
                    " idx=%#" PRIx64 " pte_addr=%#" PRIx64 " pte=%#" PRIx64 "\n",
                    (uint64_t)addr, i, (uint64_t)base, (uint64_t)idx,
                    (uint64_t)pte_addr, (uint64_t)pte);
        }

        hwaddr ppn = pte >> PTE_PPN_SHIFT;

        if (!(pte & TNE_V)) {
            /* Invalid PTE */
            return TRANSLATE_FAIL;
        } else if (!(pte & (TNE_R | TNE_W | TNE_X))) {
            /* Inner PTE, continue walking */
            base = ppn << PGSHIFT;
        } else if ((pte & (TNE_R | TNE_W | TNE_X)) == TNE_W) {
            /* Reserved leaf PTE flags: TNE_W */
            return TRANSLATE_FAIL;
        } else if ((pte & (TNE_R | TNE_W | TNE_X)) == (TNE_W | TNE_X)) {
            /* Reserved leaf PTE flags: TNE_W + TNE_X */
            return TRANSLATE_FAIL;
        } else if ((pte & TNE_PV) && ((priv != ACR2) &&
                   (access_type == MMU_INST_FETCH))) {
            /* User PTE flags when not U mode and cstate.permit is not set,
               or the access type is an instruction fetch */
            return TRANSLATE_FAIL;
        } else if (!(pte & TNE_PV) && (priv != ACR1)) {
            /* Supervisor PTE flags when not S mode */
            return TRANSLATE_FAIL;
        } else if (ppn & ((1ULL << ptshift) - 1)) {
            /* Misaligned PPN */
            return TRANSLATE_FAIL;
        } else if (access_type == MMU_DATA_LOAD && !((pte & TNE_R))) {
            /* Read access check failed */
            /* FIXME: */
            return TRANSLATE_FAIL;
        } else if (access_type == MMU_DATA_STORE && !(pte & TNE_W)) {
            /* Write access check failed */
            return TRANSLATE_FAIL;
        } else if (access_type == MMU_INST_FETCH && !(pte & TNE_X)) {
            /* Fetch access check failed */
            return TRANSLATE_FAIL;
        } else {
            /* if necessary, set accessed and dirty bits. */
            target_ulong updated_pte = pte | TNE_A |
                (access_type == MMU_DATA_STORE ? TNE_D : 0);

            /* Page table updates need to be atomic with MTTCG enabled */
            if (updated_pte != pte) {
                /*
                 * - if accessed or dirty bits need updating, and the PTE is
                 *   in RAM, then we do so atomically with a compare and swap.
                 * - if the PTE is in IO space or ROM, then it can't be updated
                 *   and we return TRANSLATE_FAIL.
                 * - if the PTE changed by the time we went to update it, then
                 *   it is no longer valid and we must re-walk the page table.
                 */
                MemoryRegion *mr;
                hwaddr l = sizeof(target_ulong), addr1;
                mr = address_space_translate(cs->as, pte_addr,
                    &addr1, &l, false, MEMTXATTRS_UNSPECIFIED);
                if (memory_region_is_ram(mr)) {
                    target_ulong *pte_pa =
                        qemu_map_ram_ptr(mr->ram_block, addr1);
#if TCG_OVERSIZED_GUEST
                    /* MTTCG is not enabled on oversized TCG guests so
                     * page table updates do not need to be atomic */
                    *pte_pa = pte = updated_pte;
#else
                    target_ulong old_pte =
                        qatomic_cmpxchg(pte_pa, pte, updated_pte);
                    if (old_pte != pte) {
                        goto restart;
                    } else {
                        pte = updated_pte;
                    }
#endif
                } else {
                    /* misconfigured PTE in ROM (AD bits are not preset) or
                     * PTE is in IO space and can't be updated atomically */
                    return TRANSLATE_FAIL;
                }
            }

            target_ulong offset;
            if (i != (levels - 1)) {
                int superleaf_level = (levels - 1) - i;
                offset = addr & ((1ull <<
                                (PGSHIFT + superleaf_level * ptidxbits)) - 1);
            } else {
                offset = addr & 0x0000000000000FFFULL;
            }

            *physical = (ppn << PGSHIFT) + offset;
            /* set permissions on the TLB entry */
            if (pte & TNE_R) {
                *prot |= PAGE_READ;
            }
            if ((pte & TNE_X)) {
                *prot |= PAGE_EXEC;
            }
            /* add write permission on stores or if the page is already dirty,
               so that we TLB miss on later writes to update the dirty bit */
            if ((pte & TNE_W) &&
                    (access_type == MMU_DATA_STORE || (pte & TNE_D))) {
                *prot |= PAGE_WRITE;
            }
            if (addr == 0x0001000000000000ULL || addr == 0x0001000000000004ULL) {
                fprintf(stderr,
                        "LINX_WALK_SUCCESS addr=%#" PRIx64 " phys=%#" PRIx64
                        " prot=%d pte=%#" PRIx64 " level=%d\n",
                        (uint64_t)addr, (uint64_t)*physical, *prot,
                        (uint64_t)pte, i);
            }
            return TRANSLATE_SUCCESS;
        }
    }
    return TRANSLATE_FAIL;
}

static void raise_mmu_exception(CPULINXState *env, target_ulong address,
                                MMUAccessType access_type, bool pmp_violation,
                                bool first_stage, bool two_stage)
{
    CPUState *cs = env_cpu(env);
    int page_fault_exceptions, vm;

    if (first_stage) {
        vm = get_field(env->sysreg[ACR1].mmconfig, MMCONFIG_EN);
    }

    page_fault_exceptions = vm != VMMA_OFF && !pmp_violation;

    switch (access_type) {
    case MMU_INST_FETCH:
        cs->exception_index = page_fault_exceptions ?
            LINX_EXCP_INSN_PAGEFAULT : LINX_EXCP_INSN_ACCESS;
        break;
    case MMU_DATA_LOAD:
        cs->exception_index = page_fault_exceptions ?
            LINX_EXCP_DATA_LD_PAGEFAULT : LINX_EXCP_DATA_LD_ACCESS;
        break;
    case MMU_DATA_STORE:
        cs->exception_index = page_fault_exceptions ?
            LINX_EXCP_DATA_ST_PAGEFAULT : LINX_EXCP_DATA_ST_ACCESS;
        break;
    default:
        g_assert_not_reached();
    }
    env->badaddr = address;
}

hwaddr linx_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    hwaddr phys_addr;
    int prot;
    int mmu_idx = cpu_mmu_index(&cpu->env, true);

    if (get_physical_address(env, &phys_addr, &prot, addr, NULL, 0, mmu_idx,
                             true, false, true)) {
        return -1;
    }

    return phys_addr & TARGET_PAGE_MASK;
}

void linx_cpu_do_transaction_failed(CPUState *cs, hwaddr physaddr,
                                     vaddr addr, unsigned size,
                                     MMUAccessType access_type,
                                     int mmu_idx, MemTxAttrs attrs,
                                     MemTxResult response, uintptr_t retaddr)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;

    if (access_type == MMU_DATA_STORE) {
        cs->exception_index = LINX_EXCP_DATA_ST_ACCESS;
    } else if (access_type == MMU_DATA_LOAD) {
        cs->exception_index = LINX_EXCP_DATA_LD_ACCESS;
    } else {
        cs->exception_index = LINX_EXCP_INSN_ACCESS;
    }

    env->badaddr = addr;
    linx_raise_exception(&cpu->env, cs->exception_index, retaddr);
}

void linx_cpu_do_unaligned_access(CPUState *cs, vaddr addr,
                                   MMUAccessType access_type, int mmu_idx,
                                   uintptr_t retaddr)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    switch (access_type) {
    case MMU_INST_FETCH:
        cs->exception_index = LINX_EXCP_INSN_MISALIGNED;
        break;
    case MMU_DATA_LOAD:
        cs->exception_index = LINX_EXCP_DATA_LD_MISALIGNED;
        break;
    case MMU_DATA_STORE:
        cs->exception_index = LINX_EXCP_DATA_ST_MISALIGNED;
        break;
    default:
        g_assert_not_reached();
    }
    env->badaddr = addr;
    linx_raise_exception(env, cs->exception_index, retaddr);
}

bool linx_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr)
{
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    hwaddr pa = 0;
    int prot;
    bool first_stage_error = true;
    int ret = TRANSLATE_FAIL;

    /* default TLB page size */
    target_ulong tlb_size = TARGET_PAGE_SIZE;


    qemu_log_mask(CPU_LOG_MMU, "%s ad %" VADDR_PRIx " rw %d mmu_idx %d\n",
                  __func__, address, access_type, mmu_idx);

    /* Single stage lookup */
    ret = get_physical_address(env, &pa, &prot, address, NULL,
                                access_type, mmu_idx, true, false, false);

    qemu_log_mask(CPU_LOG_MMU,
                    "%s address=%" VADDR_PRIx " ret %d physical "
                    TARGET_FMT_plx " prot %d\n",
                    __func__, address, ret, pa, prot);

    if (ret == TRANSLATE_SUCCESS) {
        tlb_set_page(cs, address & ~(tlb_size - 1), pa & ~(tlb_size - 1),
                     prot, mmu_idx, tlb_size);
        return true;
    } else if (probe) {
        return false;
    } else {
        raise_mmu_exception(env, address, access_type, false,
                            first_stage_error,
                            false);
        linx_raise_exception(env, cs->exception_index, retaddr);
    }

    return true;
}
#endif /* !CONFIG_USER_ONLY */

#if !defined(CONFIG_USER_ONLY)
static bool linx_should_do_fixup(CPULINXState *env, target_ulong cause,
                                 bool async)
{
    uint32_t fattr = env->header_info & HEADER_INFO_FIXUP_MASK;

    if (!async || !fattr)
        return false;

    uint64_t futo = env->sysreg[env->priv].futo;

    switch (cause) {
    case LINX_EXCP_DATA_LD_ACCESS:
        if (futo & EC_LOAD_ACCESS) {
            return false;
        }
        return true;
    case LINX_EXCP_DATA_LD_MISALIGNED:
        if (futo & EC_MISALIGNED) {
            return false;
        }
        return true;
    case LINX_EXCP_DATA_ST_ACCESS:
        if (futo & EC_STORE_A_ACCESS) {
            return false;
        }
        return true;
    case LINX_EXCP_DATA_ST_MISALIGNED:
        if (futo & EC_STORE_A_MISALIGNED) {
            return false;
        }
        return true;
    case LINX_EXCP_ASSERT:
        return true;
    default:
        /*
         * Exceptions that do not belong to this part should not be processed
         * through the fixup process.
         */
        return false;
    }

    return true;
}
#endif /* !CONFIG_USER_ONLY */

#if !defined(CONFIG_USER_ONLY)
static target_ulong get_handle_acr(target_ulong current_acr,
                                    target_ulong cause, int priv, int scall_arg)
{
    /* TODO: After the static route for exception handling is added,
     * we need to add the static route.
     */
    if (cause == LINX_EXCP_SCALL) {
        if (scall_arg == SCT_SYS && priv == ACR2) {
            return ACR1;
        } else {
            return ACR0;
        }
    }
    return exception_route_table[cause].priv[current_acr];
}

static target_ulong get_irq_target_acr(target_ulong current_acr,
                                       target_ulong cause)
{
    target_ulong irq_num = cause & IRQ_MASK;
    target_ulong target_acr = irq_num / PER_ACR_IRQ_NUM;
    /* TO DO : This problem does not occur when only acr0/1/2 is
     * configured and needs to be modified later.
     */
    return target_acr;
}
#endif /* !CONFIG_USER_ONLY */

/*
 * Handle Traps
 *
 * Adapted from Spike's processor_t::take_trap.
 *
 */
void linx_cpu_do_interrupt(CPUState *cs)
{
#if !defined(CONFIG_USER_ONLY)

    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    bool save_state = false;
    /*
     * cs->exception is 32-bits wide, the MSB is used to distinguish between
     * interrupts and exceptions and the remain bits indicate trapno.
     */
    bool sync = !!!(cs->exception_index & LINX_EXCP_INT_FLAG);
    target_ulong cause = cs->exception_index & LINX_EXCP_INT_MASK;
    target_ulong earg0 = 0;
    target_ulong handle_acr = 0;
    target_ulong trapnum = 0;
    target_ulong syndrome = 0;

    if (sync) {
        /* set earg0 to badaddr for traps with address information */
        switch (cause) {
        case LINX_EXCP_INSN_ACCESS:
        case LINX_EXCP_INSN_MISALIGNED:
        case LINX_EXCP_INSN_PAGEFAULT:
        case LINX_EXCP_DATA_LD_ACCESS:
        case LINX_EXCP_DATA_LD_MISALIGNED:
        case LINX_EXCP_DATA_LD_PAGEFAULT:
        case LINX_EXCP_DATA_ST_ACCESS:
        case LINX_EXCP_DATA_ST_MISALIGNED:
        case LINX_EXCP_DATA_ST_PAGEFAULT:
            earg0 = env->badaddr;
            break;
        case LINX_EXCP_INSN_ILLEGAL:
            earg0 = env->bins;
            break;
        case LINX_EXCP_ILLSSR:
            earg0 = env->badssr;
            break;
        case LINX_EXCP_INSN_TRANSLATION:
        case LINX_EXCP_INSN_PERMISSION:
        case LINX_EXCP_BLK_IVLD_SET:
        case LINX_EXCP_BLK_IVLD_GET:
        case LINX_EXCP_BLK_IVLD_PARM:
        case LINX_EXCP_BLK_DUP_SET:
        case LINX_EXCP_BLK_IVLD_FIXUP:
            break;
        default:
            break;
        }
    }

    /* FIXME: the fixup needs to be redesigned later */
    if (linx_should_do_fixup(env, cause, sync)) {
        if (g_getenv("LINX_LOG_PC_REWRITE") &&
            env->pc >= 0x10000 && (env->bpc + env->bnext) < 0x10000) {
            fprintf(stderr,
                    "LINX_PC_REWRITE cpu_fixup old=%#" PRIx64
                    " new=%#" PRIx64 " bpc=%#" PRIx64 " bnext=%#" PRIx64
                    " cause=%#" PRIx64 "\n",
                    (uint64_t)env->pc, (uint64_t)(env->bpc + env->bnext),
                    (uint64_t)env->bpc, (uint64_t)env->bnext,
                    (uint64_t)cause);
        }
        env->pc = env->bpc + env->bnext;
        linx_reset_bstate(env);
        goto ret_label;
    }

    trace_linx_trap(env->lxlcid, sync, cause, env->pc, earg0,
                     linx_cpu_get_trap_name(cause, sync));

    if (sync) {
        trapnum = get_field(cause, LINX_EXCP_TRAPNUM);
        syndrome = get_field(cause, LINX_EXCP_SYNDROME);
    } else {
        trapnum = get_field(cause, IRQ_MASK);
        syndrome = 0;
    }

    qemu_log_mask(CPU_LOG_INT,
                  "%s: lxlcid:"TARGET_FMT_ld", sync:%d, trapnum:"TARGET_FMT_lx
                  ", syndrome:"TARGET_FMT_lx", epc:0x"TARGET_FMT_lx
                  ", earg0:0x"TARGET_FMT_lx", desc=%s\n",
                  __func__, env->lxlcid, sync, trapnum, syndrome, env->pc,
                  earg0, linx_cpu_get_trap_name(cause, sync));

    if (sync && trapnum == 17 && env->priv == ACR1) {
        uint32_t insn32 = cpu_ldl_code(env, env->pc);
        target_ulong next_pc = env->pc +
            linx_get_inst_len_from_opcode(cpu_lduw_code(env, env->pc));
        uint16_t next16 = cpu_lduw_code(env, next_pc);

        if (insn32 == 0x0010102bU) {
            if (next16 == 0x0000) {
                next_pc += 2;
            }
            env->pc = next_pc;
            env->bpc = next_pc;
            env->tpc = next_pc;
            env->next_bpc = next_pc;
            env->carg_tgt = next_pc;
            env->tpc1 = LINX_ILLEGAL_INSTR_ADDR;
            env->header_info = 0;
            env->in_body = false;
            linx_reset_bstate(env);
            goto ret_label;
        }
    }

    if (env->pc == 0xffffffff809270f2ULL ||
        env->pc == 0x809270f2ULL) {
        fprintf(stderr,
                "LINX_PF270F2 s8=%#" PRIx64 " s2=%#" PRIx64
                " a0=%#" PRIx64 " a1=%#" PRIx64
                " a2=%#" PRIx64 " a3=%#" PRIx64 " hdr=%#" PRIx64
                " pred=%#" PRIx64 " bpc=%#" PRIx64 " next=%#" PRIx64
                " carg=%#" PRIx64 "\n",
                (uint64_t)env->gpr[19], (uint64_t)env->gpr[13],
                (uint64_t)env->gpr[xA0], (uint64_t)env->gpr[xA1],
                (uint64_t)env->gpr[xA2], (uint64_t)env->gpr[xA3],
                (uint64_t)env->header_info, (uint64_t)env->predm,
                (uint64_t)env->bpc, (uint64_t)env->next_bpc,
                (uint64_t)env->carg_tgt);
    }

    CPULINXState old_env = *env;

    /* handle the trap in handle_acr */
    if (sync) {
        handle_acr = get_handle_acr(env->cstate & CSTATE_ACR,
                                    cause, env->priv, env->scall_arg);
    } else {
        handle_acr = get_irq_target_acr(env->cstate & CSTATE_ACR, cause);
    }
    LinxSYSReg *sysreg = &env->sysreg[handle_acr];
    assert(handle_acr == ACR0 || handle_acr == ACR1);

    /* save CSTATE to ECSTATE_A<r> */
    sysreg->ecstate = env->cstate & ECSTATE_MASK;

    /* disable interrupt */
    env->cstate = set_field(env->cstate, CSTATE_IE, 0);
    /* clear cstate.vld */
    sysreg->ecstate =
        set_field(sysreg->ecstate, ECSTATE_BI, env->in_body);
    env->cstate = set_field(env->cstate, CSTATE_ACR, handle_acr);

    uint64_t hecause = 0;
    hecause = set_field(hecause, ECAUSE_E, sync);
    hecause = set_field(hecause, ECAUSE_TRAPNUM, trapnum);
    hecause = set_field(hecause, ECAUSE_SYNDROME, syndrome);
    sysreg->ecause = hecause;
    sysreg->earg0 = earg0;

    /*
     * The privilege level is set before the ebstate is written. In this way,
     * the user-mode mmu is not used to cause page faults when the ebstate is
     * written.
     */
    linx_cpu_set_mode(env, handle_acr);

    /*
     * An interrupt can land while a prior EBSTATE restore has already made the
     * recovered block body live again (`tpc` valid) but before the block head
     * has been reopened (`bpc` still invalid). In that window the architectural
     * block state is the recovered EBSTATE, not the transient exception-vector
     * PC. Overwriting ebpc/ebpcn/etpc here loses the original block target and
     * later resumes into garbage or zero.
     */
    bool recovering_bstate = env->in_body &&
                             !is_valid_linx_addr(env->bpc) &&
                             is_valid_linx_addr(env->tpc);
    if (!recovering_bstate) {
        if (is_valid_linx_addr(env->bpc)) {
            sysreg->ebpc = env->bpc;
        } else {
            sysreg->ebpc = env->pc;
        }
        if (trapnum == 17) {
            sysreg->ebpcn = env->pc +
                linx_get_inst_len_from_opcode(cpu_lduw_code(env, env->pc));
        } else if (get_brhtype(env->header_info) == BRANCH_FALL) {
            sysreg->ebpcn = env->next_bpc;
        } else {
            sysreg->ebpcn = env->carg_tgt;
        }
        sysreg->etpc = env->pc;
    }

    if (g_getenv("LINX_LOG_BREAK_FLOW") &&
        (env->pc == 0xa12d68 || env->pc == 0xffffffff80a12d68ULL ||
         trapnum == 17)) {
        fprintf(stderr,
                "LINX_BREAK_FLOW trap pc=%#" PRIx64 " trapnum=%#" PRIx64
                " hdr=%#" PRIx64 " br=%u in_body=%d recovering=%d"
                " bpc=%#" PRIx64 " next=%#" PRIx64 " carg=%#" PRIx64
                " save_ebpc=%#" PRIx64 " save_ebpcn=%#" PRIx64
                " save_etpc=%#" PRIx64 " save_ebarg=%#" PRIx64 "\n",
                (uint64_t)env->pc, (uint64_t)trapnum,
                (uint64_t)env->header_info, get_brhtype(env->header_info),
                env->in_body, recovering_bstate,
                (uint64_t)env->bpc, (uint64_t)env->next_bpc,
                (uint64_t)env->carg_tgt, (uint64_t)sysreg->ebpc,
                (uint64_t)sysreg->ebpcn, (uint64_t)sysreg->etpc,
                (uint64_t)sysreg->ebarg);
    }

    /*
     * Are we in the middle of block?
     * if in_body is false, we are in layer1, and we don't need to save bstate.
     * if in_body is valid, we are in layer2 which is in block.
     */
    if (env->in_body) {
        /*
         * Some early boot / direct-kernel entry paths can take an exception
         * before bpc is initialized. The exception frame already falls back
         * to env->pc in that case, so keep bstate save/recovery consistent.
         */
        if (!is_valid_linx_addr(env->bpc)) {
            env->bpc = env->pc;
        }
        /* save bstate to ebstate */
        if (!linx_is_atomic_blk(get_blk_atomic(env->header_info))) {
            if (recovering_bstate) {
                save_state = true;
            } else {
                linx_save_bstate(env, handle_acr);
                save_state = true;
            }
        }
    }
    /* normal exceptions handling process. */
    if (g_getenv("LINX_LOG_PC_REWRITE") &&
        env->pc >= 0x10000 && sysreg->evbase < 0x10000) {
        fprintf(stderr,
                "LINX_PC_REWRITE cpu_exception old=%#" PRIx64
                " new=%#" PRIx64 " cause=%#" PRIx64 " sync=%d\n",
                (uint64_t)env->pc, (uint64_t)sysreg->evbase,
                (uint64_t)cause, sync);
    }
    env->pc = sysreg->evbase;

    /* Log state before switch out to interrupt handler */
    if (!(qemu_loglevel_mask(CPU_LOG_CS_NO_M) &&
        (old_env.priv == PRV_M || env->priv == PRV_M))) {
        qemu_log_mask(CPU_LOG_CS, "------------- CS_OUT%s(" TARGET_FMT_ld "):"
                      " Addr(%lx=>%lx) with bpc=%lx(%lx), Priv(%ld=>%ld) for "
                      "`%s`\n",
                      save_state ? "_FROM_BLK" : "", old_env.lxlcid,
                      old_env.pc, env->pc, old_env.bpc, old_env.tpc1,
                      old_env.priv, env->priv,
                      linx_cpu_get_trap_name(cause, sync));
        linx_cs_log(env);
    }
    linx_reset_bstate(env);

    /* NOTE: it is not necessary to yield load reservations here. It is only
     * necessary for an SC from "another hart" to cause a load reservation
     * to be yielded. Refer to the memory consistency model section of the
     * LINX ISA Specification.
     */
ret_label:
#endif
    cs->exception_index = LINX_EXCP_NONE; /* mark handled to qemu */
}
