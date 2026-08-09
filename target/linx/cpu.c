/*
 * QEMU LinxISA CPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "qapi/error.h"
#include "cpu.h"
#include "trace.h"
#include "migration/vmstate.h"
#include "exec/cputlb.h"
#include "exec/memattrs.h"
#include "exec/page-protection.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "exec/watchpoint.h"
#include "exec/log.h"
#include "fpu/softfloat-helpers.h"
#include "tcg/debug-assert.h"
#include "accel/accel-cpu-ops.h"
#include "accel/tcg/cpu-ops.h"
#include "system/runstate.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/core/qdev-properties.h"

static void linx_cpu_dump_state(CPUState *cs, FILE *f, int flags);

static bool linx_cpu_dump_debug_inited;
static bool linx_cpu_dump_debug_enabled;
static bool linx_cpu_dump_on_event_inited;
static bool linx_cpu_dump_on_event_enabled;
static bool linx_fault_trace_inited;
static bool linx_fault_trace_enabled;
static bool linx_fault_trace_filter_enabled;
static uint64_t linx_fault_trace_filter_lo;
static uint64_t linx_fault_trace_filter_hi;
static bool linx_fault_trace_addr_filter_enabled;
static uint64_t linx_fault_trace_addr_lo;
static uint64_t linx_fault_trace_addr_hi = UINT64_MAX;
static bool linx_fault_trace_count_filter_enabled;
static uint64_t linx_fault_trace_count_lo;
static uint64_t linx_fault_trace_count_hi;
static bool linx_fault_trace_trapnum_filter_enabled;
static uint64_t linx_fault_trace_trapnum;
static uint64_t linx_fault_trace_limit;
static uint64_t linx_fault_trace_emitted;
static bool linx_fault_trace_regs_enabled;
static bool linx_tlb_fault_trace_inited;
static bool linx_tlb_fault_trace_enabled;
static bool linx_tlb_fault_trace_addr_filter_enabled;
static uint64_t linx_tlb_fault_trace_addr_lo;
static uint64_t linx_tlb_fault_trace_addr_hi = UINT64_MAX;
static bool linx_tlb_fault_trace_count_filter_enabled;
static uint64_t linx_tlb_fault_trace_count_lo;
static uint64_t linx_tlb_fault_trace_count_hi = UINT64_MAX;
static uint64_t linx_tlb_fault_trace_limit;
static uint64_t linx_tlb_fault_trace_emitted;
static bool linx_tlb_fill_trace_inited;
static bool linx_tlb_fill_trace_enabled;
static bool linx_tlb_fill_trace_va_filter_enabled;
static uint64_t linx_tlb_fill_trace_va_lo;
static uint64_t linx_tlb_fill_trace_va_hi = UINT64_MAX;
static bool linx_tlb_fill_trace_pc_filter_enabled;
static uint64_t linx_tlb_fill_trace_pc_lo;
static uint64_t linx_tlb_fill_trace_pc_hi = UINT64_MAX;
static bool linx_tlb_fill_trace_count_filter_enabled;
static uint64_t linx_tlb_fill_trace_count_lo;
static uint64_t linx_tlb_fill_trace_count_hi = UINT64_MAX;
static uint64_t linx_tlb_fill_trace_limit = 64;
static uint64_t linx_tlb_fill_trace_emitted;
static bool linx_tlb_fill_stats_inited;
static bool linx_tlb_fill_stats_enabled;
static bool linx_tlb_fill_hot_inited;
static bool linx_tlb_fill_hot_enabled;
static bool linx_mmu_cache_config_inited;
static bool linx_mmu_cache_enabled;
static bool linx_mmu_cache_stats_enabled;
static bool linx_mmu_cache_assoc2_enabled;
static bool linx_mmu_cache_victim_enabled;
static bool linx_tp_trace_inited;
static bool linx_tp_trace_enabled;
static uint64_t linx_tp_trace_limit;
static uint64_t linx_tp_trace_emitted;
static bool linx_trap_delivery_trace_inited;
static bool linx_trap_delivery_trace_enabled;
static bool linx_trap_delivery_trace_pc_filter_enabled;
static uint64_t linx_trap_delivery_trace_pc_lo;
static uint64_t linx_trap_delivery_trace_pc_hi = UINT64_MAX;
static bool linx_trap_delivery_trace_count_filter_enabled;
static uint64_t linx_trap_delivery_trace_count_lo;
static uint64_t linx_trap_delivery_trace_count_hi = UINT64_MAX;
static uint64_t linx_trap_delivery_trace_limit = 128;
static uint64_t linx_trap_delivery_trace_emitted;

typedef struct LinxLegacyMmuProbe {
    bool legacy;
    bool ok;
    unsigned level;
    hwaddr desc_addr;
    uint64_t desc;
    hwaddr pa;
    hwaddr block_size;
    int prot;
    uint8_t cause;
    const char *why;
} LinxLegacyMmuProbe;

static bool linx_cpu_parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !s[0]) {
        return false;
    }

    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno || end == s || (end && *end != '\0')) {
        return false;
    }

    *out = (uint64_t)v;
    return true;
}

static bool linx_cpu_env_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] && strcmp(value, "0") != 0;
}

static const char *linx_cpu_env_nonzero2(const char *name, const char *alias)
{
    const char *value = getenv(name);

    if (value && value[0] && strcmp(value, "0") != 0) {
        return value;
    }
    value = getenv(alias);
    if (value && value[0] && strcmp(value, "0") != 0) {
        return value;
    }
    return NULL;
}

static const char *linx_cpu_env_value2(const char *name, const char *alias)
{
    const char *value = getenv(name);

    if (value && value[0]) {
        return value;
    }
    value = getenv(alias);
    if (value && value[0]) {
        return value;
    }
    return NULL;
}

static const char *linx_cpu_env_value3(const char *name,
                                       const char *alias1,
                                       const char *alias2)
{
    const char *value = linx_cpu_env_value2(name, alias1);

    if (value) {
        return value;
    }
    value = getenv(alias2);
    if (value && value[0]) {
        return value;
    }
    return NULL;
}

static const char *linx_cpu_env_value4(const char *name,
                                       const char *alias1,
                                       const char *alias2,
                                       const char *alias3)
{
    const char *value = linx_cpu_env_value3(name, alias1, alias2);

    if (value) {
        return value;
    }
    value = getenv(alias3);
    if (value && value[0]) {
        return value;
    }
    return NULL;
}

static const char *const linx_cpu_gpr_names[LINX_GPR_COUNT] = {
    "zero", "sp", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "ra", "s0", "s1", "s2", "s3", "s4",
    "s5", "s6", "s7", "s8", "x0", "x1", "x2", "x3",
};

static void linx_cpu_fprint_gprs(FILE *f, CPULinxState *env)
{
    for (unsigned i = 0; i < LINX_GPR_COUNT; i++) {
        fprintf(f, " %s=0x%" PRIx64, linx_cpu_gpr_names[i], env->gpr[i]);
    }
}

static inline bool linx_cpu_dump_debug(void)
{
    if (!linx_cpu_dump_debug_inited) {
        const char *v = getenv("LINX_CPU_DUMP_DEBUG");
        linx_cpu_dump_debug_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_cpu_dump_debug_inited = true;
    }
    return linx_cpu_dump_debug_enabled;
}

static inline uint32_t linx_managing_acr(uint32_t acr)
{
    return (acr == 0) ? 0 : 1;
}

static inline bool linx_cpu_dump_on_event(void)
{
    if (!linx_cpu_dump_on_event_inited) {
        const char *v = getenv("LINX_CPU_DUMP_ON_EVENT");
        linx_cpu_dump_on_event_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_cpu_dump_on_event_inited = true;
    }
    return linx_cpu_dump_on_event_enabled;
}

static void linx_fault_trace_init(void)
{
    if (linx_fault_trace_inited) {
        return;
    }

    const char *enabled_s =
        linx_cpu_env_nonzero2("LINX_FAULT_TRACE", "LINX_QEMU_FAULT_TRACE");
    linx_fault_trace_enabled = enabled_s != NULL;

    uint64_t lo = 0;
    uint64_t hi = 0;
    const char *lo_s = linx_cpu_env_nonzero2("LINX_FAULT_TRACE_PC_LO",
                                             "LINX_QEMU_FAULT_TRACE_PC_LO");
    const char *hi_s = linx_cpu_env_nonzero2("LINX_FAULT_TRACE_PC_HI",
                                             "LINX_QEMU_FAULT_TRACE_PC_HI");
    if (lo_s && hi_s &&
        linx_cpu_parse_u64(lo_s, &lo) && linx_cpu_parse_u64(hi_s, &hi)) {
        linx_fault_trace_filter_lo = MIN(lo, hi);
        linx_fault_trace_filter_hi = MAX(lo, hi);
        linx_fault_trace_filter_enabled = true;
    }
    const char *pc_s = linx_cpu_env_value2("LINX_FAULT_TRACE_PC",
                                           "LINX_QEMU_FAULT_TRACE_PC");
    if (pc_s && linx_cpu_parse_u64(pc_s, &lo)) {
        linx_fault_trace_filter_lo = lo;
        linx_fault_trace_filter_hi = lo;
        linx_fault_trace_filter_enabled = true;
    }

    lo = 0;
    hi = 0;
    lo_s = linx_cpu_env_value4("LINX_FAULT_TRACE_ADDR_LO",
                               "LINX_QEMU_FAULT_TRACE_ADDR_LO",
                               "LINX_FAULT_TRACE_VA_LO",
                               "LINX_QEMU_FAULT_TRACE_VA_LO");
    hi_s = linx_cpu_env_value4("LINX_FAULT_TRACE_ADDR_HI",
                               "LINX_QEMU_FAULT_TRACE_ADDR_HI",
                               "LINX_FAULT_TRACE_VA_HI",
                               "LINX_QEMU_FAULT_TRACE_VA_HI");
    if (lo_s && hi_s &&
        linx_cpu_parse_u64(lo_s, &lo) && linx_cpu_parse_u64(hi_s, &hi)) {
        linx_fault_trace_addr_lo = MIN(lo, hi);
        linx_fault_trace_addr_hi = MAX(lo, hi);
        linx_fault_trace_addr_filter_enabled = true;
    }
    const char *addr_s = linx_cpu_env_value4("LINX_FAULT_TRACE_ADDR",
                                             "LINX_QEMU_FAULT_TRACE_ADDR",
                                             "LINX_FAULT_TRACE_VA",
                                             "LINX_QEMU_FAULT_TRACE_VA");
    if (addr_s && linx_cpu_parse_u64(addr_s, &lo)) {
        linx_fault_trace_addr_lo = lo;
        linx_fault_trace_addr_hi = lo;
        linx_fault_trace_addr_filter_enabled = true;
    }

    lo = 0;
    hi = 0;
    lo_s = linx_cpu_env_nonzero2("LINX_FAULT_TRACE_COUNT_LO",
                                 "LINX_QEMU_FAULT_TRACE_COUNT_LO");
    hi_s = linx_cpu_env_nonzero2("LINX_FAULT_TRACE_COUNT_HI",
                                 "LINX_QEMU_FAULT_TRACE_COUNT_HI");
    if (lo_s && hi_s &&
        linx_cpu_parse_u64(lo_s, &lo) && linx_cpu_parse_u64(hi_s, &hi)) {
        linx_fault_trace_count_lo = MIN(lo, hi);
        linx_fault_trace_count_hi = MAX(lo, hi);
        linx_fault_trace_count_filter_enabled = true;
    }

    const char *trapnum_s = linx_cpu_env_nonzero2("LINX_FAULT_TRACE_TRAPNUM",
                                                  "LINX_QEMU_FAULT_TRACE_TRAPNUM");
    if (trapnum_s &&
        linx_cpu_parse_u64(trapnum_s, &linx_fault_trace_trapnum)) {
        linx_fault_trace_trapnum_filter_enabled = true;
    }

    const char *limit_s = linx_cpu_env_nonzero2("LINX_FAULT_TRACE_LIMIT",
                                                "LINX_QEMU_FAULT_TRACE_LIMIT");
    if (limit_s) {
        (void)linx_cpu_parse_u64(limit_s, &linx_fault_trace_limit);
    }

    linx_fault_trace_regs_enabled =
        linx_cpu_env_enabled("LINX_FAULT_TRACE_REGS") ||
        linx_cpu_env_enabled("LINX_QEMU_FAULT_TRACE_REGS") ||
        linx_cpu_env_enabled("LINX_TRACE_REGS");

    linx_fault_trace_inited = true;
}

static void linx_fault_trace_emit_regs(CPULinxState *env, uint8_t trapnum,
                                       uint64_t tpc, uint64_t report_bpc)
{
    if (!linx_fault_trace_regs_enabled) {
        return;
    }

    fprintf(stderr,
            "LINX_FAULT_REGS trapnum=%u"
            " count=%" PRIu64
            " tpc=0x%" PRIx64
            " report_bpc=0x%" PRIx64,
            trapnum, env->insn_count, tpc, report_bpc);
    linx_cpu_fprint_gprs(stderr, env);
    fputc('\n', stderr);
}

static bool linx_fault_trace_addr_matches(uint64_t addr)
{
    return !linx_fault_trace_filter_enabled ||
           (addr >= linx_fault_trace_filter_lo &&
            addr <= linx_fault_trace_filter_hi);
}

static bool linx_fault_trace_fault_addr_matches(uint64_t addr)
{
    return !linx_fault_trace_addr_filter_enabled ||
           (addr >= linx_fault_trace_addr_lo &&
            addr <= linx_fault_trace_addr_hi);
}

static void linx_tlb_fault_trace_init(void)
{
    if (linx_tlb_fault_trace_inited) {
        return;
    }

    linx_tlb_fault_trace_enabled =
        linx_cpu_env_enabled("LINX_TLB_FAULT_TRACE") ||
        linx_cpu_env_enabled("LINX_QEMU_TLB_FAULT_TRACE");

    uint64_t lo = 0;
    uint64_t hi = 0;
    const char *lo_s =
        linx_cpu_env_value4("LINX_TLB_FAULT_TRACE_ADDR_LO",
                            "LINX_QEMU_TLB_FAULT_TRACE_ADDR_LO",
                            "LINX_TLB_FAULT_TRACE_VA_LO",
                            "LINX_QEMU_TLB_FAULT_TRACE_VA_LO");
    const char *hi_s =
        linx_cpu_env_value4("LINX_TLB_FAULT_TRACE_ADDR_HI",
                            "LINX_QEMU_TLB_FAULT_TRACE_ADDR_HI",
                            "LINX_TLB_FAULT_TRACE_VA_HI",
                            "LINX_QEMU_TLB_FAULT_TRACE_VA_HI");
    if (lo_s && hi_s &&
        linx_cpu_parse_u64(lo_s, &lo) && linx_cpu_parse_u64(hi_s, &hi)) {
        linx_tlb_fault_trace_addr_lo = MIN(lo, hi);
        linx_tlb_fault_trace_addr_hi = MAX(lo, hi);
        linx_tlb_fault_trace_addr_filter_enabled = true;
    }
    const char *addr_s =
        linx_cpu_env_value4("LINX_TLB_FAULT_TRACE_ADDR",
                            "LINX_QEMU_TLB_FAULT_TRACE_ADDR",
                            "LINX_TLB_FAULT_TRACE_VA",
                            "LINX_QEMU_TLB_FAULT_TRACE_VA");
    if (addr_s && linx_cpu_parse_u64(addr_s, &lo)) {
        linx_tlb_fault_trace_addr_lo = lo;
        linx_tlb_fault_trace_addr_hi = lo;
        linx_tlb_fault_trace_addr_filter_enabled = true;
    }

    lo = 0;
    hi = 0;
    lo_s = linx_cpu_env_nonzero2("LINX_TLB_FAULT_TRACE_COUNT_LO",
                                 "LINX_QEMU_TLB_FAULT_TRACE_COUNT_LO");
    hi_s = linx_cpu_env_nonzero2("LINX_TLB_FAULT_TRACE_COUNT_HI",
                                 "LINX_QEMU_TLB_FAULT_TRACE_COUNT_HI");
    if (lo_s && hi_s &&
        linx_cpu_parse_u64(lo_s, &lo) && linx_cpu_parse_u64(hi_s, &hi)) {
        linx_tlb_fault_trace_count_lo = MIN(lo, hi);
        linx_tlb_fault_trace_count_hi = MAX(lo, hi);
        linx_tlb_fault_trace_count_filter_enabled = true;
    }

    const char *limit_s =
        linx_cpu_env_nonzero2("LINX_TLB_FAULT_TRACE_LIMIT",
                              "LINX_QEMU_TLB_FAULT_TRACE_LIMIT");
    if (limit_s) {
        (void)linx_cpu_parse_u64(limit_s, &linx_tlb_fault_trace_limit);
    }

    linx_tlb_fault_trace_inited = true;
}

static bool linx_fault_trace_matches(CPULinxState *env, uint8_t trapnum,
                                     uint64_t tpc, uint64_t tpc_next,
                                     uint64_t src_bpc, uint64_t report_bpc)
{
    linx_fault_trace_init();
    if (!linx_fault_trace_enabled) {
        return false;
    }
    if (linx_fault_trace_trapnum_filter_enabled &&
        trapnum != linx_fault_trace_trapnum) {
        return false;
    }
    if (linx_fault_trace_count_filter_enabled &&
        (env->insn_count < linx_fault_trace_count_lo ||
         env->insn_count > linx_fault_trace_count_hi)) {
        return false;
    }
    if (linx_fault_trace_limit &&
        linx_fault_trace_emitted >= linx_fault_trace_limit) {
        return false;
    }
    if (!linx_fault_trace_addr_matches(tpc) &&
        !linx_fault_trace_addr_matches(tpc_next) &&
        !linx_fault_trace_addr_matches(src_bpc) &&
        !linx_fault_trace_addr_matches(report_bpc) &&
        !linx_fault_trace_addr_matches(env->pc) &&
        !linx_fault_trace_addr_matches(env->pending_trap_arg0)) {
        return false;
    }
    /* TRAPNO.TRAPNUM data exception is 1; the named enum is declared later. */
    if (!linx_fault_trace_fault_addr_matches(
            trapnum == 1u ? env->pending_trap_arg0 : tpc)) {
        return false;
    }
    linx_fault_trace_emitted++;
    return true;
}

static bool linx_fault_trace_tlb_matches(CPULinxState *env, uint64_t va)
{
    linx_fault_trace_init();
    linx_tlb_fault_trace_init();
    if (!linx_tlb_fault_trace_enabled) {
        return false;
    }
    if (linx_tlb_fault_trace_count_filter_enabled &&
        (env->insn_count < linx_tlb_fault_trace_count_lo ||
         env->insn_count > linx_tlb_fault_trace_count_hi)) {
        return false;
    }
    if (!linx_tlb_fault_trace_count_filter_enabled &&
        linx_fault_trace_count_filter_enabled &&
        (env->insn_count < linx_fault_trace_count_lo ||
         env->insn_count > linx_fault_trace_count_hi)) {
        return false;
    }
    if (linx_tlb_fault_trace_limit &&
        linx_tlb_fault_trace_emitted >= linx_tlb_fault_trace_limit) {
        return false;
    }
    if (!linx_fault_trace_addr_matches(env->pc) &&
        !linx_fault_trace_addr_matches(env->insn_pc_next) &&
        !linx_fault_trace_addr_matches(env->bpc) &&
        !linx_fault_trace_addr_matches(env->body_tpc) &&
        !linx_fault_trace_addr_matches(va)) {
        return false;
    }
    if (linx_tlb_fault_trace_addr_filter_enabled &&
        (va < linx_tlb_fault_trace_addr_lo ||
         va > linx_tlb_fault_trace_addr_hi)) {
        return false;
    }
    linx_tlb_fault_trace_emitted++;
    return true;
}

/* Managing-ACR SSR indices (low 12 bits). */
enum {
    LINX_SSR_ECSTATE  = 0xF00,
    LINX_SSR_EVBASE   = 0xF01,
    LINX_SSR_TRAPNO   = 0xF02,
    LINX_SSR_TRAPARG0 = 0xF03,
    LINX_SSR_ETEMP    = 0xF05,
    LINX_SSR_ETEMP0   = 0xF06,
    LINX_SSR_IPENDING = 0xF08,
    LINX_SSR_EOIEI    = 0xF0A,
    LINX_SSR_TIMECMP  = 0xF21,

    /* ACR1 privileged MMU/IOMMU registers (see linxisa manual). */
    LINX_SSR_TTBR0    = 0xF10,
    LINX_SSR_TTBR1    = 0xF11,
    LINX_SSR_TCR      = 0xF12,
    LINX_SSR_MAIR     = 0xF13,
    LINX_SSR_IOTTBR   = 0xF14,
    LINX_SSR_IOTCR    = 0xF15,
    LINX_SSR_IOMAIR   = 0xF16,

    /* EBARG register group (v0.2). */
    LINX_SSR_EBARG0          = 0xF40,
    LINX_SSR_EBARG_BPC_CUR   = 0xF41,
    LINX_SSR_EBARG_BPC_TGT   = 0xF42,
    LINX_SSR_EBARG_TPC       = 0xF43,
    LINX_SSR_EBARG_LRA       = 0xF44,
    LINX_SSR_EBARG_TQ0       = 0xF45,
    LINX_SSR_EBARG_TQ1       = 0xF46,
    LINX_SSR_EBARG_TQ2       = 0xF47,
    LINX_SSR_EBARG_TQ3       = 0xF48,
    LINX_SSR_EBARG_UQ0       = 0xF49,
    LINX_SSR_EBARG_UQ1       = 0xF4A,
    LINX_SSR_EBARG_UQ2       = 0xF4B,
    LINX_SSR_EBARG_UQ3       = 0xF4C,
    LINX_SSR_EBARG_LB        = 0xF4D,
    LINX_SSR_EBARG_LC        = 0xF4E,
    LINX_SSR_EBARG_EXT_PTR   = 0xF4F,
    LINX_SSR_EBARG_EXT_META  = 0xF50,

    /* Debug SSR bank (v0.2). */
    LINX_SSR_DBGID           = 0xF80,
    LINX_SSR_DBCR0           = 0xF90,
    LINX_SSR_DBVR0           = 0xF91,
    LINX_SSR_DCCR0           = 0xFA0,
    LINX_SSR_DCVR0           = 0xFA1,
    LINX_SSR_DWCR0           = 0xFB0,
    LINX_SSR_DWVR0           = 0xFB1,
};

static inline bool linx_dbg_active_for_acr(const CPULinxState *env, uint32_t acr)
{
    for (uint32_t n = 0; n < 4; n++) {
        if (env->ssr_acr[acr][LINX_SSR_DBCR0 + 2u * n] & 1u) {
            return true;
        }
        if (env->ssr_acr[acr][LINX_SSR_DWCR0 + 2u * n] & 1u) {
            return true;
        }
    }
    return false;
}

static inline void linx_refresh_tb_dbg_active(CPULinxState *env)
{
    const uint32_t acr = env->acr & 0xFu;

    if (acr >= LINX_ACR_COUNT) {
        env->tb_dbg_active = 0;
        return;
    }
    env->tb_dbg_active = linx_dbg_active_for_acr(env, acr) ? 1 : 0;
}

#define LINX_LEGACY_MMTBASE_MASK        UINT64_C(0x000000fffffffffc)
#define LINX_LEGACY_MMTBASE_TO_PA(v)    (((v) & LINX_LEGACY_MMTBASE_MASK) << 10)
#define LINX_LEGACY_MMCONFIG_MODE_MASK  UINT64_C(0x3)
#define LINX_LEGACY_MMCONFIG_Q_BIT      (UINT64_C(1) << 7)
#define LINX_LEGACY_MMCONFIG_ENABLE_BIT (UINT64_C(1) << 63)
#define LINX_LEGACY_PTE_V               UINT64_C(1) << 0
#define LINX_LEGACY_PTE_X               UINT64_C(1) << 1
#define LINX_LEGACY_PTE_W               UINT64_C(1) << 2
#define LINX_LEGACY_PTE_R               UINT64_C(1) << 3
#define LINX_LEGACY_PTE_U               UINT64_C(1) << 4
#define LINX_LEGACY_PTE_AF              UINT64_C(1) << 22
#define LINX_LEGACY_PTE_LEAF_MASK       (LINX_LEGACY_PTE_R | LINX_LEGACY_PTE_W | LINX_LEGACY_PTE_X)

/* Common (non-banked) SSR indices. */
enum {
    LINX_SSR_TP     = 0x0000,
    LINX_SSR_CSTATE = 0x0020,
};

/* CSTATE bits (keep in sync with target/linx/helper.c). */
#define LINX_CSTATE_ACR_MASK 0xFULL
#define LINX_CSTATE_I_BIT    (1ULL << 4)

/* ECSTATE bits (v0.2 bring-up profile; mirrors key CSTATE fields). */
#define LINX_ECSTATE_BI_BIT        (1ULL << 62)

static void linx_cpu_tp_trace_init(void)
{
    if (linx_tp_trace_inited) {
        return;
    }

    linx_tp_trace_enabled = linx_cpu_env_enabled("LINX_TP_TRACE");

    const char *limit_s = getenv("LINX_TP_TRACE_LIMIT");
    if (limit_s) {
        (void)linx_cpu_parse_u64(limit_s, &linx_tp_trace_limit);
    }

    linx_tp_trace_inited = true;
}

static bool linx_cpu_tp_trace_enabled_p(void)
{
    linx_cpu_tp_trace_init();
    if (!linx_tp_trace_enabled) {
        return false;
    }
    if (linx_tp_trace_limit != 0 &&
        linx_tp_trace_emitted >= linx_tp_trace_limit) {
        return false;
    }
    return true;
}

static void linx_cpu_tp_trace_emit_handoff(CPULinxState *env,
                                           const char *event,
                                           uint32_t src_acr,
                                           uint32_t dst_acr,
                                           uint64_t user_tp,
                                           uint64_t thread_info)
{
    if (!linx_cpu_tp_trace_enabled_p()) {
        return;
    }

    linx_tp_trace_emitted++;
    fprintf(stderr,
            "LINX_TP_TRACE event=%s seq=%" PRIu64
            " count=%" PRIu64
            " pc=0x%" PRIx64 " bpc=0x%" PRIx64 " tpc=0x%" PRIx64
            " envpc=0x%" PRIx64 " src_acr=%u dst_acr=%u"
            " acr=%u cstate=0x%" PRIx64
            " user_tp=0x%" PRIx64 " thread_info=0x%" PRIx64
            " tp=0x%" PRIx64 " etemp1=0x%" PRIx64
            " etemp0_1=0x%" PRIx64
            " sp=0x%" PRIx64 " ra=0x%" PRIx64
            " a0=0x%" PRIx64 " a1=0x%" PRIx64 "\n",
            event, linx_tp_trace_emitted,
            env->insn_count, env->pc, env->bpc, env->body_tpc,
            env->pc, src_acr, dst_acr, env->acr & 0xFu,
            env->ssr[LINX_SSR_CSTATE], user_tp, thread_info,
            env->ssr[LINX_SSR_TP], env->ssr_acr[1][LINX_SSR_ETEMP],
            env->ssr_acr[1][LINX_SSR_ETEMP0],
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1]);
    fflush(stderr);
}

static void linx_cpu_tp_trace_emit_same_acr_frame(CPULinxState *env,
                                                  const char *event,
                                                  uint32_t acr,
                                                  uint64_t saved_x1)
{
    if (!linx_cpu_tp_trace_enabled_p()) {
        return;
    }

    linx_tp_trace_emitted++;
    fprintf(stderr,
            "LINX_TP_TRACE event=%s seq=%" PRIu64
            " count=%" PRIu64
            " pc=0x%" PRIx64 " bpc=0x%" PRIx64 " tpc=0x%" PRIx64
            " envpc=0x%" PRIx64 " src_acr=%u dst_acr=%u"
            " acr=%u cstate=0x%" PRIx64
            " tp=0x%" PRIx64 " etemp=0x%" PRIx64
            " etemp0=0x%" PRIx64 " saved_x1=0x%" PRIx64
            " live_x1=0x%" PRIx64
            " sp=0x%" PRIx64 " ra=0x%" PRIx64
            " a0=0x%" PRIx64 " a1=0x%" PRIx64 "\n",
            event, linx_tp_trace_emitted,
            env->insn_count, env->pc, env->bpc, env->body_tpc,
            env->pc, acr, acr, env->acr & 0xFu,
            env->ssr[LINX_SSR_CSTATE], env->ssr[LINX_SSR_TP],
            env->ssr_acr[acr][LINX_SSR_ETEMP],
            env->ssr_acr[acr][LINX_SSR_ETEMP0], saved_x1,
            env->gpr[LINX_REG_X1], env->gpr[LINX_REG_SP],
            env->gpr[LINX_REG_RA], env->gpr[LINX_REG_A0],
            env->gpr[LINX_REG_A1]);
    fflush(stderr);
}

static void linx_trap_delivery_trace_init(void)
{
    const char *lo_s;
    const char *hi_s;
    uint64_t lo;
    uint64_t hi;

    if (linx_trap_delivery_trace_inited) {
        return;
    }

    linx_trap_delivery_trace_enabled =
        linx_cpu_env_enabled("LINX_TRAP_DELIVERY_TRACE") ||
        linx_cpu_env_enabled("LINX_QEMU_TRAP_DELIVERY_TRACE");

    lo_s = linx_cpu_env_nonzero2("LINX_TRAP_DELIVERY_TRACE_PC_LO",
                                 "LINX_QEMU_TRAP_DELIVERY_TRACE_PC_LO");
    hi_s = linx_cpu_env_nonzero2("LINX_TRAP_DELIVERY_TRACE_PC_HI",
                                 "LINX_QEMU_TRAP_DELIVERY_TRACE_PC_HI");
    if (lo_s && hi_s &&
        linx_cpu_parse_u64(lo_s, &lo) &&
        linx_cpu_parse_u64(hi_s, &hi)) {
        linx_trap_delivery_trace_pc_lo = MIN(lo, hi);
        linx_trap_delivery_trace_pc_hi = MAX(lo, hi);
        linx_trap_delivery_trace_pc_filter_enabled = true;
        linx_trap_delivery_trace_enabled = true;
    }

    lo_s = linx_cpu_env_nonzero2("LINX_TRAP_DELIVERY_TRACE_PC",
                                 "LINX_QEMU_TRAP_DELIVERY_TRACE_PC");
    if (lo_s && linx_cpu_parse_u64(lo_s, &lo)) {
        linx_trap_delivery_trace_pc_lo = lo;
        linx_trap_delivery_trace_pc_hi = lo;
        linx_trap_delivery_trace_pc_filter_enabled = true;
        linx_trap_delivery_trace_enabled = true;
    }

    lo_s = linx_cpu_env_nonzero2("LINX_TRAP_DELIVERY_TRACE_COUNT_LO",
                                 "LINX_QEMU_TRAP_DELIVERY_TRACE_COUNT_LO");
    hi_s = linx_cpu_env_nonzero2("LINX_TRAP_DELIVERY_TRACE_COUNT_HI",
                                 "LINX_QEMU_TRAP_DELIVERY_TRACE_COUNT_HI");
    if (lo_s && hi_s &&
        linx_cpu_parse_u64(lo_s, &lo) &&
        linx_cpu_parse_u64(hi_s, &hi)) {
        linx_trap_delivery_trace_count_lo = MIN(lo, hi);
        linx_trap_delivery_trace_count_hi = MAX(lo, hi);
        linx_trap_delivery_trace_count_filter_enabled = true;
        linx_trap_delivery_trace_enabled = true;
    }

    lo_s = linx_cpu_env_nonzero2("LINX_TRAP_DELIVERY_TRACE_LIMIT",
                                 "LINX_QEMU_TRAP_DELIVERY_TRACE_LIMIT");
    if (lo_s) {
        (void)linx_cpu_parse_u64(lo_s, &linx_trap_delivery_trace_limit);
    }

    linx_trap_delivery_trace_inited = true;
}

static bool linx_trap_delivery_trace_matches(CPULinxState *env,
                                             uint64_t tpc,
                                             uint64_t report_bpc)
{
    linx_trap_delivery_trace_init();

    if (!linx_trap_delivery_trace_enabled) {
        return false;
    }
    if (linx_trap_delivery_trace_limit != 0 &&
        linx_trap_delivery_trace_emitted >=
        linx_trap_delivery_trace_limit) {
        return false;
    }
    if (linx_trap_delivery_trace_count_filter_enabled &&
        (env->insn_count < linx_trap_delivery_trace_count_lo ||
         env->insn_count > linx_trap_delivery_trace_count_hi)) {
        return false;
    }
    if (linx_trap_delivery_trace_pc_filter_enabled &&
        !((tpc >= linx_trap_delivery_trace_pc_lo &&
           tpc <= linx_trap_delivery_trace_pc_hi) ||
          (report_bpc >= linx_trap_delivery_trace_pc_lo &&
           report_bpc <= linx_trap_delivery_trace_pc_hi))) {
        return false;
    }
    return true;
}

static void linx_cpu_prepare_same_acr_exception_frame(CPULinxState *env,
                                                      const char *event,
                                                      uint32_t acr)
{
    const uint64_t saved_x1 = env->gpr[LINX_REG_X1];

    if (acr >= LINX_ACR_COUNT) {
        return;
    }

    /*
     * Linux's kernel-origin exception prologue expects x1 to be zero and the
     * interrupted x1 value to be recoverable from the current bank's ETEMP.
     * User-origin handoff uses a different TP/ETEMP0 path and must not flow
     * through this helper.
     */
    env->ssr_acr[acr][LINX_SSR_ETEMP] = saved_x1;
    env->gpr[LINX_REG_X1] = 0;
    linx_cpu_tp_trace_emit_same_acr_frame(env, event, acr, saved_x1);
}

/* TRAPNO encoding (v0.2 bring-up profile). */
#define LINX_TRAPNO_E_BIT          (1ULL << 63) /* 1=exception, 0=interrupt */
#define LINX_TRAPNO_ARGV_BIT       (1ULL << 62)
#define LINX_TRAPNO_CAUSE_SHIFT    24u
#define LINX_TRAPNO_CAUSE_MASK     0xFFFFFFu
#define LINX_TRAPNO_TRAPNUM_MASK   0x3Fu

enum {
    /* Linux-compatible trap major classes (TRAPNO.TRAPNUM). */
    LINX_TRAPNUM_INSN_EXP         = 0,
    LINX_TRAPNUM_DATA_EXP         = 1,
    LINX_TRAPNUM_EXEC_STATE_CHECK = 0,
    LINX_TRAPNUM_ILLEGAL_INST     = 0,
    LINX_TRAPNUM_BLOCK_TRAP       = 5,
    LINX_TRAPNUM_SYSCALL          = 16,
    /*
     * Linux currently routes EBREAK through ECAUSE_TRAPNUM_BREAKPOINT_EXP=17.
     * Keep software breakpoints aligned with that contract so WARN/BUG fixups
     * land in do_trap_break() instead of the generic unknown-exception path.
     */
    LINX_TRAPNUM_SW_BREAKPOINT    = 17,
    LINX_TRAPNUM_INTERRUPT        = 44,
    LINX_TRAPNUM_HW_BREAKPOINT    = 49,
    LINX_TRAPNUM_HW_WATCHPOINT    = 51,
};

enum {
    LINX_TRAPCAUSE_CAT_NONE      = 0,
    LINX_TRAPCAUSE_CAT_MMU_PF    = 1,
    LINX_TRAPCAUSE_CAT_MMU_PERM  = 2,
    LINX_TRAPCAUSE_CAT_IOMMU_PF  = 3,
};

enum {
    LINX_TRAPCAUSE_ACC_LOAD  = 0,
    LINX_TRAPCAUSE_ACC_STORE = 1,
    LINX_TRAPCAUSE_ACC_INST  = 2,
};

static inline uint8_t linx_linux_fault_syndrome(uint8_t cat, uint8_t acc)
{
    switch (acc) {
    case LINX_TRAPCAUSE_ACC_INST:
        switch (cat) {
        case LINX_TRAPCAUSE_CAT_NONE:
            return 0; /* ECAUSE_INSN_SYD_ACCESS_FAULT */
        case LINX_TRAPCAUSE_CAT_MMU_PERM:
            return 4; /* ECAUSE_INSN_SYD_PERM_FAULT */
        case LINX_TRAPCAUSE_CAT_MMU_PF:
        default:
            return 5; /* ECAUSE_INSN_SYD_PAGE_FAULT */
        }
    case LINX_TRAPCAUSE_ACC_STORE:
        switch (cat) {
        case LINX_TRAPCAUSE_CAT_NONE:
            return 3; /* ECAUSE_DATA_SYD_ST_AOP_ACCESS_FAULT */
        case LINX_TRAPCAUSE_CAT_MMU_PERM:
        case LINX_TRAPCAUSE_CAT_MMU_PF:
        default:
            return 5; /* ECAUSE_DATA_SYD_ST_AOP_PAGE_FAULT */
        }
    case LINX_TRAPCAUSE_ACC_LOAD:
    default:
        switch (cat) {
        case LINX_TRAPCAUSE_CAT_NONE:
            return 0; /* ECAUSE_DATA_SYD_LD_ACCESS_FAULT */
        case LINX_TRAPCAUSE_CAT_MMU_PERM:
        case LINX_TRAPCAUSE_CAT_MMU_PF:
        default:
            return 2; /* ECAUSE_DATA_SYD_LD_PAGE_FAULT */
        }
    }
}

static inline uint8_t linx_trapcause_make(uint8_t cat, uint8_t acc)
{
    return linx_linux_fault_syndrome(cat, acc);
}

static inline uint64_t linx_trapno_make(bool exception, bool argv,
                                        uint32_t cause, uint8_t trapnum)
{
    const uint64_t e = exception ? LINX_TRAPNO_E_BIT : 0;
    const uint64_t a = argv ? LINX_TRAPNO_ARGV_BIT : 0;
    const uint64_t c = ((uint64_t)(cause & LINX_TRAPNO_CAUSE_MASK)) << LINX_TRAPNO_CAUSE_SHIFT;
    const uint64_t t = (uint64_t)(trapnum & LINX_TRAPNO_TRAPNUM_MASK);
    return e | a | c | t;
}

static bool linx_mmu_translate(CPUState *cs, CPULinxState *env, vaddr va,
                               MMUAccessType access_type, int mmu_idx,
                               hwaddr *pa_out, int *prot_out,
                               hwaddr *tlb_size_out, uint8_t *cause_out);
static inline uint8_t linx_fault_acc(MMUAccessType access_type);
static LinxLegacyMmuProbe linx_probe_legacy_mmu(CPULinxState *env, vaddr va,
                                                MMUAccessType access_type,
                                                int mmu_idx);

static bool linx_read_insn_bytes(CPUState *cs, uint64_t pc,
                                 uint8_t *buf, size_t len)
{
    CPULinxState *env = cpu_env(cs);
    size_t done = 0;

    while (done < len) {
        const vaddr va = (vaddr)(pc + done);
        const int mmu_idx = ((env->acr & 0xFu) == 2) ? 1 : 0;
        hwaddr pa = 0;
        int prot = 0;
        hwaddr tlb_size = TARGET_PAGE_SIZE;
        uint8_t cause = 0;

        if (!linx_mmu_translate(cs, env, va, MMU_INST_FETCH, mmu_idx,
                                &pa, &prot, &tlb_size, &cause)) {
            return false;
        }

        const size_t page_left =
            TARGET_PAGE_SIZE - (size_t)(pa & (TARGET_PAGE_SIZE - 1));
        const size_t n = MIN(len - done, page_left);
        MemTxResult result =
            address_space_read(&address_space_memory, pa,
                               MEMTXATTRS_UNSPECIFIED, buf + done, n);

        if (result != MEMTX_OK) {
            return false;
        }
        done += n;
    }

    return true;
}

static uint64_t linx_break_resume_pc(CPUState *cs, uint64_t tpc_next,
                                     bool in_body)
{
    uint8_t buf[4];

    if (linx_read_insn_bytes(cs, tpc_next, buf, 2)) {
        const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        if (hw == 0x0000u) {
            if (in_body) {
                return tpc_next;
            }
            return tpc_next + 2;
        }
    }

    if (linx_read_insn_bytes(cs, tpc_next, buf, 4)) {
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        if (insn == 0x00000001u) {
            if (in_body) {
                return tpc_next;
            }
            return tpc_next + 4;
        }
    }

    return tpc_next;
}

static unsigned linx_fetch_insn_len(CPUState *cs, uint64_t pc)
{
    uint8_t buf[2];

    if (!linx_read_insn_bytes(cs, pc, buf, sizeof(buf))) {
        return 0;
    }

    const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if ((hw & 0x1) == 0) {
        return ((hw & 0xf) == 0xe) ? 6 : 2;
    }
    return ((hw & 0xf) == 0xf) ? 8 : 4;
}

static bool linx_cpu_is_bstart_at_addr(CPUState *cs, uint64_t pc)
{
    uint8_t buf[8];

    if (!linx_read_insn_bytes(cs, pc, buf, 2)) {
        return false;
    }

    const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    const unsigned len = linx_fetch_insn_len(cs, pc);

    if (len == 2) {
        if ((hw & 0xc7ff) == 0x0000 || (hw & 0xc7ff) == 0x0080) {
            const uint8_t brtype = (hw >> 11) & 0x7;
            if (brtype != 0) {
                return true;
            }
        }
        if ((hw & 0x000f) == 0x0002 || (hw & 0x000f) == 0x0004) {
            return true;
        }
        switch (hw) {
        case 0x0840:
        case 0x08c0:
        case 0x48c0:
        case 0x88c0:
        case 0xc8c0:
            return true;
        default:
            return false;
        }
    }

    if (len == 4) {
        if (!linx_read_insn_bytes(cs, pc, buf, 4)) {
            return false;
        }
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) |
                              ((uint32_t)buf[3] << 24);
        if ((insn & 0x7f) == 0x11 || (insn & 0x7f) == 0x21) {
            return true;
        }
        if ((insn & 0x7f) == 0x01 && ((insn >> 12) & 0x7) != 0) {
            return true;
        }
        if ((insn & 0x7f) == 0x41 && ((insn >> 12) & 0x7) <= 3) {
            return true;
        }
        if ((insn & 0x7f) == 0x31 && ((insn >> 7) & 0x1f) == 0 &&
            ((insn >> 12) & 0x7) <= 1) {
            return true;
        }
        return false;
    }

    if (len == 6) {
        if (!linx_read_insn_bytes(cs, pc, buf, 6)) {
            return false;
        }
        const uint16_t prefix = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        const uint32_t main32 = (uint32_t)buf[2] |
                                ((uint32_t)buf[3] << 8) |
                                ((uint32_t)buf[4] << 16) |
                                ((uint32_t)buf[5] << 24);
        return (prefix & 0xf) == 0xe &&
               (main32 & 0xff) == 0x01 &&
               ((main32 >> 12) & 0x7) != 0;
    }

    if (len == 8) {
        if (!linx_read_insn_bytes(cs, pc, buf, 8)) {
            return false;
        }
        const uint32_t main32 = (uint32_t)buf[4] |
                                ((uint32_t)buf[5] << 8) |
                                ((uint32_t)buf[6] << 16) |
                                ((uint32_t)buf[7] << 24);
        return (main32 & 0x7f) == 0x01 && ((main32 >> 12) & 0x7) != 0;
    }

    return false;
}

static uint64_t linx_containing_insn_start(CPUState *cs, uint64_t block_pc,
                                           uint64_t reported_pc)
{
    uint64_t pc = block_pc;

    if (block_pc == 0 || block_pc > reported_pc) {
        return reported_pc;
    }

    for (unsigned i = 0; i < 4096 && pc < reported_pc; i++) {
        const unsigned len = linx_fetch_insn_len(cs, pc);

        if (len == 0) {
            break;
        }
        if (reported_pc < pc + len) {
            return pc;
        }
        pc += len;
    }

    return reported_pc;
}

static bool linx_resolve_user_fault_block(CPUState *cs, uint64_t seed_bpc,
                                          uint64_t reported_pc,
                                          uint64_t *block_pc_out,
                                          uint64_t *insn_pc_out)
{
    uint64_t block_pc = seed_bpc;

    if (block_pc != 0 && block_pc <= reported_pc &&
        linx_cpu_is_bstart_at_addr(cs, block_pc)) {
        *block_pc_out = block_pc;
        *insn_pc_out = linx_containing_insn_start(cs, block_pc, reported_pc);
        return true;
    }

    /*
     * TCG restore can occasionally surface a data fault with env->pc already
     * inside the faulting instruction and env->bpc polluted by the same suffix
     * address.  Recover the nearest decoded block header so Linux resumes at a
     * real instruction boundary instead of feeding a halfword PC back to ACRE.
     */
    const uint64_t scan_limit = 512;
    const uint64_t lo = reported_pc > scan_limit ? reported_pc - scan_limit : 0;
    uint64_t cand = reported_pc & ~UINT64_C(1);

    while (cand >= lo) {
        if (linx_cpu_is_bstart_at_addr(cs, cand)) {
            uint64_t insn_pc = linx_containing_insn_start(cs, cand, reported_pc);
            const unsigned len = linx_fetch_insn_len(cs, insn_pc);

            if (len != 0 && reported_pc >= insn_pc &&
                reported_pc < insn_pc + len) {
                *block_pc_out = cand;
                *insn_pc_out = insn_pc;
                return true;
            }
        }
        if (cand < 2) {
            break;
        }
        cand -= 2;
    }

    return false;
}

static void linx_dump_q4(FILE *f, const char *name, const uint64_t q[4],
                         const char *head_name, const char *tail_name,
                         const char *producer)
{
    qemu_fprintf(f,
                 "%s[%s..%s]=[0x%016" PRIx64 ",0x%016" PRIx64
                 ",0x%016" PRIx64 ",0x%016" PRIx64 "]\n",
                 name, head_name, tail_name, q[0], q[1], q[2], q[3]);
    qemu_fprintf(f,
                 "  %s semantics: push-front by %s producer, consume from %s\n",
                 name, producer, head_name);
}

static void linx_dump_ebarg_bank(FILE *f, const CPULinxState *env,
                                 uint32_t bank, const char *tag)
{
    const uint64_t *ssr = env->ssr_acr[bank];

    qemu_fprintf(f,
                 "%s ACR%u: ECSTATE=0x%016" PRIx64 " TRAPNO=0x%016" PRIx64
                 " TRAPARG0=0x%016" PRIx64 " IPENDING=0x%016" PRIx64 "\n",
                 tag, bank,
                 ssr[LINX_SSR_ECSTATE], ssr[LINX_SSR_TRAPNO],
                 ssr[LINX_SSR_TRAPARG0], ssr[LINX_SSR_IPENDING]);

    qemu_fprintf(f,
                 "  EBARG0=0x%016" PRIx64 " BPC_CUR=0x%016" PRIx64
                 " BPC_TGT=0x%016" PRIx64 " TPC=0x%016" PRIx64
                 " LRA=0x%016" PRIx64 "\n",
                 ssr[LINX_SSR_EBARG0], ssr[LINX_SSR_EBARG_BPC_CUR],
                 ssr[LINX_SSR_EBARG_BPC_TGT], ssr[LINX_SSR_EBARG_TPC],
                 ssr[LINX_SSR_EBARG_LRA]);

    qemu_fprintf(f,
                 "  EBARG.TQ=[0x%016" PRIx64 ",0x%016" PRIx64
                 ",0x%016" PRIx64 ",0x%016" PRIx64 "]\n",
                 ssr[LINX_SSR_EBARG_TQ0], ssr[LINX_SSR_EBARG_TQ1],
                 ssr[LINX_SSR_EBARG_TQ2], ssr[LINX_SSR_EBARG_TQ3]);
    qemu_fprintf(f,
                 "  EBARG.UQ=[0x%016" PRIx64 ",0x%016" PRIx64
                 ",0x%016" PRIx64 ",0x%016" PRIx64 "]\n",
                 ssr[LINX_SSR_EBARG_UQ0], ssr[LINX_SSR_EBARG_UQ1],
                 ssr[LINX_SSR_EBARG_UQ2], ssr[LINX_SSR_EBARG_UQ3]);
    qemu_fprintf(f,
                 "  EBARG.LB=0x%016" PRIx64 " EBARG.LC=0x%016" PRIx64
                 " EBARG.EXT_PTR=0x%016" PRIx64
                 " EBARG.EXT_META=0x%016" PRIx64 "\n",
                 ssr[LINX_SSR_EBARG_LB], ssr[LINX_SSR_EBARG_LC],
                 ssr[LINX_SSR_EBARG_EXT_PTR], ssr[LINX_SSR_EBARG_EXT_META]);
}

static void linx_dump_bstate_snapshot(FILE *f, const char *label, uint32_t acr,
                                      const LinxAcrBlockState *s)
{
    qemu_fprintf(f,
                 "%s ACR%u: bstate{blocktype=%u brtype=%u carg=0x%08x cond=%u"
                 " tgt=0x%016" PRIx64 " bpc=0x%016" PRIx64
                 " in_body=%u body_tpc=0x%016" PRIx64
                 " return_pc=0x%016" PRIx64 "}\n",
                 label, acr,
                 s->blocktype, s->brtype, s->carg, s->cond, s->tgt, s->bpc,
                 s->in_body, s->body_tpc, s->return_pc);
    qemu_fprintf(f,
                 "  bstate.lb=[0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64
                 "] lc=[0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 "]\n",
                 s->lb[0], s->lb[1], s->lb[2], s->lc[0], s->lc[1], s->lc[2]);
    qemu_fprintf(f,
                 "  bstate.template={kind=%u step=%u pc=0x%016" PRIx64
                 " reg_cur=%u reg_range=[%u,%u] rem=0x%016" PRIx64 "}\n",
                 s->tmpl_kind, s->tmpl_step, s->tmpl_pc,
                 s->tmpl_reg_cur, s->tmpl_reg_begin, s->tmpl_reg_end,
                 s->tmpl_mem_remaining);
    qemu_fprintf(f,
                 "  bstate.tq=[0x%016" PRIx64 ",0x%016" PRIx64
                 ",0x%016" PRIx64 ",0x%016" PRIx64 "]\n",
                 s->tq[0], s->tq[1], s->tq[2], s->tq[3]);
    qemu_fprintf(f,
                 "  bstate.uq=[0x%016" PRIx64 ",0x%016" PRIx64
                 ",0x%016" PRIx64 ",0x%016" PRIx64 "]\n",
                 s->uq[0], s->uq[1], s->uq[2], s->uq[3]);
}

static void linx_dump_debug_ssr_bank(FILE *f, const CPULinxState *env,
                                     uint32_t bank, const char *tag)
{
    const uint64_t *ssr = env->ssr_acr[bank];
    unsigned n;

    for (n = 0; n < 4; n++) {
        const uint32_t dbcr = LINX_SSR_DBCR0 + (2u * n);
        const uint32_t dbvr = LINX_SSR_DBVR0 + (2u * n);
        const uint32_t dwcr = LINX_SSR_DWCR0 + (2u * n);
        const uint32_t dwvr = LINX_SSR_DWVR0 + (2u * n);

        qemu_fprintf(f,
                     "%s ACR%u slot%u: DBCR=0x%016" PRIx64
                     " DBVR=0x%016" PRIx64
                     " DWCR=0x%016" PRIx64
                     " DWVR=0x%016" PRIx64 "\n",
                     tag, bank, n,
                     ssr[dbcr], ssr[dbvr], ssr[dwcr], ssr[dwvr]);
    }
}

static void linx_dump_event_state(CPUState *cs, const char *tag, int exception)
{
    CPULinxState *env;
    const uint32_t mgr = linx_managing_acr(cpu_env(cs)->acr & 0xFu);

    if (!linx_cpu_dump_on_event()) {
        return;
    }

    env = cpu_env(cs);
    qemu_fprintf(stderr,
                 "linx-event: %s exception=%d pc=0x%016" PRIx64
                 " acr=%u cstate=0x%016" PRIx64
                 " ecstate_mgr=0x%016" PRIx64
                 " trapno_mgr=0x%016" PRIx64
                 " traparg0_mgr=0x%016" PRIx64
                 " ipending_mgr=0x%016" PRIx64
                 " pending_cause=0x%08" PRIx32
                 " pending_arg0=0x%016" PRIx64 "\n",
                 tag, exception, env->pc, env->acr & 0xFu, env->ssr[LINX_SSR_CSTATE],
                 env->ssr_acr[mgr][LINX_SSR_ECSTATE],
                 env->ssr_acr[mgr][LINX_SSR_TRAPNO],
                 env->ssr_acr[mgr][LINX_SSR_TRAPARG0],
                 env->ssr_acr[mgr][LINX_SSR_IPENDING],
                 env->pending_trap_cause,
                 env->pending_trap_arg0);
    linx_cpu_dump_state(cs, stderr, 0);
    fflush(stderr);
}

static bool linx_disable_timer_irq_inited;
static bool linx_disable_timer_irq;
static const vaddr linx_debug_jiffy_sched_clock_page =
    UINT64_C(0xffffffff800a6000);

static inline bool linx_timer_irq_enabled(void)
{
    if (!linx_disable_timer_irq_inited) {
        const char *v = getenv("LINX_DISABLE_TIMER_IRQ");
        linx_disable_timer_irq = v && v[0] && strcmp(v, "0") != 0;
        linx_disable_timer_irq_inited = true;
    }
    return !linx_disable_timer_irq;
}

/* Simple timer interrupt ID (bring-up). */
enum {
    LINX_IRQ_TIMER0 = 4,
};

static bool linx_mmu_translate(CPUState *cs, CPULinxState *env, vaddr va,
                               MMUAccessType access_type, int mmu_idx,
                               hwaddr *pa_out, int *prot_out,
                               hwaddr *tlb_size_out, uint8_t *cause_out);

static inline hwaddr linx_nommu_phys_addr(vaddr va)
{
    /*
     * NOMMU bring-up profile:
     * - keep identity mapping for normal low addresses (including MMIO windows),
     * - fold sign-extended low-31-bit addresses back into the low physical
     *   region so high-half kernel text/data still resolves to the loaded RAM
     *   window before full MMU enablement.
     */
    const uint64_t low_mask = 0x7fffffffULL;
    const uint64_t high_mask = ~low_mask;
    const uint64_t raw = (uint64_t)va;

    if ((raw & high_mask) == high_mask) {
        return (hwaddr)(raw & low_mask);
    }
    return (hwaddr)raw;
}

static inline uint64_t linx_cstate_set_acr(uint64_t cstate, uint32_t acr)
{
    return (cstate & ~LINX_CSTATE_ACR_MASK) | ((uint64_t)acr & LINX_CSTATE_ACR_MASK);
}

static inline bool linx_irq_allowed(const CPULinxState *env, uint32_t dst_acr)
{
    const uint32_t cur_acr = env->acr & 0xF;
    const uint64_t cstate = env->ssr[LINX_SSR_CSTATE];
    const bool ie = (cstate & LINX_CSTATE_I_BIT) != 0;

    if (dst_acr < cur_acr) {
        return true;
    }
    if (dst_acr == cur_acr) {
        return ie;
    }
    return ie;
}

static bool linx_mmu_cache_enabled_p(void)
{
    if (!linx_mmu_cache_config_inited) {
        const char *value =
            linx_cpu_env_value2("LINX_MMU_CACHE", "LINX_QEMU_MMU_CACHE");

        linx_mmu_cache_enabled =
            value && value[0] && strcmp(value, "0") &&
            strcmp(value, "false") && strcmp(value, "no") &&
            strcmp(value, "off");
        value = linx_cpu_env_value2("LINX_MMU_CACHE_STATS",
                                    "LINX_QEMU_MMU_CACHE_STATS");
        linx_mmu_cache_stats_enabled =
            value && value[0] && strcmp(value, "0") &&
            strcmp(value, "false") && strcmp(value, "no") &&
            strcmp(value, "off");
        value = linx_cpu_env_value2("LINX_MMU_CACHE_ASSOC2",
                                    "LINX_QEMU_MMU_CACHE_ASSOC2");
        linx_mmu_cache_assoc2_enabled =
            value && value[0] && strcmp(value, "0") &&
            strcmp(value, "false") && strcmp(value, "no") &&
            strcmp(value, "off");
        value = linx_cpu_env_value2("LINX_MMU_CACHE_VICTIM",
                                    "LINX_QEMU_MMU_CACHE_VICTIM");
        linx_mmu_cache_victim_enabled =
            value && value[0] && strcmp(value, "0") &&
            strcmp(value, "false") && strcmp(value, "no") &&
            strcmp(value, "off");
        linx_mmu_cache_config_inited = true;
    }
    return linx_mmu_cache_enabled;
}

static inline bool linx_mmu_cache_victim_enabled_p(void)
{
    return linx_mmu_cache_enabled_p() &&
           linx_mmu_cache_victim_enabled &&
           !linx_mmu_cache_assoc2_enabled;
}

static inline bool linx_mmu_cache_size_valid(hwaddr size)
{
    return size >= TARGET_PAGE_SIZE && (size & (size - 1u)) == 0;
}

static inline hwaddr linx_mmu_cache_size_or_page(hwaddr size)
{
    return linx_mmu_cache_size_valid(size) ? size : TARGET_PAGE_SIZE;
}

static inline vaddr linx_mmu_cache_base(vaddr va, hwaddr size)
{
    return va & ~(vaddr)(size - 1u);
}

static inline size_t linx_mmu_cache_slot(vaddr base, int mmu_idx,
                                         hwaddr size)
{
    uint64_t key = ((uint64_t)base >> TARGET_PAGE_BITS) ^
                   (((uint64_t)size >> TARGET_PAGE_BITS) << 17) ^
                   ((uint64_t)(unsigned)mmu_idx << 5);

    key ^= key >> 11;
    key ^= key >> 23;
    return (size_t)(key & (LINX_MMU_CACHE_SIZE - 1u));
}

static inline size_t linx_mmu_cache_set_base(size_t slot)
{
    return linx_mmu_cache_assoc2_enabled ? (slot & ~(size_t)1u) : slot;
}

static inline size_t linx_mmu_cache_way_count(void)
{
    return linx_mmu_cache_assoc2_enabled ? 2u : 1u;
}

static inline bool linx_mmu_cache_entry_matches(const LinxMmuCacheEntry *entry,
                                                vaddr base, int mmu_idx,
                                                hwaddr size)
{
    return entry->valid &&
           entry->tag == (uint64_t)base &&
           entry->tlb_size == (uint64_t)size &&
           entry->mmu_idx == (uint8_t)mmu_idx;
}

static inline bool linx_mmu_cache_access_ok(int prot,
                                            MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_INST_FETCH:
        return (prot & PAGE_EXEC) != 0;
    case MMU_DATA_LOAD:
        return (prot & PAGE_READ) != 0;
    case MMU_DATA_STORE:
        return (prot & PAGE_WRITE) != 0;
    default:
        return false;
    }
}

static inline void linx_mmu_cache_count_size(hwaddr size, uint64_t *count_4k,
                                             uint64_t *count_2m,
                                             uint64_t *count_1g,
                                             uint64_t *count_512g)
{
    if (size == TARGET_PAGE_SIZE) {
        (*count_4k)++;
    } else if (size == ((hwaddr)1ull << 21)) {
        (*count_2m)++;
    } else if (size == ((hwaddr)1ull << 30)) {
        (*count_1g)++;
    } else if (size == ((hwaddr)1ull << 39)) {
        (*count_512g)++;
    }
}

static inline void linx_mmu_cache_count_hit(CPULinxState *env, hwaddr size)
{
    linx_mmu_cache_count_size(size, &env->mmu_cache_hit_4k,
                              &env->mmu_cache_hit_2m,
                              &env->mmu_cache_hit_1g,
                              &env->mmu_cache_hit_512g);
}

static inline void linx_mmu_cache_count_fill(CPULinxState *env, hwaddr size)
{
    linx_mmu_cache_count_size(size, &env->mmu_cache_fill_4k,
                              &env->mmu_cache_fill_2m,
                              &env->mmu_cache_fill_1g,
                              &env->mmu_cache_fill_512g);
}

static inline void linx_mmu_cache_count_collision(CPULinxState *env,
                                                  hwaddr size)
{
    linx_mmu_cache_count_size(size, &env->mmu_cache_collision_4k,
                              &env->mmu_cache_collision_2m,
                              &env->mmu_cache_collision_1g,
                              &env->mmu_cache_collision_512g);
}

static bool linx_mmu_cache_lookup(CPULinxState *env, vaddr va,
                                  MMUAccessType access_type, int mmu_idx,
                                  hwaddr *pa_out, int *prot_out,
                                  hwaddr *tlb_size_out, uint8_t *cause_out)
{
    static const hwaddr candidate_sizes[] = {
        TARGET_PAGE_SIZE,
        (hwaddr)1ull << 21,
        (hwaddr)1ull << 30,
        (hwaddr)1ull << 39,
    };

    if (!linx_mmu_cache_enabled_p()) {
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(candidate_sizes); i++) {
        const hwaddr size = candidate_sizes[i];
        const vaddr base = linx_mmu_cache_base(va, size);
        const size_t set_base = linx_mmu_cache_set_base(
            linx_mmu_cache_slot(base, mmu_idx, size));
        const size_t ways = linx_mmu_cache_way_count();

        for (size_t way = 0; way < ways; way++) {
            LinxMmuCacheEntry *entry = &env->mmu_cache[set_base + way];

            if (linx_mmu_cache_entry_matches(entry, base, mmu_idx, size) &&
                linx_mmu_cache_access_ok((int)entry->prot, access_type)) {
                *pa_out = (hwaddr)entry->pbase | (hwaddr)(va & (size - 1u));
                *prot_out = (int)entry->prot;
                *tlb_size_out = size;
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE,
                                                 linx_fault_acc(access_type));
                if (linx_mmu_cache_stats_enabled) {
                    env->mmu_cache_hits++;
                    linx_mmu_cache_count_hit(env, size);
                }
                return true;
            }
        }

        if (linx_mmu_cache_victim_enabled_p() &&
            linx_mmu_cache_entry_matches(&env->mmu_cache_victim, base,
                                         mmu_idx, size) &&
            linx_mmu_cache_access_ok((int)env->mmu_cache_victim.prot,
                                     access_type)) {
            LinxMmuCacheEntry *primary = &env->mmu_cache[set_base];
            LinxMmuCacheEntry tmp = *primary;

            *primary = env->mmu_cache_victim;
            env->mmu_cache_victim = tmp;
            *pa_out = (hwaddr)primary->pbase | (hwaddr)(va & (size - 1u));
            *prot_out = (int)primary->prot;
            *tlb_size_out = size;
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE,
                                             linx_fault_acc(access_type));
            if (linx_mmu_cache_stats_enabled) {
                env->mmu_cache_hits++;
                env->mmu_cache_victim_hits++;
                linx_mmu_cache_count_hit(env, size);
            }
            return true;
        }
    }

    if (linx_mmu_cache_stats_enabled) {
        env->mmu_cache_misses++;
    }
    return false;
}

static void linx_mmu_cache_store(CPULinxState *env, vaddr va,
                                 int mmu_idx, hwaddr pa, int prot,
                                 hwaddr tlb_size)
{
    if (!linx_mmu_cache_enabled_p()) {
        return;
    }

    const hwaddr size = linx_mmu_cache_size_or_page(tlb_size);
    const vaddr base = linx_mmu_cache_base(va, size);
    const size_t set_base = linx_mmu_cache_set_base(
        linx_mmu_cache_slot(base, mmu_idx, size));
    const size_t ways = linx_mmu_cache_way_count();
    LinxMmuCacheEntry *entry = NULL;

    for (size_t way = 0; way < ways; way++) {
        LinxMmuCacheEntry *candidate = &env->mmu_cache[set_base + way];

        if (linx_mmu_cache_entry_matches(candidate, base, mmu_idx, size)) {
            entry = candidate;
            break;
        }
        if (!candidate->valid && entry == NULL) {
            entry = candidate;
        }
    }

    if (entry == NULL) {
        if (linx_mmu_cache_assoc2_enabled) {
            const size_t set_index = set_base >> 1;
            const size_t victim_way = env->mmu_cache_next_way[set_index] & 1u;
            entry = &env->mmu_cache[set_base + victim_way];
            env->mmu_cache_next_way[set_index] = (uint8_t)(victim_way ^ 1u);
        } else {
            entry = &env->mmu_cache[set_base];
        }
    }

    const bool collision =
        entry->valid &&
        (entry->tag != (uint64_t)base ||
         entry->tlb_size != (uint64_t)size ||
         entry->mmu_idx != (uint8_t)mmu_idx);

    if (collision && linx_mmu_cache_victim_enabled_p()) {
        env->mmu_cache_victim = *entry;
        if (linx_mmu_cache_stats_enabled) {
            env->mmu_cache_victim_fills++;
        }
    }

    entry->tag = (uint64_t)base;
    entry->pbase = (uint64_t)(pa & ~(hwaddr)(size - 1u));
    entry->tlb_size = (uint64_t)size;
    entry->prot = (uint32_t)prot;
    entry->valid = 1;
    entry->mmu_idx = (uint8_t)mmu_idx;
    if (linx_mmu_cache_stats_enabled) {
        env->mmu_cache_fills++;
        linx_mmu_cache_count_fill(env, size);
        if (collision) {
            env->mmu_cache_collisions++;
            linx_mmu_cache_count_collision(env, size);
        }
    }
}

void linx_mmu_cache_flush(CPULinxState *env)
{
    if (!linx_mmu_cache_enabled_p()) {
        return;
    }

    memset(env->mmu_cache, 0, sizeof(env->mmu_cache));
    memset(env->mmu_cache_next_way, 0, sizeof(env->mmu_cache_next_way));
    memset(&env->mmu_cache_victim, 0, sizeof(env->mmu_cache_victim));
    if (linx_mmu_cache_stats_enabled) {
        env->mmu_cache_flushes++;
    }
}

void linx_mmu_cache_flush_page(CPULinxState *env, uint64_t addr)
{
    static const hwaddr candidate_sizes[] = {
        TARGET_PAGE_SIZE,
        (hwaddr)1ull << 21,
        (hwaddr)1ull << 30,
        (hwaddr)1ull << 39,
    };

    if (!linx_mmu_cache_enabled_p()) {
        return;
    }

    for (int mmu_idx = 0; mmu_idx < 2; mmu_idx++) {
        for (size_t i = 0; i < ARRAY_SIZE(candidate_sizes); i++) {
            const hwaddr size = candidate_sizes[i];
            const vaddr base = linx_mmu_cache_base((vaddr)addr, size);
            const size_t set_base = linx_mmu_cache_set_base(
                linx_mmu_cache_slot(base, mmu_idx, size));
            const size_t ways = linx_mmu_cache_way_count();

            for (size_t way = 0; way < ways; way++) {
                LinxMmuCacheEntry *entry = &env->mmu_cache[set_base + way];

                if (entry->valid &&
                    entry->tag == (uint64_t)base &&
                    entry->tlb_size == (uint64_t)size) {
                    entry->valid = 0;
                }
            }
            if (env->mmu_cache_victim.valid &&
                env->mmu_cache_victim.tag == (uint64_t)base &&
                env->mmu_cache_victim.tlb_size == (uint64_t)size) {
                env->mmu_cache_victim.valid = 0;
            }
        }
    }
    if (linx_mmu_cache_stats_enabled) {
        env->mmu_cache_page_flushes++;
    }
}

static inline void linx_irq_kick_if_allowed(CPUState *cs, CPULinxState *env,
                                            uint32_t dst_acr)
{
    if (env->ssr_acr[dst_acr][LINX_SSR_IPENDING] == 0) {
        return;
    }
    /*
     * Latch CPU_INTERRUPT_HARD whenever a source is pending.
     *
     * Permission checks (CSTATE.I / ring rules) run in cpu_exec_interrupt();
     * keeping the request latched prevents pending IRQ loss across ACR changes.
     *
     * cpu_interrupt() requires the BQL. The timer callback can run without the
     * BQL, so use the lock-free helper.
     */
    generic_handle_interrupt(cs, CPU_INTERRUPT_HARD);
}

static void linx_timer_cb(void *opaque)
{
    CPUState *cs = opaque;
    LinxCPU *cpu = LINX_CPU(cs);
    CPULinxState *env = &cpu->env;

    if (!linx_timer_irq_enabled()) {
        return;
    }

    /* Set pending bit and raise a hard interrupt. */
    const uint64_t before = env->ssr_acr[1][LINX_SSR_IPENDING];
    const uint64_t after = before | (1ull << LINX_IRQ_TIMER0);
    const uint64_t now = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    trace_linx_timer_fire(now, before, after);
    env->ssr_acr[1][LINX_SSR_IPENDING] = after;
    linx_irq_kick_if_allowed(cs, env, 1);
}

static hwaddr linx_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    LinxCPU *cpu = LINX_CPU(cs);
    CPULinxState *env = &cpu->env;
    const uint64_t tcr = env->ssr_acr[1][LINX_SSR_TCR];
    const uint64_t legacy_mmconfig = env->ssr_acr[1][LINX_SSR_TTBR1];
    const bool legacy_mmu =
        tcr == 0 && (legacy_mmconfig & LINX_LEGACY_MMCONFIG_ENABLE_BIT) != 0;
    const bool mme = (tcr & 1u) != 0;

    if (!legacy_mmu && !mme) {
        return linx_nommu_phys_addr(addr);
    }

    /* Debug translation: attempt a best-effort walk using the current ACR. */
    hwaddr phys = 0;
    int prot = 0;
    hwaddr tlb_size = TARGET_PAGE_SIZE;
    uint8_t cause = 0;
    const int mmu_idx = ((env->acr & 0xFu) == 2) ? 1 : 0;

    if (!linx_mmu_translate(cs, env, addr, MMU_DATA_LOAD, mmu_idx,
                            &phys, &prot, &tlb_size, &cause)) {
        return (hwaddr)-1;
    }

    return phys & TARGET_PAGE_MASK;
}

static void linx_cpu_do_interrupt(CPUState *cs);

static void linx_cpu_set_pc(CPUState *cs, vaddr value)
{
    LinxCPU *cpu = LINX_CPU(cs);
    cpu->env.pc = value;
}

static vaddr linx_cpu_get_pc(CPUState *cs)
{
    LinxCPU *cpu = LINX_CPU(cs);
    return cpu->env.pc;
}

static TCGTBCPUState linx_get_tb_cpu_state(CPUState *cs)
{
    CPULinxState *env = cpu_env(cs);
    uint32_t flags = 0;
    if (env->in_body) {
        flags |= LINX_TB_FLAG_IN_BODY;
    }
    if ((env->acr & 0xFu) == 2) {
        flags |= LINX_TB_FLAG_USER_MMU;
    }
    if (env->tb_dbg_active) {
        flags |= LINX_TB_FLAG_DBG_ACTIVE;
    }
    if (env->tb_cosim_precheck) {
        flags |= LINX_TB_FLAG_COSIM_PRECHECK;
    }
    return (TCGTBCPUState){ .pc = env->pc, .flags = flags };
}

static void linx_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    LinxCPU *cpu = LINX_CPU(cs);

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.pc = tb->pc;
}

static void linx_restore_state_to_opc(CPUState *cs,
                                      const TranslationBlock *tb,
                                      const uint64_t *data)
{
    LinxCPU *cpu = LINX_CPU(cs);
    cpu->env.pc = data[0];
}

static bool linx_cpu_has_work(CPUState *cs)
{
    /*
     * Linx currently has no WFI/idle instruction: if the CPU is not halted,
     * it always has work. If it is halted, only interrupts/reset should wake it.
     */
    if (!cs->halted) {
        return true;
    }
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_RESET);
}

static bool linx_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        CPULinxState *env = cpu_env(cs);

        /*
         * The hard-interrupt request bit can lag behind IPENDING updates.
         * Do not deliver EXCP_INTERRUPT without a live pending source.
         */
        if (env->ssr_acr[1][LINX_SSR_IPENDING] == 0) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
            return false;
        }

        /* Route all external interrupts to EXCP_INTERRUPT for now. */
        cs->exception_index = EXCP_INTERRUPT;
        if (!linx_irq_allowed(env, 1)) {
            /* Leave the interrupt request pending until it becomes allowed. */
            cs->exception_index = -1;
            return false;
        }
        linx_cpu_do_interrupt(cs);
        return true;
    }
    return false;
}

static inline uint64_t linx_pack_u16x3(uint64_t a, uint64_t b, uint64_t c)
{
    return ((a & 0xffffu) << 0) | ((b & 0xffffu) << 16) | ((c & 0xffffu) << 32);
}

static void linx_deliver_sync_trap(CPUState *cs, CPULinxState *env,
                                   uint64_t tpc, uint64_t tpc_next,
                                   uint8_t trapnum,
                                   bool argv, bool is_trap, bool bi)
{
    /*
     * Deliver a synchronous exception via the bring-up trap SSRs and EVBASE.
     *
     * Note: this is a simplified model that routes all synchronous exceptions
     * (except those from ACR0) to ACR1, matching the bring-up defaults.
     */
    const uint32_t src_acr = env->acr & 0xFu;
    const uint32_t dst_acr = (src_acr == 0) ? 0 : 1;

    /* Capture trapped-from state before switching to the managing ACR. */
    uint64_t src_cstate = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], src_acr);
    if (bi) {
        src_cstate |= LINX_ECSTATE_BI_BIT;
    } else {
        src_cstate &= ~LINX_ECSTATE_BI_BIT;
    }
    const uint64_t src_bpc = env->bpc;
    /*
     * Non-BI instruction/data exceptions are precise instruction faults.  A
     * data fault can happen after earlier instructions in the same block have
     * already updated architectural registers; returning to the block header
     * would replay those updates with mutated inputs. Resume these faults at
     * the faulting instruction itself.
     */
    const bool precise_user_exception =
        src_acr == 2 && !bi &&
        (trapnum == LINX_TRAPNUM_INSN_EXP ||
         trapnum == LINX_TRAPNUM_DATA_EXP);
    const uint64_t report_bpc = precise_user_exception ? tpc : src_bpc;
    trace_linx_deliver_sync_trap(trapnum, src_acr, dst_acr, tpc, report_bpc, src_cstate);
    if (linx_fault_trace_matches(env, trapnum, tpc, tpc_next, src_bpc,
                                 report_bpc)) {
        uint8_t trace_bytes[8] = {0};
        const int trace_bytes_rc =
            linx_read_insn_bytes(cs, tpc, trace_bytes, sizeof(trace_bytes));
        const int trace_mmu_idx = ((env->acr & 0xFu) == 2) ? 1 : 0;
        hwaddr trace_fetch_pa = 0;
        int trace_fetch_prot = 0;
        hwaddr trace_fetch_tlb_size = TARGET_PAGE_SIZE;
        uint8_t trace_fetch_cause = 0;
        const vaddr trace_mem_va =
            trapnum == LINX_TRAPNUM_DATA_EXP ?
            (vaddr)env->pending_trap_arg0 : (vaddr)tpc;
        const bool trace_fetch_ok =
            linx_mmu_translate(cs, env, (vaddr)tpc, MMU_INST_FETCH,
                               trace_mmu_idx, &trace_fetch_pa,
                               &trace_fetch_prot, &trace_fetch_tlb_size,
                               &trace_fetch_cause);
        hwaddr trace_store_pa = 0;
        int trace_store_prot = 0;
        hwaddr trace_store_tlb_size = TARGET_PAGE_SIZE;
        uint8_t trace_store_cause = 0;
        const bool trace_store_ok =
            linx_mmu_translate(cs, env, trace_mem_va, MMU_DATA_STORE,
                               trace_mmu_idx, &trace_store_pa,
                               &trace_store_prot, &trace_store_tlb_size,
                               &trace_store_cause);
        const LinxLegacyMmuProbe trace_fetch_probe =
            linx_probe_legacy_mmu(env, (vaddr)tpc, MMU_INST_FETCH,
                                  trace_mmu_idx);
        const LinxLegacyMmuProbe trace_store_probe =
            linx_probe_legacy_mmu(env, trace_mem_va, MMU_DATA_STORE,
                                  trace_mmu_idx);
        fprintf(stderr,
                "LINX_FAULT_TRACE count=%" PRIu64
                " trapnum=%u src_acr=%u dst_acr=%u argv=%u is_trap=%u"
                " bi=%u precise=%u"
                " tpc=0x%" PRIx64 " tpc_next=0x%" PRIx64
                " src_bpc=0x%" PRIx64 " report_bpc=0x%" PRIx64
                " envpc=0x%" PRIx64 " body_tpc=0x%" PRIx64
                " in_body=%u brtype=%u tgt=0x%" PRIx64
                " traparg0=0x%" PRIx64 " mem_va=0x%" VADDR_PRIx
                " cause=0x%x cstate=0x%" PRIx64
                " ra=0x%" PRIx64 " sp=0x%" PRIx64
                " tp=0x%" PRIx64
                " a0=0x%" PRIx64 " a1=0x%" PRIx64
                " a2=0x%" PRIx64 " a3=0x%" PRIx64
                " a4=0x%" PRIx64 " a5=0x%" PRIx64
                " a6=0x%" PRIx64 " a7=0x%" PRIx64
                " tq0=0x%" PRIx64 " tq1=0x%" PRIx64
                " tq2=0x%" PRIx64 " tq3=0x%" PRIx64
                " uq0=0x%" PRIx64 " uq1=0x%" PRIx64
                " uq2=0x%" PRIx64 " uq3=0x%" PRIx64
                " bytes_rc=%d bytes=%02x%02x%02x%02x%02x%02x%02x%02x"
                " fetch_ok=%u fetch_pa=0x%" HWADDR_PRIx
                " fetch_prot=0x%x fetch_tlb=0x%" HWADDR_PRIx
                " fetch_cause=0x%x"
                " store_ok=%u store_pa=0x%" HWADDR_PRIx
                " store_prot=0x%x store_tlb=0x%" HWADDR_PRIx
                " store_cause=0x%x"
                " legacy_fetch=%u:%u:%s:%u:0x%" HWADDR_PRIx
                ":0x%" PRIx64 ":0x%x:0x%" HWADDR_PRIx
                ":0x%" HWADDR_PRIx ":0x%x"
                " legacy_store=%u:%u:%s:%u:0x%" HWADDR_PRIx
                ":0x%" PRIx64 ":0x%x:0x%" HWADDR_PRIx
                ":0x%" HWADDR_PRIx ":0x%x\n",
                env->insn_count, trapnum, src_acr, dst_acr,
                argv ? 1u : 0u, is_trap ? 1u : 0u, bi ? 1u : 0u,
                precise_user_exception ? 1u : 0u, tpc, tpc_next,
                src_bpc, report_bpc, env->pc, env->body_tpc,
                env->in_body, env->brtype, env->tgt,
                env->pending_trap_arg0, trace_mem_va, env->pending_trap_cause,
                src_cstate, env->gpr[LINX_REG_RA], env->gpr[LINX_REG_SP],
                env->ssr[LINX_SSR_TP], env->gpr[LINX_REG_A0],
                env->gpr[LINX_REG_A1], env->gpr[LINX_REG_A2],
                env->gpr[LINX_REG_A3], env->gpr[LINX_REG_A4],
                env->gpr[LINX_REG_A5], env->gpr[LINX_REG_A6],
                env->gpr[LINX_REG_A7], env->tq[0], env->tq[1],
                env->tq[2], env->tq[3], env->uq[0], env->uq[1],
                env->uq[2], env->uq[3], trace_bytes_rc,
                trace_bytes[0], trace_bytes[1], trace_bytes[2],
                trace_bytes[3], trace_bytes[4], trace_bytes[5],
                trace_bytes[6], trace_bytes[7],
                trace_fetch_ok ? 1u : 0u, trace_fetch_pa, trace_fetch_prot,
                trace_fetch_tlb_size, trace_fetch_cause,
                trace_store_ok ? 1u : 0u, trace_store_pa, trace_store_prot,
                trace_store_tlb_size, trace_store_cause,
                trace_fetch_probe.legacy ? 1u : 0u,
                trace_fetch_probe.ok ? 1u : 0u,
                trace_fetch_probe.why ? trace_fetch_probe.why : "null",
                trace_fetch_probe.level, trace_fetch_probe.desc_addr,
                trace_fetch_probe.desc, trace_fetch_probe.prot,
                trace_fetch_probe.pa, trace_fetch_probe.block_size,
                trace_fetch_probe.cause,
                trace_store_probe.legacy ? 1u : 0u,
                trace_store_probe.ok ? 1u : 0u,
                trace_store_probe.why ? trace_store_probe.why : "null",
                trace_store_probe.level, trace_store_probe.desc_addr,
                trace_store_probe.desc, trace_store_probe.prot,
                trace_store_probe.pa, trace_store_probe.block_size,
                trace_store_probe.cause);
        linx_fault_trace_emit_regs(env, trapnum, tpc, report_bpc);
        fflush(stderr);
        linx_call_trace_dump_recent(env, "fault", tpc);
        linx_debug_pc_watch_dump_recent(env, "fault", tpc);
    }

    linx_acr_save_block_state(env, src_acr);
    if (src_acr == 2 && report_bpc == tpc &&
        linx_cpu_is_bstart_at_addr(cs, report_bpc)) {
        linx_acr_reset_block_state_for_header(env, src_acr, report_bpc);
    }
    const LinxAcrBlockState *src_state = &env->acr_block_state[src_acr];
    linx_acr_restore_block_state(env, dst_acr);

    const uint64_t evbase = env->ssr_acr[dst_acr][LINX_SSR_EVBASE];

    if (linx_trap_delivery_trace_matches(env, tpc, report_bpc)) {
        linx_trap_delivery_trace_emitted++;
        fprintf(stderr,
                "LINX_TRAP_DELIVERY_TRACE seq=%" PRIu64
                " count=%" PRIu64
                " trapnum=%u cause=0x%x argv=%u is_trap=%u bi=%u precise=%u"
                " src_acr=%u dst_acr=%u"
                " tpc=0x%" PRIx64 " tpc_next=0x%" PRIx64
                " src_bpc=0x%" PRIx64 " report_bpc=0x%" PRIx64
                " pending_arg0=0x%" PRIx64
                " pending_cause=0x%" PRIx32
                " envpc=0x%" PRIx64 " body_tpc=0x%" PRIx64
                " in_body=%u brtype=%u tgt=0x%" PRIx64
                " src_blocktype=%u"
                " src_tq0=0x%" PRIx64 " src_tq1=0x%" PRIx64
                " src_tq2=0x%" PRIx64 " src_tq3=0x%" PRIx64
                " src_uq0=0x%" PRIx64 " src_uq1=0x%" PRIx64
                " src_uq2=0x%" PRIx64 " src_uq3=0x%" PRIx64
                " src_lb=0x%" PRIx64 ":%" PRIx64 ":%" PRIx64
                " src_lc=0x%" PRIx64 ":%" PRIx64 ":%" PRIx64
                " dst_evbase=0x%" PRIx64
                " cstate=0x%" PRIx64
                " sp=0x%" PRIx64 " ra=0x%" PRIx64
                " a0=0x%" PRIx64 " a1=0x%" PRIx64
                " a2=0x%" PRIx64 " a3=0x%" PRIx64
                " a7=0x%" PRIx64 "\n",
                linx_trap_delivery_trace_emitted, env->insn_count,
                trapnum, env->pending_trap_cause, argv ? 1u : 0u,
                is_trap ? 1u : 0u, bi ? 1u : 0u,
                precise_user_exception ? 1u : 0u, src_acr, dst_acr,
                tpc, tpc_next, src_bpc, report_bpc,
                env->pending_trap_arg0, env->pending_trap_cause,
                env->pc, env->body_tpc, env->in_body, env->brtype,
                env->tgt, src_state->blocktype,
                src_state->tq[0], src_state->tq[1],
                src_state->tq[2], src_state->tq[3],
                src_state->uq[0], src_state->uq[1],
                src_state->uq[2], src_state->uq[3],
                src_state->lb[0], src_state->lb[1], src_state->lb[2],
                src_state->lc[0], src_state->lc[1], src_state->lc[2],
                evbase, src_cstate, env->gpr[LINX_REG_SP],
                env->gpr[LINX_REG_RA], env->gpr[LINX_REG_A0],
                env->gpr[LINX_REG_A1], env->gpr[LINX_REG_A2],
                env->gpr[LINX_REG_A3], env->gpr[LINX_REG_A7]);
        fflush(stderr);
    }

    env->ssr_acr[dst_acr][LINX_SSR_ECSTATE] = src_cstate;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG0] = (uint64_t)(src_state->blocktype & 0x1fu);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_CUR] = report_bpc;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_TGT] = tpc_next;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TPC] = is_trap ? tpc_next : tpc;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LRA] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ0] = src_state->tq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ1] = src_state->tq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ2] = src_state->tq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ3] = src_state->tq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ0] = src_state->uq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ1] = src_state->uq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ2] = src_state->uq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ3] = src_state->uq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LB] = linx_pack_u16x3(src_state->lb[0], src_state->lb[1], src_state->lb[2]);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LC] = linx_pack_u16x3(src_state->lc[0], src_state->lc[1], src_state->lc[2]);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_PTR] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_META] = 0;

    env->ssr_acr[dst_acr][LINX_SSR_TRAPNO] =
        linx_trapno_make(true, argv, env->pending_trap_cause, trapnum);
    env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = env->pending_trap_arg0;

    env->pending_trap_arg0 = 0;
    env->pending_trap_cause = 0;

    env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
    env->acr = dst_acr;
    linx_refresh_tb_dbg_active(env);
    if (src_acr == 2 && dst_acr == 1) {
        const uint64_t user_tp = env->ssr[LINX_SSR_TP];

        /*
         * Linux bring-up currently relies on exception entry finding the
         * current task pointer in live SSR_TP during the first kernel-origin
         * save block. Preserve the interrupted user TLS pointer in ETEMP0 so
         * the trap prologue can save it into pt_regs before ACRE restores it.
         */
        env->ssr[LINX_SSR_TP] = env->ssr_acr[dst_acr][LINX_SSR_ETEMP];
        env->ssr_acr[dst_acr][LINX_SSR_ETEMP0] = user_tp;
        linx_cpu_tp_trace_emit_handoff(env, "sync_user_to_kernel",
                                       src_acr, dst_acr, user_tp,
                                       env->ssr_acr[dst_acr][LINX_SSR_ETEMP]);
    } else if (src_acr == dst_acr) {
        linx_cpu_prepare_same_acr_exception_frame(env, "sync_same_acr_frame",
                                                  dst_acr);
    }
    env->ssr[LINX_SSR_CSTATE] =
        linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], dst_acr);
    env->pc = evbase ? evbase : tpc;
    if (src_acr == 2 && dst_acr == 1 && linx_cpu_dump_debug()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx user trap handoff: tpc=0x%" PRIx64
                      " next=0x%" PRIx64 " trapno=%u arg0=0x%" PRIx64
                      " etemp1=0x%" PRIx64 " tp=0x%" PRIx64
                      " acr=%u cstate=0x%" PRIx64 "\n",
                      tpc, tpc_next, trapnum, env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0],
                      env->ssr_acr[1][LINX_SSR_ETEMP], env->ssr[LINX_SSR_TP],
                      env->acr, env->ssr[LINX_SSR_CSTATE]);
    }
    cs->exception_index = -1;
    linx_dump_event_state(cs, "sync-delivered", trapnum);
}

static void linx_cpu_do_interrupt(CPUState *cs)
{
    CPULinxState *env = cpu_env(cs);
    int exception = cs->exception_index;
    uint64_t last_pc = env->pc;

    linx_dump_event_state(cs, "interrupt-entry", exception);

    qemu_log_mask(CPU_LOG_INT, "Linx: exception %d at PC=0x%" PRIx64 "\n",
                  exception, last_pc);

    switch (exception) {
    case 0:
        /* exception_index = 0 shouldn't happen - treat as invalid */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: BUG: exception_index is 0 (invalid) at PC=0x%" PRIx64 "\n",
                      last_pc);
        cs->exception_index = -1;
        cpu_abort(cs, "Linx: BUG: exception_index is 0");
        return;

    case LINX_EXCP_BREAKPOINT:
        /* Software breakpoint trap (EBREAK). */
        env->pending_trap_arg0 = last_pc;
        /* pending_trap_cause may carry the imm value (profile-defined). */
        linx_deliver_sync_trap(cs, env, last_pc,
                               linx_break_resume_pc(cs, env->insn_pc_next,
                                                    env->in_body != 0),
                               LINX_TRAPNUM_SW_BREAKPOINT,
                               true,  /* argv */
                               false, /* Linux do_trap_break() advances past EBREAK */
                               false  /* resume via BPC after skip_over_break() */
                               );
        return;

    case LINX_EXCP_BAD_BRANCH_TARGET:
    case LINX_EXCP_BLOCK_FAULT:
        if (exception == LINX_EXCP_BAD_BRANCH_TARGET) {
            /* v0.3: route via E_BLOCK(EC_CFI) (see E_BLOCK delivery path). */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: branch target violation at PC=0x%" PRIx64 "\n",
                          last_pc);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: block fault at PC=0x%" PRIx64 "\n",
                          last_pc);
        }
        /* v0.3: BI is determined by the E_BLOCK EC class. */
        {
            const uint8_t ec = linx_eblock_cause_ec(env->pending_trap_cause);
            const bool bi = (ec == LINX_EBLOCK_EC_BFETCH) ? true : false;
            linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                                   LINX_TRAPNUM_BLOCK_TRAP,
                                   true,  /* argv */
                                   false, /* fault */
                                   bi     /* BI */
                                   );
        }
        return;

    case LINX_EXCP_TILE_FAULT:
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_BLOCK_TRAP,
                               true,  /* argv: fault address/TPC */
                               false, /* fault */
                               (env->in_body != 0));
        return;

    case LINX_EXCP_ILLEGAL_INST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: illegal instruction at PC=0x%" PRIx64 "\n",
                      last_pc);
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_ILLEGAL_INST,
                               false, /* argv */
                               false, /* fault */
                               (env->in_body != 0) /* BI */
                               );
        return;

    case LINX_EXCP_HW_BREAKPOINT:
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_HW_BREAKPOINT,
                               true,  /* argv */
                               true,  /* trap */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_HW_WATCHPOINT:
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_HW_WATCHPOINT,
                               true,  /* argv */
                               true,  /* trap */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_EXEC_STATE_CHECK:
        linx_deliver_sync_trap(cs, env, last_pc, env->insn_pc_next,
                               LINX_TRAPNUM_EXEC_STATE_CHECK,
                               false, /* argv */
                               false, /* fault */
                               true   /* BI */
                               );
        return;

    case LINX_EXCP_INST_ACCESS_FAULT:
    case LINX_EXCP_LOAD_ACCESS_FAULT:
    case LINX_EXCP_STORE_ACCESS_FAULT:
    {
        uint64_t fault_pc = last_pc;
        const bool in_body = env->in_body != 0;
        const bool user_data_fault =
            (env->acr & 0xFu) == 2 &&
            exception != LINX_EXCP_INST_ACCESS_FAULT;
        uint64_t user_fault_bpc = env->bpc;
        uint64_t user_fault_reported_pc = last_pc;
        if (user_data_fault && env->insn_pc_next > 0) {
            /*
             * Scalar block execution keeps env->pc at the block header while
             * individual memory ops advance insn_pc_next.  Use the byte before
             * insn_pc_next to recover the actual faulting payload instruction;
             * last_pc may name an older header after a fixed-target branch.
             */
            user_fault_reported_pc = env->insn_pc_next - 1;
        }
        const bool user_valid_block_fault =
            user_data_fault &&
            linx_resolve_user_fault_block(cs, env->bpc, user_fault_reported_pc,
                                          &user_fault_bpc, &fault_pc) &&
            user_fault_bpc != fault_pc;
        if (user_valid_block_fault) {
            env->bpc = user_fault_bpc;
        }
        const bool user_mid_block_data_fault =
            user_valid_block_fault && user_fault_bpc != fault_pc;
        const bool bi = in_body || user_mid_block_data_fault;
        const uint8_t trapnum =
            (exception == LINX_EXCP_INST_ACCESS_FAULT && !in_body)
                ? LINX_TRAPNUM_INSN_EXP
                : LINX_TRAPNUM_DATA_EXP;
        /*
         * User page faults inside an open scalar block must return through
         * RRAT_RESTORE: the resume PC is the faulting TPC, but the block BPC
         * and SETC/branch metadata must remain the original header state.
         */
        linx_deliver_sync_trap(cs, env, fault_pc, env->insn_pc_next,
                               trapnum,
                               true,  /* argv (TRAPARG0=fault VA) */
                               false, /* fault */
                               bi     /* BI */
                               );
        return;
    }

    case EXCP_INTERRUPT:
    {
        /*
         * Hardware interrupt (bring-up): asynchronous interrupt routed to ACR1.
         *
         * v0.2 resume contract:
         * - Preserve BPC as the interrupted block start marker.
         * - Resume from TPC (BI=1) for normal instruction streams.
         * - For in-flight restartable templates, resume from template PC.
         */
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);

        const uint32_t src_acr = env->acr & 0xFu;
        const uint32_t dst_acr = 1;
        /*
         * Asynchronous IRQs are taken at instruction boundaries, so env->pc is
         * already the architectural resume address. Using insn_pc_next here can
         * skip an instruction if the interrupt is recognized between TBs.
         */
        const uint64_t resume_pc = env->pc;
        uint64_t resume_bpc = env->bpc ? env->bpc : resume_pc;
        /*
         * A timer IRQ can be recognized after a scalar branch has staged the
         * next BSTART PC while env->bpc still names the previous block.  Linux
         * returns through EBARG_BPC_CUR, so preserve a self-consistent user
         * BSTART boundary instead of resuming with pc and bpc in different
         * scalar blocks.
         */
        const bool user_bstart_resume =
            src_acr == 2 && linx_cpu_is_bstart_at_addr(cs, resume_pc);
        if (user_bstart_resume) {
            resume_bpc = resume_pc;
        }

        uint64_t src_cstate = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], src_acr);
        src_cstate |= LINX_ECSTATE_BI_BIT;

        linx_acr_save_block_state(env, src_acr);
        if (user_bstart_resume) {
            linx_acr_reset_block_state_for_header(env, src_acr, resume_bpc);
        }
        const LinxAcrBlockState *src_state = &env->acr_block_state[src_acr];
        linx_acr_restore_block_state(env, dst_acr);

        const uint64_t evbase = env->ssr_acr[dst_acr][LINX_SSR_EVBASE];

        if (src_acr == dst_acr) {
            const uint32_t depth_before = env->ebarg_stack_depth;
            if (linx_ebarg_stack_push(env, dst_acr)) {
                trace_linx_ebarg_stack_push(dst_acr, depth_before,
                                            env->ebarg_stack_depth);
            } else if (depth_before >= LINX_EBARG_STACK_DEPTH) {
                trace_linx_ebarg_stack_overflow(dst_acr, depth_before);
            }
        }

        /* Save interrupt source state into managing ACR bank. */
        env->ssr_acr[dst_acr][LINX_SSR_ECSTATE] = src_cstate;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG0] = (uint64_t)(src_state->blocktype & 0x1fu);
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_CUR] = resume_bpc;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_TGT] = resume_pc;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TPC] = resume_pc;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_LRA] = 0;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ0] = src_state->tq[0];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ1] = src_state->tq[1];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ2] = src_state->tq[2];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ3] = src_state->tq[3];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ0] = src_state->uq[0];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ1] = src_state->uq[1];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ2] = src_state->uq[2];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ3] = src_state->uq[3];
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_LB] = linx_pack_u16x3(src_state->lb[0], src_state->lb[1], src_state->lb[2]);
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_LC] = linx_pack_u16x3(src_state->lc[0], src_state->lc[1], src_state->lc[2]);
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_PTR] = 0;
        env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_META] = 0;

        /* Find an IRQ ID from IPENDING (simple bitmap model). */
        uint32_t irq_id = LINX_IRQ_TIMER0;
        {
            const uint64_t ip = env->ssr_acr[dst_acr][LINX_SSR_IPENDING];
            if (ip) {
                irq_id = (uint32_t)ctz64(ip);
            }
        }

        trace_linx_deliver_async_irq(src_acr, dst_acr, irq_id, 1u,
                                     resume_pc, resume_bpc, resume_pc,
                                     evbase, src_cstate,
                                     env->ssr_acr[dst_acr][LINX_SSR_IPENDING]);

        env->ssr_acr[dst_acr][LINX_SSR_TRAPNO] =
            linx_trapno_make(false, true, 0, LINX_TRAPNUM_INTERRUPT);
        env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = (uint64_t)irq_id;

        /*
         * Linux entry expects live SSR_TP to name thread_info for user-origin
         * traps.  Synchronous traps and service requests already perform this
         * handoff; asynchronous interrupts need the same TLS save so the
         * from_user prologue can switch to the kernel stack.
         */
        if (src_acr == 2 && dst_acr == 1) {
            const uint64_t user_tp = env->ssr[LINX_SSR_TP];
            const uint64_t thread_info = env->ssr_acr[dst_acr][LINX_SSR_ETEMP];

            env->ssr[LINX_SSR_TP] = thread_info;
            env->ssr_acr[dst_acr][LINX_SSR_ETEMP0] = user_tp;
            linx_cpu_tp_trace_emit_handoff(env, "irq_user_to_kernel",
                                           src_acr, dst_acr, user_tp,
                                           thread_info);
        } else if (src_acr == dst_acr) {
            linx_cpu_prepare_same_acr_exception_frame(env,
                                                      "irq_same_acr_frame",
                                                      dst_acr);
        }

        /* Switch to managing ring and vector. */
        env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
        env->acr = dst_acr;
        linx_refresh_tb_dbg_active(env);
        env->ssr[LINX_SSR_CSTATE] = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], dst_acr);
        env->pc = evbase ? evbase : last_pc;
        cs->exception_index = -1;
        linx_dump_event_state(cs, "irq-delivered", exception);
        return;
    }

    default:
        /* Check if it's a generic QEMU exception that we should handle */
        if (exception >= 0 && exception < 0x100) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: unhandled exception %d at PC=0x%" PRIx64 "\n",
                          exception, last_pc);
            cs->exception_index = -1;
            cpu_abort(cs, "Linx: Unhandled exception");
            return;
        } else if (exception < 0) {
            /* Negative exception_index means no exception */
            cs->exception_index = -1;
            return;
        } else {
            /* Unrecognized exception >= 0x100 */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: unrecognized exception %d at PC=0x%" PRIx64 "\n",
                          exception, last_pc);
            cs->exception_index = -1;
            cpu_set_interrupt(cs, CPU_INTERRUPT_EXITTB);
            return;
        }
    }
}

#if TARGET_LONG_BITS == 64
static vaddr linx_pointer_wrap(CPUState *cs, int mmu_idx, vaddr result, vaddr base)
{
    /* 64-bit addresses don't wrap */
    return result;
}
#endif

static int linx_cpu_mmu_index(CPUState *cs, bool ifunc)
{
    CPULinxState *env = cpu_env(cs);
    return ((env->acr & 0xFu) == 2) ? 1 : 0;
}

static inline bool linx_va_is_canonical(vaddr va)
{
    const uint64_t top = ((uint64_t)va >> 48) & 0xffffu;
    const uint64_t sign = ((uint64_t)va >> 47) & 1u;
    return top == (sign ? 0xffffu : 0x0000u);
}

static inline bool linx_va_is_canonical_width(vaddr va, unsigned bits)
{
    const uint64_t raw = (uint64_t)va;
    const uint64_t sign = (raw >> (bits - 1u)) & 1u;
    const uint64_t upper = raw >> bits;
    const uint64_t expect = sign ? (~UINT64_C(0) >> bits) : 0;
    return upper == expect;
}

static inline uint8_t linx_fault_acc(MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_INST_FETCH:
        return LINX_TRAPCAUSE_ACC_INST;
    case MMU_DATA_STORE:
        return LINX_TRAPCAUSE_ACC_STORE;
    case MMU_DATA_LOAD:
    default:
        return LINX_TRAPCAUSE_ACC_LOAD;
    }
}

static const char *linx_mmu_access_name(MMUAccessType access_type)
{
    switch (access_type) {
    case MMU_INST_FETCH:
        return "fetch";
    case MMU_DATA_STORE:
        return "store";
    case MMU_DATA_LOAD:
        return "load";
    default:
        return "unknown";
    }
}

static LinxLegacyMmuProbe linx_probe_legacy_mmu(CPULinxState *env, vaddr va,
                                                MMUAccessType access_type,
                                                int mmu_idx)
{
    LinxLegacyMmuProbe probe = {
        .level = 0,
        .cause = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF,
                                     linx_fault_acc(access_type)),
        .why = "not-legacy",
    };
    const uint64_t tcr = env->ssr_acr[1][LINX_SSR_TCR];
    const uint64_t legacy_mmconfig = env->ssr_acr[1][LINX_SSR_TTBR1];
    const bool legacy_mmu =
        tcr == 0 && (legacy_mmconfig & LINX_LEGACY_MMCONFIG_ENABLE_BIT) != 0;
    const uint8_t acc = linx_fault_acc(access_type);

    if (!legacy_mmu) {
        return probe;
    }

    probe.legacy = true;
    const bool qpte = (legacy_mmconfig & LINX_LEGACY_MMCONFIG_Q_BIT) != 0;
    const unsigned mode =
        (unsigned)(legacy_mmconfig & LINX_LEGACY_MMCONFIG_MODE_MASK);
    const unsigned levels = 3u + mode;
    const unsigned va_bits = qpte ? (36u + mode * 8u) : (39u + mode * 9u);

    if (qpte || levels < 3u || levels > 5u ||
        !linx_va_is_canonical_width(va, va_bits)) {
        probe.why = "bad-config-or-canonical";
        return probe;
    }

    hwaddr table =
        (hwaddr)LINX_LEGACY_MMTBASE_TO_PA(env->ssr_acr[1][LINX_SSR_TTBR0]);
    for (unsigned level = 0; level < levels; level++) {
        const unsigned shift = 12u + 9u * (levels - 1u - level);
        const uint64_t idx = (((uint64_t)va) >> shift) & 0x1ffu;
        const hwaddr desc_addr = table + (hwaddr)(idx * 8u);
        MemTxResult result = MEMTX_OK;
        const uint64_t desc =
            address_space_ldq_le(&address_space_memory, desc_addr,
                                 MEMTXATTRS_UNSPECIFIED, &result);

        probe.level = level;
        probe.desc_addr = desc_addr;
        probe.desc = desc;
        if (result != MEMTX_OK) {
            probe.why = "desc-read";
            return probe;
        }

        if ((desc & LINX_LEGACY_PTE_V) == 0) {
            probe.why = "type0";
            return probe;
        }

        const bool is_leaf = (desc & LINX_LEGACY_PTE_LEAF_MASK) != 0;
        if (!is_leaf) {
            if ((desc & 0xffeULL) != 0) {
                probe.why = "bad-table-desc";
                return probe;
            }
            table = (hwaddr)(((uint64_t)(desc >> 32)) << 12);
            continue;
        }

        if (level == 0) {
            probe.why = "leaf-at-l0";
            return probe;
        }

        const hwaddr block_size = (hwaddr)1ull << shift;
        const hwaddr out_base = (hwaddr)(((uint64_t)(desc >> 32)) << 12);
        const bool u = (desc & LINX_LEGACY_PTE_U) != 0;
        const bool x = (desc & LINX_LEGACY_PTE_X) != 0;
        const bool w = (desc & LINX_LEGACY_PTE_W) != 0;
        const bool r = (desc & LINX_LEGACY_PTE_R) != 0;

        probe.block_size = block_size;
        if (r) {
            probe.prot |= PAGE_READ;
        }
        if (w) {
            probe.prot |= PAGE_WRITE;
        }
        if (x) {
            probe.prot |= PAGE_EXEC;
        }

        if ((out_base & (block_size - 1u)) != 0) {
            probe.why = "bad-leaf-desc";
            return probe;
        }

        if ((mmu_idx == 1 && !u) ||
            (access_type == MMU_INST_FETCH && !x) ||
            (access_type == MMU_DATA_LOAD && !r) ||
            (access_type == MMU_DATA_STORE && !w)) {
            probe.cause = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM,
                                              acc);
            probe.why = "perm";
            return probe;
        }

        probe.pa =
            out_base | (hwaddr)((uint64_t)va & (uint64_t)(block_size - 1u));
        if (((uint64_t)probe.pa >> 48) != 0) {
            probe.why = "pa-range";
            return probe;
        }

        probe.ok = true;
        probe.cause = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE, acc);
        probe.why = "ok";
        return probe;
    }

    probe.why = "walk-fell-through";
    return probe;
}

static void linx_tlb_fill_trace_init(void)
{
    if (linx_tlb_fill_trace_inited) {
        return;
    }

    linx_tlb_fill_trace_enabled =
        linx_cpu_env_enabled("LINX_TLB_FILL_TRACE") ||
        linx_cpu_env_enabled("LINX_QEMU_TLB_FILL_TRACE");

    uint64_t lo = 0;
    uint64_t hi = UINT64_MAX;
    const char *lo_s =
        linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_VA_LO",
                              "LINX_QEMU_TLB_FILL_TRACE_VA_LO");
    const char *hi_s =
        linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_VA_HI",
                              "LINX_QEMU_TLB_FILL_TRACE_VA_HI");
    const char *va_s =
        linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_VA",
                              "LINX_QEMU_TLB_FILL_TRACE_VA");
    if (va_s && linx_cpu_parse_u64(va_s, &lo)) {
        hi = lo;
        linx_tlb_fill_trace_va_filter_enabled = true;
    } else {
        const bool have_lo = lo_s && linx_cpu_parse_u64(lo_s, &lo);
        const bool have_hi = hi_s && linx_cpu_parse_u64(hi_s, &hi);
        if (have_lo || have_hi) {
            linx_tlb_fill_trace_va_filter_enabled = true;
        }
    }
    if (linx_tlb_fill_trace_va_filter_enabled) {
        linx_tlb_fill_trace_va_lo = MIN(lo, hi);
        linx_tlb_fill_trace_va_hi = MAX(lo, hi);
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_PC_LO",
                                 "LINX_QEMU_TLB_FILL_TRACE_PC_LO");
    hi_s = linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_PC_HI",
                                 "LINX_QEMU_TLB_FILL_TRACE_PC_HI");
    const bool have_pc_lo = lo_s && linx_cpu_parse_u64(lo_s, &lo);
    const bool have_pc_hi = hi_s && linx_cpu_parse_u64(hi_s, &hi);
    if (have_pc_lo || have_pc_hi) {
        linx_tlb_fill_trace_pc_lo = MIN(lo, hi);
        linx_tlb_fill_trace_pc_hi = MAX(lo, hi);
        linx_tlb_fill_trace_pc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_COUNT_LO",
                                 "LINX_QEMU_TLB_FILL_TRACE_COUNT_LO");
    hi_s = linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_COUNT_HI",
                                 "LINX_QEMU_TLB_FILL_TRACE_COUNT_HI");
    const bool have_count_lo = lo_s && linx_cpu_parse_u64(lo_s, &lo);
    const bool have_count_hi = hi_s && linx_cpu_parse_u64(hi_s, &hi);
    if (have_count_lo || have_count_hi) {
        linx_tlb_fill_trace_count_lo = MIN(lo, hi);
        linx_tlb_fill_trace_count_hi = MAX(lo, hi);
        linx_tlb_fill_trace_count_filter_enabled = true;
    }

    const char *limit_s =
        linx_cpu_env_nonzero2("LINX_TLB_FILL_TRACE_LIMIT",
                              "LINX_QEMU_TLB_FILL_TRACE_LIMIT");
    if (limit_s) {
        (void)linx_cpu_parse_u64(limit_s, &linx_tlb_fill_trace_limit);
    }

    linx_tlb_fill_trace_inited = true;
}

static inline bool linx_tlb_fill_trace_enabled_p(void)
{
    linx_tlb_fill_trace_init();
    return linx_tlb_fill_trace_enabled;
}

static bool linx_tlb_fill_trace_addr_matches(uint64_t addr)
{
    return addr >= linx_tlb_fill_trace_va_lo &&
           addr <= linx_tlb_fill_trace_va_hi;
}

static bool linx_tlb_fill_trace_pc_matches(CPULinxState *env)
{
    return !linx_tlb_fill_trace_pc_filter_enabled ||
           (env->pc >= linx_tlb_fill_trace_pc_lo &&
            env->pc <= linx_tlb_fill_trace_pc_hi) ||
           (env->bpc >= linx_tlb_fill_trace_pc_lo &&
            env->bpc <= linx_tlb_fill_trace_pc_hi) ||
           (env->body_tpc >= linx_tlb_fill_trace_pc_lo &&
            env->body_tpc <= linx_tlb_fill_trace_pc_hi);
}

static bool linx_tlb_fill_trace_matches(CPULinxState *env, vaddr addr)
{
    if (!linx_tlb_fill_trace_enabled_p()) {
        return false;
    }
    if (linx_tlb_fill_trace_limit != 0 &&
        linx_tlb_fill_trace_emitted >= linx_tlb_fill_trace_limit) {
        return false;
    }
    if (linx_tlb_fill_trace_count_filter_enabled &&
        (env->insn_count < linx_tlb_fill_trace_count_lo ||
         env->insn_count > linx_tlb_fill_trace_count_hi)) {
        return false;
    }
    if (linx_tlb_fill_trace_va_filter_enabled &&
        !linx_tlb_fill_trace_addr_matches((uint64_t)addr)) {
        return false;
    }
    return linx_tlb_fill_trace_pc_matches(env);
}

static void linx_tlb_fill_trace_emit(CPULinxState *env, vaddr addr, int size,
                                     MMUAccessType access_type, int mmu_idx,
                                     bool probe, bool ok, hwaddr pa, int prot,
                                     hwaddr tlb_size, uint8_t cause)
{
    if (!linx_tlb_fill_trace_matches(env, addr)) {
        return;
    }

    linx_tlb_fill_trace_emitted++;
    const LinxLegacyMmuProbe legacy =
        linx_probe_legacy_mmu(env, addr, access_type, mmu_idx);
    fprintf(stderr,
            "LINX_TLB_FILL_TRACE count=%" PRIu64
            " emitted=%" PRIu64
            " ok=%u access=%s access_id=%d size=%d"
            " va=0x%" VADDR_PRIx
            " pa=0x%" HWADDR_PRIx
            " prot=0x%x tlb_size=0x%" HWADDR_PRIx
            " cause=0x%x mmu=%d probe=%d"
            " pc=0x%" PRIx64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " envpc=0x%" PRIx64
            " acr=%u cstate=0x%" PRIx64
            " tcr=0x%" PRIx64
            " ttbr0=0x%" PRIx64
            " ttbr1=0x%" PRIx64
            " legacy=%u legacy_ok=%u legacy_why=%s"
            " legacy_level=%u legacy_desc_addr=0x%" HWADDR_PRIx
            " legacy_desc=0x%" PRIx64
            " legacy_prot=0x%x legacy_pa=0x%" HWADDR_PRIx
            " legacy_block=0x%" HWADDR_PRIx
            " legacy_cause=0x%x\n",
            env->insn_count, linx_tlb_fill_trace_emitted,
            ok ? 1u : 0u, linx_mmu_access_name(access_type), access_type,
            size, addr, pa, prot, tlb_size, cause, mmu_idx, probe ? 1 : 0,
            env->pc, env->bpc, env->body_tpc, env->pc,
            env->acr & 0xFu, env->ssr[LINX_SSR_CSTATE],
            env->ssr_acr[1][LINX_SSR_TCR],
            env->ssr_acr[1][LINX_SSR_TTBR0],
            env->ssr_acr[1][LINX_SSR_TTBR1],
            legacy.legacy ? 1u : 0u, legacy.ok ? 1u : 0u,
            legacy.why ? legacy.why : "null", legacy.level,
            legacy.desc_addr, legacy.desc, legacy.prot, legacy.pa,
            legacy.block_size, legacy.cause);
    fflush(stderr);
}

static inline bool linx_tlb_fill_stats_enabled_p(void)
{
    if (!linx_tlb_fill_stats_inited) {
        linx_tlb_fill_stats_enabled =
            linx_cpu_env_nonzero2("LINX_TLB_FILL_STATS",
                                  "LINX_QEMU_TLB_FILL_STATS") != NULL;
        linx_tlb_fill_stats_inited = true;
    }
    return linx_tlb_fill_stats_enabled;
}

static inline bool linx_tlb_fill_hot_enabled_p(void)
{
    if (!linx_tlb_fill_hot_inited) {
        linx_tlb_fill_hot_enabled =
            linx_cpu_env_nonzero2("LINX_TLB_FILL_HOT",
                                  "LINX_QEMU_TLB_FILL_HOT") != NULL;
        linx_tlb_fill_hot_inited = true;
    }
    return linx_tlb_fill_hot_enabled;
}

static inline bool linx_tlb_fill_stats_any_enabled_p(void)
{
    const bool stats_enabled = linx_tlb_fill_stats_enabled_p();
    const bool hot_enabled = linx_tlb_fill_hot_enabled_p();

    return stats_enabled || hot_enabled;
}

static void linx_tlb_fill_hot_record(CPULinxState *env, vaddr addr,
                                     MMUAccessType access_type, int mmu_idx,
                                     bool probe, hwaddr pa, int prot,
                                     uint8_t cause)
{
    const uint64_t page = ((uint64_t)addr) & TARGET_PAGE_MASK;
    int found = -1;
    int empty = -1;
    int min_slot = 0;
    int slot = -1;
    uint64_t min_count = UINT64_MAX;
    const uint8_t last_slot = env->tlb_fill_hot_last_slot;

    env->tlb_fill_hot_active = 1;
    if (last_slot < LINX_TLB_FILL_HOT_SLOTS &&
        env->tlb_fill_hot_valid[last_slot] &&
        env->tlb_fill_hot_page[last_slot] == page &&
        env->tlb_fill_hot_access[last_slot] == (uint8_t)access_type &&
        env->tlb_fill_hot_mmu[last_slot] == (uint8_t)mmu_idx &&
        env->tlb_fill_hot_probe[last_slot] == (probe ? 1u : 0u)) {
        found = (int)last_slot;
        env->tlb_fill_hot_last_hits++;
        goto update_slot;
    }

    for (unsigned i = 0; i < LINX_TLB_FILL_HOT_SLOTS; i++) {
        if (!env->tlb_fill_hot_valid[i]) {
            if (empty < 0) {
                empty = (int)i;
            }
            continue;
        }
        if (env->tlb_fill_hot_page[i] == page &&
            env->tlb_fill_hot_access[i] == (uint8_t)access_type &&
            env->tlb_fill_hot_mmu[i] == (uint8_t)mmu_idx &&
            env->tlb_fill_hot_probe[i] == (probe ? 1u : 0u)) {
            found = (int)i;
            env->tlb_fill_hot_slot_hits++;
            break;
        }
        if (env->tlb_fill_hot_count[i] < min_count) {
            min_count = env->tlb_fill_hot_count[i];
            min_slot = (int)i;
        }
    }

    slot = found;
    if (slot < 0) {
        slot = empty >= 0 ? empty : min_slot;
        env->tlb_fill_hot_inserts++;
        if (empty < 0) {
            env->tlb_fill_hot_evictions++;
        }
        env->tlb_fill_hot_valid[slot] = 1;
        env->tlb_fill_hot_count[slot] = 0;
        env->tlb_fill_hot_page[slot] = page;
        env->tlb_fill_hot_access[slot] = (uint8_t)access_type;
        env->tlb_fill_hot_mmu[slot] = (uint8_t)mmu_idx;
        env->tlb_fill_hot_probe[slot] = probe ? 1u : 0u;
    }

update_slot:
    env->tlb_fill_hot_last_slot = (uint8_t)slot;
    env->tlb_fill_hot_count[slot]++;
    env->tlb_fill_hot_acr[slot] = env->acr & 0xFu;
    env->tlb_fill_hot_prot[slot] = (uint32_t)prot;
    env->tlb_fill_hot_cause[slot] = cause;
    env->tlb_fill_hot_last_va[slot] = (uint64_t)addr;
    env->tlb_fill_hot_last_pa[slot] = (uint64_t)pa;
    env->tlb_fill_hot_last_pc[slot] = env->pc;
    env->tlb_fill_hot_last_bpc[slot] = env->bpc;
}

static inline void linx_tlb_fill_stats_record(CPULinxState *env, vaddr addr,
                                              MMUAccessType access_type,
                                              int mmu_idx, bool probe, bool ok,
                                              hwaddr pa, int prot,
                                              uint8_t cause)
{
    const bool stats_enabled = linx_tlb_fill_stats_enabled;
    const bool hot_enabled = linx_tlb_fill_hot_enabled;

    if (!stats_enabled && !hot_enabled) {
        return;
    }

    if (hot_enabled) {
        linx_tlb_fill_hot_record(env, addr, access_type, mmu_idx, probe,
                                 pa, prot, cause);
    }

    if (!stats_enabled) {
        return;
    }

    env->tlb_fill_total++;
    switch (access_type) {
    case MMU_INST_FETCH:
        env->tlb_fill_fetch++;
        break;
    case MMU_DATA_LOAD:
        env->tlb_fill_load++;
        break;
    case MMU_DATA_STORE:
        env->tlb_fill_store++;
        break;
    }
    if (probe) {
        env->tlb_fill_probe++;
    }
    if (ok) {
        env->tlb_fill_ok++;
    } else {
        env->tlb_fill_fault++;
    }
    if (mmu_idx == 1) {
        env->tlb_fill_user++;
        switch (access_type) {
        case MMU_INST_FETCH:
            env->tlb_fill_user_fetch++;
            break;
        case MMU_DATA_LOAD:
            env->tlb_fill_user_load++;
            break;
        case MMU_DATA_STORE:
            env->tlb_fill_user_store++;
            break;
        }
    } else if (mmu_idx == 0) {
        env->tlb_fill_kernel++;
        switch (access_type) {
        case MMU_INST_FETCH:
            env->tlb_fill_kernel_fetch++;
            break;
        case MMU_DATA_LOAD:
            env->tlb_fill_kernel_load++;
            break;
        case MMU_DATA_STORE:
            env->tlb_fill_kernel_store++;
            break;
        }
    } else {
        env->tlb_fill_other++;
    }

    env->tlb_fill_last_count = env->insn_count;
    env->tlb_fill_last_pc = env->pc;
    env->tlb_fill_last_bpc = env->bpc;
    env->tlb_fill_last_va = (uint64_t)addr;
    env->tlb_fill_last_pa = (uint64_t)pa;
    env->tlb_fill_last_access = (uint32_t)access_type;
    env->tlb_fill_last_mmu_idx = (uint32_t)mmu_idx;
    env->tlb_fill_last_prot = (uint32_t)prot;
    env->tlb_fill_last_cause = cause;
    env->tlb_fill_last_acr = env->acr & 0xFu;
}

static inline bool linx_debug_fetch_va(vaddr va)
{
    const uint64_t low = ((uint64_t)va) & 0xfffffu;

    return low < 0x2000u || low == 0x10bc0u;
}

static void linx_debug_log_fetch_bytes(const char *tag, vaddr va, hwaddr pa,
                                       int prot, int mmu_idx, bool probe)
{
    uint8_t buf[8] = {0};
    MemTxResult result;

    result = address_space_read(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx %s: va=0x%" VADDR_PRIx " pa=0x%" HWADDR_PRIx
                  " prot=0x%x mmu=%d probe=%d phys_read=%d"
                  " bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                  tag, va, pa, prot, mmu_idx, probe ? 1 : 0, (int)result,
                  buf[0], buf[1], buf[2], buf[3],
                  buf[4], buf[5], buf[6], buf[7]);
}

static bool linx_mmu_translate(CPUState *cs, CPULinxState *env, vaddr va,
                               MMUAccessType access_type, int mmu_idx,
                               hwaddr *pa_out, int *prot_out,
                               hwaddr *tlb_size_out, uint8_t *cause_out)
{
    (void)cs;
#define LEGACY_FETCH_FAULT(_why, _addr, _level, _desc)                                 \
    do {                                                                                \
        if (access_type == MMU_INST_FETCH &&                                            \
            (linx_cpu_dump_debug() || linx_cpu_dump_on_event()))                        \
            qemu_log_mask(LOG_GUEST_ERROR,                                              \
                          "Linx legacy MMU fetch fault: %s va=0x%" VADDR_PRIx           \
                          " level=%u addr=0x%" HWADDR_PRIx " desc=0x%" PRIx64           \
                          " ttbr0=0x%" PRIx64 " mmconfig=0x%" PRIx64 "\n",              \
                          _why, va, (unsigned)(_level), (hwaddr)(_addr),                \
                          (uint64_t)(_desc), env->ssr_acr[1][LINX_SSR_TTBR0],           \
                          legacy_mmconfig);                                             \
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);              \
        return false;                                                                   \
    } while (0)
    const uint64_t tcr = env->ssr_acr[1][LINX_SSR_TCR];
    const uint64_t legacy_mmconfig = env->ssr_acr[1][LINX_SSR_TTBR1];
    const bool legacy_mmu =
        tcr == 0 && (legacy_mmconfig & LINX_LEGACY_MMCONFIG_ENABLE_BIT) != 0;
    const bool mme = (tcr & 1u) != 0;
    const uint8_t acc = linx_fault_acc(access_type);

    if (!legacy_mmu && !mme) {
        /* Identity mapping for NOMMU / MME=0. */
        *pa_out = linx_nommu_phys_addr(va);
        *prot_out = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        *tlb_size_out = TARGET_PAGE_SIZE;
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE, acc);
        return true;
    }

    if (linx_mmu_cache_lookup(env, va, access_type, mmu_idx,
                              pa_out, prot_out, tlb_size_out, cause_out)) {
        return true;
    }

    if (legacy_mmu) {
        const bool qpte = (legacy_mmconfig & LINX_LEGACY_MMCONFIG_Q_BIT) != 0;
        const unsigned mode = (unsigned)(legacy_mmconfig & LINX_LEGACY_MMCONFIG_MODE_MASK);
        const unsigned levels = 3u + mode;
        const unsigned va_bits = qpte ? (36u + mode * 8u) : (39u + mode * 9u);
        if (access_type == MMU_INST_FETCH &&
            (linx_cpu_dump_debug() || linx_cpu_dump_on_event()) &&
            mmu_idx == 1 &&
            linx_debug_fetch_va(va)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx legacy MMU fetch probe: va=0x%" VADDR_PRIx
                          " ttbr0=0x%" PRIx64 " mmconfig=0x%" PRIx64 "\n",
                          va, env->ssr_acr[1][LINX_SSR_TTBR0], legacy_mmconfig);
        }
        if (qpte || levels < 3u || levels > 5u ||
            !linx_va_is_canonical_width(va, va_bits)) {
            LEGACY_FETCH_FAULT("bad-config-or-canonical", 0, 0, 0);
        }

        hwaddr table = (hwaddr)LINX_LEGACY_MMTBASE_TO_PA(env->ssr_acr[1][LINX_SSR_TTBR0]);
        for (unsigned level = 0; level < levels; level++) {
            const unsigned shift = 12u + 9u * (levels - 1u - level);
            const uint64_t idx = (((uint64_t)va) >> shift) & 0x1ffu;
            const hwaddr desc_addr = table + (hwaddr)(idx * 8u);
            MemTxResult result = MEMTX_OK;
            const uint64_t desc =
                address_space_ldq_le(&address_space_memory, desc_addr,
                                     MEMTXATTRS_UNSPECIFIED, &result);
            if (result != MEMTX_OK) {
                LEGACY_FETCH_FAULT("desc-read", desc_addr, level, 0);
            }

            if ((desc & LINX_LEGACY_PTE_V) == 0) {
                LEGACY_FETCH_FAULT("type0", desc_addr, level, desc);
            }

            const bool is_leaf = (desc & LINX_LEGACY_PTE_LEAF_MASK) != 0;
            if (!is_leaf) {
                /*
                 * Legacy Linx table descriptors encode the next-level PFN in
                 * desc[63:32], matching the Linux port's _PAGE_PFN_SHIFT=32
                 * layout. Only the permission/attribute bits below PFN are
                 * reserved for non-leaf entries.
                 */
                if ((desc & 0xffeULL) != 0) {
                    LEGACY_FETCH_FAULT("bad-table-desc", desc_addr, level, desc);
                }
                table = (hwaddr)(((uint64_t)(desc >> 32)) << 12);
                continue;
            }

            if (level == 0) {
                LEGACY_FETCH_FAULT("leaf-at-l0", desc_addr, level, desc);
            }

            const hwaddr block_size = (hwaddr)1ull << shift;
            const hwaddr out_base = (hwaddr)(((uint64_t)(desc >> 32)) << 12);
            const bool u = (desc & LINX_LEGACY_PTE_U) != 0;
            const bool x = (desc & LINX_LEGACY_PTE_X) != 0;
            const bool w = (desc & LINX_LEGACY_PTE_W) != 0;
            const bool r = (desc & LINX_LEGACY_PTE_R) != 0;
            if ((out_base & (block_size - 1u)) != 0) {
                LEGACY_FETCH_FAULT("bad-leaf-desc", desc_addr, level, desc);
            }
            if ((mmu_idx == 1 && !u) ||
                (access_type == MMU_INST_FETCH && !x) ||
                (access_type == MMU_DATA_LOAD && !r) ||
                (access_type == MMU_DATA_STORE && !w)) {
                if (access_type == MMU_INST_FETCH &&
                    (linx_cpu_dump_debug() || linx_cpu_dump_on_event())) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "Linx legacy MMU perm fault: va=0x%" VADDR_PRIx
                                  " level=%u desc=0x%" PRIx64 " mmu=%d u=%d r=%d w=%d x=%d\n",
                                  va, level, desc, mmu_idx, u ? 1 : 0, r ? 1 : 0,
                                  w ? 1 : 0, x ? 1 : 0);
                }
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
                return false;
            }

            const hwaddr pa =
                out_base | (hwaddr)((uint64_t)va & (uint64_t)(block_size - 1u));
            if (((uint64_t)pa >> 48) != 0) {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }

            int prot = 0;
            if (r) prot |= PAGE_READ;
            if (w) prot |= PAGE_WRITE;
            if (x) prot |= PAGE_EXEC;
            if (access_type == MMU_INST_FETCH &&
                (linx_cpu_dump_debug() || linx_cpu_dump_on_event()) &&
                mmu_idx == 1 &&
                linx_debug_fetch_va(va)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Linx legacy MMU fetch ok: va=0x%" VADDR_PRIx
                              " level=%u pa=0x%" HWADDR_PRIx " desc=0x%" PRIx64
                              " prot=0x%x\n",
                              va, level, pa, desc, prot);
            }
            *pa_out = pa;
            *prot_out = prot;
            *tlb_size_out = block_size;
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE, acc);
            linx_mmu_cache_store(env, va, mmu_idx, pa, prot, block_size);
            return true;
        }

        LEGACY_FETCH_FAULT("walk-fell-through", 0, levels, 0);
    }

    if (!linx_va_is_canonical(va)) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    /* v0.2 bring-up profile: only 48-bit VA supported (T0SZ/T1SZ must be 16). */
    const uint32_t t0sz = (uint32_t)((tcr >> 1) & 0x3fu);
    const uint32_t t1sz = (uint32_t)((tcr >> 7) & 0x3fu);
    if (t0sz != 16 || t1sz != 16) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    const uint32_t epd0 = (uint32_t)((tcr >> 13) & 1u);
    const uint32_t epd1 = (uint32_t)((tcr >> 14) & 1u);
    const bool use_ttbr1 = (((uint64_t)va >> 47) & 1u) != 0;

    if (!use_ttbr1 && epd0) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }
    if (use_ttbr1 && epd1) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    const uint64_t ttbr = use_ttbr1 ? env->ssr_acr[1][LINX_SSR_TTBR1]
                                    : env->ssr_acr[1][LINX_SSR_TTBR0];
    if ((ttbr & 0xfffu) != 0) {
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
        return false;
    }

    hwaddr table = (hwaddr)(ttbr & 0x0000fffffffff000ULL);

    /* Walk L0..L3. */
    for (int level = 0; level < 4; level++) {
        const uint32_t shift = 39u - (uint32_t)level * 9u;
        const uint64_t idx = (((uint64_t)va) >> shift) & 0x1ffu;
        const hwaddr desc_addr = table + (hwaddr)(idx * 8u);
        MemTxResult result = MEMTX_OK;
        const uint64_t desc = address_space_ldq_le(&address_space_memory, desc_addr,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const uint32_t type = (uint32_t)(desc & 0x3u);
        if (type == 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        if (type == 3) {
            /* Table descriptor: Desc[1:0]=11. */
            if ((desc & 0xffcULL) != 0) {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
            if ((desc >> 48) != 0) {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
            table = (hwaddr)(desc & 0x0000fffffffff000ULL);
            continue;
        }

        /* Leaf descriptor: Page at L3, Block at L1/L2 (optional). */
        if (level == 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        hwaddr block_size = TARGET_PAGE_SIZE;
        if (type == 2) {
            if (level == 1) {
                block_size = (hwaddr)1ull << 30; /* 1 GiB */
            } else if (level == 2) {
                block_size = (hwaddr)1ull << 21; /* 2 MiB */
            } else {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
        } else if (type == 1) {
            if (level != 3) {
                *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
                return false;
            }
        } else {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const hwaddr out_base = (hwaddr)(desc & 0x0000fffffffff000ULL);
        if ((desc >> 48) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }
        if ((out_base & (block_size - 1u)) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        /* Reserved bits for leaf descriptors. */
        if ((desc & (3ull << 10)) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const uint32_t attridx = (uint32_t)((desc >> 7) & 0x7u);
        if (attridx > 2u) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const bool af = ((desc >> 6) & 1u) != 0;
        if (!af) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        const bool u = ((desc >> 5) & 1u) != 0;
        const bool x = ((desc >> 4) & 1u) != 0;
        const bool w = ((desc >> 3) & 1u) != 0;
        const bool r = ((desc >> 2) & 1u) != 0;

        if (mmu_idx == 1 && !u) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }
        if (access_type == MMU_INST_FETCH && !x) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }
        if (access_type == MMU_DATA_LOAD && !r) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }
        if (access_type == MMU_DATA_STORE && !w) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PERM, acc);
            return false;
        }

        const hwaddr pa = out_base | (hwaddr)((uint64_t)va & (uint64_t)(block_size - 1u));
        if (((uint64_t)pa >> 48) != 0) {
            *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
            return false;
        }

        int prot = 0;
        if (r) {
            prot |= PAGE_READ;
        }
        if (w) {
            prot |= PAGE_WRITE;
        }
        if (x) {
            prot |= PAGE_EXEC;
        }

        *pa_out = pa;
        *prot_out = prot;
        *tlb_size_out = block_size;
        *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_NONE, acc);
        linx_mmu_cache_store(env, va, mmu_idx, pa, prot, block_size);
        return true;
    }

    *cause_out = linx_trapcause_make(LINX_TRAPCAUSE_CAT_MMU_PF, acc);
    return false;
#undef LEGACY_FETCH_FAULT
}

static bool linx_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    /*
     * NOMMU uses identity translation, with a compatibility fold for
     * sign-extended legacy 29-bit physical addresses.
     */
    CPULinxState *env = cpu_env(cs);
    hwaddr pa = 0;
    int prot = 0;
    hwaddr tlb_size = TARGET_PAGE_SIZE;
    uint8_t cause = 0;
    const bool fill_stats_enabled =
        unlikely(linx_tlb_fill_stats_any_enabled_p());
    const bool fill_trace_enabled =
        unlikely(linx_tlb_fill_trace_enabled_p());

    trace_linx_tlb_fill(addr, access_type, mmu_idx, probe ? 1 : 0,
                        env->ssr_acr[1][LINX_SSR_TCR], env->acr & 0xFu);

    if (linx_mmu_translate(cs, env, addr, access_type, mmu_idx,
                           &pa, &prot, &tlb_size, &cause)) {
        if (fill_stats_enabled) {
            linx_tlb_fill_stats_record(env, addr, access_type, mmu_idx, probe,
                                       true, pa, prot, cause);
        }
        if (fill_trace_enabled) {
            linx_tlb_fill_trace_emit(env, addr, size, access_type, mmu_idx,
                                     probe, true, pa, prot, tlb_size, cause);
        }
        if (access_type == MMU_INST_FETCH &&
            (linx_cpu_dump_debug() || linx_cpu_dump_on_event()) &&
            mmu_idx == 1 &&
            linx_debug_fetch_va(addr)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx tlb fill ok: va=0x%" VADDR_PRIx " pa=0x%" HWADDR_PRIx
                          " prot=0x%x tlb_size=0x%" HWADDR_PRIx " mmu=%d probe=%d\n",
                          addr, pa, prot, tlb_size, mmu_idx, probe ? 1 : 0);
            linx_debug_log_fetch_bytes("tlb fill fetch bytes", addr, pa, prot,
                                       mmu_idx, probe);
        }
        if (linx_cpu_dump_debug() &&
            access_type == MMU_INST_FETCH &&
            (addr & ~(vaddr)(TARGET_PAGE_SIZE - 1u)) == linx_debug_jiffy_sched_clock_page) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: tlb ok va=0x%" VADDR_PRIx " pa=0x%" HWADDR_PRIx
                          " prot=0x%x tlb_size=0x%" HWADDR_PRIx " mmu=%d probe=%d\n",
                          addr, pa, prot, tlb_size, mmu_idx, probe ? 1 : 0);
        }
        /*
         * Bring-up: map only TARGET_PAGE_SIZE granularity in the softmmu TLB,
         * even when the page table descriptor is a larger block mapping.
         *
         * This avoids relying on large-page TLB support while the Linx MMU
         * model is still stabilizing.
         */
        hwaddr map_size = tlb_size;
        if (map_size > TARGET_PAGE_SIZE) {
            map_size = TARGET_PAGE_SIZE;
        }
        vaddr vbase = addr & ~(vaddr)(map_size - 1u);
        hwaddr pbase = pa & ~(hwaddr)(map_size - 1u);
        trace_linx_tlb_fill_ok(addr, pa, map_size, prot);
        tlb_set_page(cs, vbase, pbase, prot, mmu_idx, map_size);
        trace_linx_tlb_fill_set(vbase, pbase, map_size);
        return true;
    }

    if (fill_stats_enabled) {
        linx_tlb_fill_stats_record(env, addr, access_type, mmu_idx, probe,
                                   false, pa, prot, cause);
    }
    if (fill_trace_enabled) {
        linx_tlb_fill_trace_emit(env, addr, size, access_type, mmu_idx,
                                 probe, false, pa, prot, tlb_size, cause);
    }
    if (probe) {
        return false;
    }

    if (linx_fault_trace_tlb_matches(env, (uint64_t)addr)) {
        const LinxLegacyMmuProbe legacy =
            linx_probe_legacy_mmu(env, addr, access_type, mmu_idx);
        fprintf(stderr,
                "LINX_TLB_FAULT_TRACE count=%" PRIu64
                " pc=0x%" PRIx64 " va=0x%" VADDR_PRIx
                " access=%d cause=0x%x mmu=%d probe=%d"
                " acr=%u bpc=0x%" PRIx64
                " body_tpc=0x%" PRIx64 " insn_next=0x%" PRIx64
                " in_body=%u brtype=%u tgt=0x%" PRIx64
                " cstate=0x%" PRIx64
                " tcr=0x%" PRIx64 " ttbr0=0x%" PRIx64
                " ttbr1=0x%" PRIx64
                " legacy=%u legacy_ok=%u legacy_why=%s"
                " legacy_level=%u legacy_desc_addr=0x%" HWADDR_PRIx
                " legacy_desc=0x%" PRIx64
                " legacy_prot=0x%x legacy_pa=0x%" HWADDR_PRIx
                " legacy_block=0x%" HWADDR_PRIx
                " legacy_cause=0x%x\n",
                env->insn_count, env->pc, addr, access_type, cause, mmu_idx,
                probe ? 1 : 0, (unsigned)(env->acr & 0xFu), env->bpc,
                env->body_tpc, env->insn_pc_next, env->in_body,
                env->brtype, env->tgt, env->ssr[LINX_SSR_CSTATE],
                env->ssr_acr[1][LINX_SSR_TCR],
                env->ssr_acr[1][LINX_SSR_TTBR0],
                env->ssr_acr[1][LINX_SSR_TTBR1],
                legacy.legacy ? 1u : 0u, legacy.ok ? 1u : 0u,
                legacy.why ? legacy.why : "null", legacy.level,
                legacy.desc_addr, legacy.desc, legacy.prot, legacy.pa,
                legacy.block_size, legacy.cause);
        fflush(stderr);
    }

    if (linx_cpu_dump_debug() || linx_cpu_dump_on_event()) {
        qemu_fprintf(stderr,
                     "Linx: tlb miss/fault pc=0x%" PRIx64
                     " va=0x%" VADDR_PRIx " access=%d cause=0x%x mmu=%d probe=%d"
                     " tp=0x%" PRIx64 " sp=0x%" PRIx64
                     " ra=0x%" PRIx64 " a0=0x%" PRIx64
                     " a1=0x%" PRIx64 " a2=0x%" PRIx64
                     " a3=0x%" PRIx64 " a4=0x%" PRIx64
                     " a5=0x%" PRIx64 " a6=0x%" PRIx64
                     " a7=0x%" PRIx64
                     " tcr=0x%" PRIx64 " ttbr0=0x%" PRIx64
                     " ttbr1=0x%" PRIx64 "\n",
                     env->pc, addr, access_type, cause, mmu_idx, probe ? 1 : 0,
                     env->ssr[LINX_SSR_TP],
                     env->gpr[LINX_REG_SP],
                     env->gpr[LINX_REG_RA],
                     env->gpr[LINX_REG_A0],
                     env->gpr[LINX_REG_A1],
                     env->gpr[LINX_REG_A2],
                     env->gpr[LINX_REG_A3],
                     env->gpr[LINX_REG_A4],
                     env->gpr[LINX_REG_A5],
                     env->gpr[LINX_REG_A6],
                     env->gpr[LINX_REG_A7],
                     env->ssr_acr[1][LINX_SSR_TCR],
                     env->ssr_acr[1][LINX_SSR_TTBR0],
                     env->ssr_acr[1][LINX_SSR_TTBR1]);
        fflush(stderr);
    } else if (env->pc >= 0xffffffff80010bc0ULL &&
               env->pc < 0xffffffff80010c40ULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: tlb miss/fault pc=0x%" PRIx64
                      " va=0x%" VADDR_PRIx " access=%d cause=0x%x mmu=%d probe=%d"
                      " tp=0x%" PRIx64 " sp=0x%" PRIx64
                      " ra=0x%" PRIx64 " a0=0x%" PRIx64
                      " a1=0x%" PRIx64 "\n",
                      env->pc, addr, access_type, cause, mmu_idx, probe ? 1 : 0,
                      env->ssr[LINX_SSR_TP],
                      env->gpr[LINX_REG_SP],
                      env->gpr[LINX_REG_RA],
                      env->gpr[LINX_REG_A0],
                      env->gpr[LINX_REG_A1]);
    }

    env->pending_trap_arg0 = (uint64_t)addr;
    env->pending_trap_cause = (uint32_t)cause;

    switch (access_type) {
    case MMU_INST_FETCH:
        cs->exception_index = LINX_EXCP_INST_ACCESS_FAULT;
        break;
    case MMU_DATA_STORE:
        cs->exception_index = LINX_EXCP_STORE_ACCESS_FAULT;
        break;
    case MMU_DATA_LOAD:
    default:
        cs->exception_index = LINX_EXCP_LOAD_ACCESS_FAULT;
        break;
    }

    cpu_loop_exit_restore(cs, retaddr);
}

static void linx_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPULinxState *env = cpu_env(cs);
    const uint32_t cur_acr = env->acr & 0xFu;
    const uint32_t mgr_acr = linx_managing_acr(cur_acr);
    int i;

    qemu_fprintf(f,
                 "pc=0x%016" PRIx64 " brtype=%u carg=0x%08x cond=%u vec_p=%" PRIu64 " tgt=0x%016" PRIx64
                 " fcsr=0x%08x\n",
                 env->pc, env->brtype, env->carg, env->cond, env->vec_p,
                 env->tgt, env->fcsr);
    for (i = 0; i < LINX_GPR_COUNT; i += 4) {
        qemu_fprintf(f,
                     "r%-2d=0x%016" PRIx64 " r%-2d=0x%016" PRIx64
                     " r%-2d=0x%016" PRIx64 " r%-2d=0x%016" PRIx64 "\n",
                     i, env->gpr[i], i + 1, env->gpr[i + 1],
                     i + 2, env->gpr[i + 2], i + 3, env->gpr[i + 3]);
    }

    if (!linx_cpu_dump_debug()) {
        return;
    }

    qemu_fprintf(f,
                 "debug: LINX_CPU_DUMP_DEBUG=1 (flags=0x%x) cstate=0x%016" PRIx64
                 " acr=%u mgr_acr=%u fcsr=0x%08x\n",
                 flags, env->ssr[LINX_SSR_CSTATE], cur_acr, mgr_acr, env->fcsr);
    qemu_fprintf(f,
                 "debug.bstate.live: blocktype=%u brtype=%u carg=0x%08x cond=%u vec_p=%" PRIu64
                 " tgt=0x%016" PRIx64 " bpc=0x%016" PRIx64
                 " in_body=%u body_tpc=0x%016" PRIx64
                 " return_pc=0x%016" PRIx64 "\n",
                 env->blocktype, env->brtype, env->carg, env->cond, env->vec_p,
                 env->tgt,
                 env->bpc, env->in_body, env->body_tpc, env->return_pc);
    qemu_fprintf(f,
                 "debug.bstate.live.lb=[0x%016" PRIx64 ",0x%016" PRIx64
                 ",0x%016" PRIx64 "] lc=[0x%016" PRIx64 ",0x%016" PRIx64
                 ",0x%016" PRIx64 "]\n",
                 env->lb[0], env->lb[1], env->lb[2],
                 env->lc[0], env->lc[1], env->lc[2]);

    {
        const hwaddr sp = (hwaddr)env->gpr[LINX_REG_SP];
        MemTxResult result = MEMTX_OK;
        const uint64_t w0 = address_space_ldq_le(&address_space_memory, sp + 0,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w1 = address_space_ldq_le(&address_space_memory, sp + 8,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w2 = address_space_ldq_le(&address_space_memory, sp + 16,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w3 = address_space_ldq_le(&address_space_memory, sp + 24,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w4 = address_space_ldq_le(&address_space_memory, sp + 32,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w5 = address_space_ldq_le(&address_space_memory, sp + 40,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w6 = address_space_ldq_le(&address_space_memory, sp + 48,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w7 = address_space_ldq_le(&address_space_memory, sp + 56,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w8 = address_space_ldq_le(&address_space_memory, sp + 64,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w9 = address_space_ldq_le(&address_space_memory, sp + 72,
                                                 MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w10 = address_space_ldq_le(&address_space_memory, sp + 80,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w11 = address_space_ldq_le(&address_space_memory, sp + 88,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w12 = address_space_ldq_le(&address_space_memory, sp + 96,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w13 = address_space_ldq_le(&address_space_memory, sp + 104,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w14 = address_space_ldq_le(&address_space_memory, sp + 112,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w15 = address_space_ldq_le(&address_space_memory, sp + 120,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w16 = address_space_ldq_le(&address_space_memory, sp + 128,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w17 = address_space_ldq_le(&address_space_memory, sp + 136,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w18 = address_space_ldq_le(&address_space_memory, sp + 144,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w19 = address_space_ldq_le(&address_space_memory, sp + 152,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w20 = address_space_ldq_le(&address_space_memory, sp + 160,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w21 = address_space_ldq_le(&address_space_memory, sp + 168,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w22 = address_space_ldq_le(&address_space_memory, sp + 176,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w23 = address_space_ldq_le(&address_space_memory, sp + 184,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w24 = address_space_ldq_le(&address_space_memory, sp + 192,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w25 = address_space_ldq_le(&address_space_memory, sp + 200,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w26 = address_space_ldq_le(&address_space_memory, sp + 208,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w27 = address_space_ldq_le(&address_space_memory, sp + 216,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w28 = address_space_ldq_le(&address_space_memory, sp + 224,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w29 = address_space_ldq_le(&address_space_memory, sp + 232,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w30 = address_space_ldq_le(&address_space_memory, sp + 240,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w31 = address_space_ldq_le(&address_space_memory, sp + 248,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w32 = address_space_ldq_le(&address_space_memory, sp + 256,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w33 = address_space_ldq_le(&address_space_memory, sp + 264,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w34 = address_space_ldq_le(&address_space_memory, sp + 272,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w35 = address_space_ldq_le(&address_space_memory, sp + 280,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w36 = address_space_ldq_le(&address_space_memory, sp + 288,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w37 = address_space_ldq_le(&address_space_memory, sp + 296,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w38 = address_space_ldq_le(&address_space_memory, sp + 304,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w39 = address_space_ldq_le(&address_space_memory, sp + 312,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w40 = address_space_ldq_le(&address_space_memory, sp + 320,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w41 = address_space_ldq_le(&address_space_memory, sp + 328,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w42 = address_space_ldq_le(&address_space_memory, sp + 336,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w43 = address_space_ldq_le(&address_space_memory, sp + 344,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w44 = address_space_ldq_le(&address_space_memory, sp + 352,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w45 = address_space_ldq_le(&address_space_memory, sp + 360,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w46 = address_space_ldq_le(&address_space_memory, sp + 368,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w47 = address_space_ldq_le(&address_space_memory, sp + 376,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w48 = address_space_ldq_le(&address_space_memory, sp + 384,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w49 = address_space_ldq_le(&address_space_memory, sp + 392,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w50 = address_space_ldq_le(&address_space_memory, sp + 400,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w51 = address_space_ldq_le(&address_space_memory, sp + 408,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w52 = address_space_ldq_le(&address_space_memory, sp + 416,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w53 = address_space_ldq_le(&address_space_memory, sp + 424,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w54 = address_space_ldq_le(&address_space_memory, sp + 432,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w55 = address_space_ldq_le(&address_space_memory, sp + 440,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w56 = address_space_ldq_le(&address_space_memory, sp + 448,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w57 = address_space_ldq_le(&address_space_memory, sp + 456,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w93 = address_space_ldq_le(&address_space_memory, sp + 744,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w94 = address_space_ldq_le(&address_space_memory, sp + 752,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w95 = address_space_ldq_le(&address_space_memory, sp + 760,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w96 = address_space_ldq_le(&address_space_memory, sp + 768,
                                                  MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w143 = address_space_ldq_le(&address_space_memory, sp + 1144,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w144 = address_space_ldq_le(&address_space_memory, sp + 1152,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w145 = address_space_ldq_le(&address_space_memory, sp + 1160,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w146 = address_space_ldq_le(&address_space_memory, sp + 1168,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w151 = address_space_ldq_le(&address_space_memory, sp + 1208,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w152 = address_space_ldq_le(&address_space_memory, sp + 1216,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w157 = address_space_ldq_le(&address_space_memory, sp + 1256,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w158 = address_space_ldq_le(&address_space_memory, sp + 1260,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w159 = address_space_ldq_le(&address_space_memory, sp + 1264,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        const uint64_t w188 = address_space_ldq_le(&address_space_memory, sp + 1512,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        qemu_fprintf(f,
                     "debug.stack sp=0x%016" PRIx64
                     " [0]=0x%016" PRIx64
                     " [8]=0x%016" PRIx64
                     " [16]=0x%016" PRIx64
                     " [24]=0x%016" PRIx64
                     " [32]=0x%016" PRIx64
                     " [40]=0x%016" PRIx64
                     " [48]=0x%016" PRIx64
                     " [56]=0x%016" PRIx64
                     " [64]=0x%016" PRIx64
                     " [72]=0x%016" PRIx64
                     " [80]=0x%016" PRIx64
                     " [88]=0x%016" PRIx64
                     " [96]=0x%016" PRIx64
                     " [104]=0x%016" PRIx64
                     " [112]=0x%016" PRIx64
                     " [120]=0x%016" PRIx64
                     " [128]=0x%016" PRIx64
                     " [136]=0x%016" PRIx64
                     " [144]=0x%016" PRIx64
                     " [152]=0x%016" PRIx64
                     " [160]=0x%016" PRIx64
                     " [168]=0x%016" PRIx64
                     " [176]=0x%016" PRIx64
                     " [184]=0x%016" PRIx64
                     " [192]=0x%016" PRIx64
                     " [200]=0x%016" PRIx64
                     " [208]=0x%016" PRIx64
                     " [216]=0x%016" PRIx64
                     " [224]=0x%016" PRIx64
                     " [232]=0x%016" PRIx64
                     " [240]=0x%016" PRIx64
                     " [248]=0x%016" PRIx64
                     " [256]=0x%016" PRIx64
                     " [264]=0x%016" PRIx64
                     " [272]=0x%016" PRIx64
                     " [280]=0x%016" PRIx64
                     " [288]=0x%016" PRIx64
                     " [296]=0x%016" PRIx64
                     " [304]=0x%016" PRIx64
                     " [312]=0x%016" PRIx64
                     " [320]=0x%016" PRIx64
                     " [328]=0x%016" PRIx64
                     " [336]=0x%016" PRIx64
                     " [344]=0x%016" PRIx64
                     " [352]=0x%016" PRIx64
                     " [360]=0x%016" PRIx64
                     " [368]=0x%016" PRIx64
                     " [376]=0x%016" PRIx64
                     " [384]=0x%016" PRIx64
                     " [392]=0x%016" PRIx64
                     " [400]=0x%016" PRIx64
                     " [408]=0x%016" PRIx64
                     " [416]=0x%016" PRIx64
                     " [424]=0x%016" PRIx64
                     " [432]=0x%016" PRIx64
                     " [440]=0x%016" PRIx64
                     " [448]=0x%016" PRIx64
                     " [456]=0x%016" PRIx64
                     " [744]=0x%016" PRIx64
                     " [752]=0x%016" PRIx64
                     " [760]=0x%016" PRIx64
                     " [768]=0x%016" PRIx64
                     " [1144]=0x%016" PRIx64
                     " [1152]=0x%016" PRIx64
                     " [1160]=0x%016" PRIx64
                     " [1168]=0x%016" PRIx64
                     " [1208]=0x%016" PRIx64
                     " [1216]=0x%016" PRIx64
                     " [1256]=0x%016" PRIx64
                     " [1260]=0x%016" PRIx64
                     " [1264]=0x%016" PRIx64
                     " [1512]=0x%016" PRIx64 "\n",
                     env->gpr[LINX_REG_SP], w0, w1, w2, w3, w4, w5, w6, w7,
                     w8, w9, w10, w11, w12, w13, w14, w15,
                     w16, w17, w18, w19, w20, w21, w22, w23,
                     w24, w25, w26, w27, w28, w29, w30, w31,
                     w32, w33, w34, w35, w36, w37, w38, w39,
                     w40, w41, w42, w43, w44, w45, w46, w47,
                     w48, w49, w50, w51, w52, w53, w54, w55,
                     w56, w57, w93, w94, w95, w96,
                     w143, w144, w145, w146, w151, w152,
                     w157, w158, w159, w188);
    }

    {
        const hwaddr mem_map_sym = UINT64_C(0xffffffff81109be8);
        MemTxResult result = MEMTX_OK;
        const uint64_t mem_map_val =
            address_space_ldq_le(&address_space_memory, mem_map_sym,
                                 MEMTXATTRS_UNSPECIFIED, &result);
        qemu_fprintf(f,
                     "debug.globals mem_map@0x%016" PRIx64 "=0x%016" PRIx64 "\n",
                     (uint64_t)mem_map_sym, mem_map_val);
    }

    linx_dump_q4(f, "debug.queue.tq", env->tq, "t#1", "t#4", "T-hand");
    linx_dump_q4(f, "debug.queue.uq", env->uq, "u#1", "u#4", "U-hand");

    linx_dump_ebarg_bank(f, env, mgr_acr, "debug.ebarg.manager");
    if (mgr_acr != cur_acr) {
        linx_dump_ebarg_bank(f, env, cur_acr, "debug.ebarg.current");
    }

    linx_dump_debug_ssr_bank(f, env, mgr_acr, "debug.hwdbg.manager");
    if (mgr_acr != cur_acr) {
        linx_dump_debug_ssr_bank(f, env, cur_acr, "debug.hwdbg.current");
    }

    linx_dump_bstate_snapshot(f, "debug.bstate.snapshot.current",
                              cur_acr, &env->acr_block_state[cur_acr]);
    if (mgr_acr != cur_acr) {
        linx_dump_bstate_snapshot(f, "debug.bstate.snapshot.manager",
                                  mgr_acr, &env->acr_block_state[mgr_acr]);
    }
}

void linx_core4_reset(LinxCore4State *core4)
{
    qemu_mutex_lock(&core4->lock);
    memset(core4->shared_tile, 0, sizeof(core4->shared_tile));
    core4->collective_bpc = 0;
    core4->collective_func = 0;
    core4->collective_dtype = 0;
    core4->collective_shared_id = 0;
    core4->collective_m = 0;
    core4->collective_n = 0;
    core4->collective_k = 0;
    core4->collective_arrived = 0;
    memset(core4->collective_src, 0, sizeof(core4->collective_src));
    memset(core4->collective_dst, 0, sizeof(core4->collective_dst));
    memset(core4->collective_peer, 0, sizeof(core4->collective_peer));
    core4->collective_pe_mask = 0;
    core4->collective_size_code = 0;
    memset(core4->collective_resume_pc, 0,
           sizeof(core4->collective_resume_pc));
    for (unsigned pe = 0; pe < LINX_CORE4_PE_COUNT; pe++) {
        if (core4->cpu[pe] == NULL) {
            continue;
        }
        CPULinxState *env = &core4->cpu[pe]->env;

        env->tile_shared_binder_count = 0;
        memset(env->tile_shared_binder, 0,
               sizeof(env->tile_shared_binder));
        for (unsigned acr = 0; acr < LINX_ACR_COUNT; acr++) {
            env->acr_block_state[acr].tile_shared_binder_count = 0;
            memset(env->acr_block_state[acr].tile_shared_binder, 0,
                   sizeof(env->acr_block_state[acr].tile_shared_binder));
        }
    }
    qemu_cond_broadcast(&core4->collective_cond);
    qemu_mutex_unlock(&core4->lock);
}

static void linx_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    LinxCPU *cpu = LINX_CPU(obj);
    CPULinxState *env = cpu_env(cs);

    memset(env, 0, offsetof(CPULinxState, end_reset_fields));

    env->gpr[LINX_REG_ZERO] = 0;
    env->pc = cpu->boot_pc;
    env->gpr[LINX_REG_SP] = cpu->boot_sp;
    env->gpr[LINX_REG_RA] = cpu->boot_ra;
    env->gpr[LINX_REG_A0] = cpu->boot_a0;
    env->gpr[LINX_REG_A1] = cpu->boot_a1;
    env->gpr[LINX_REG_A2] = cpu->boot_a2;
    env->pe_id = cpu->boot_pe_id;
    env->fcsr = 0;
    env->acr = 0;
    set_float_exception_flags(0, &env->fp_status);
    set_float_rounding_mode(float_round_nearest_even, &env->fp_status);
    set_default_nan_mode(true, &env->fp_status);
    set_float_default_nan_pattern(0b01000000, &env->fp_status);
    cs->exception_index = -1;
    cs->halted = 0;

    /* Cancel any pending timer interrupt. */
    if (env->timer) {
        timer_del(env->timer);
    }
}

static void linx_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    LinxCPU *cpu = LINX_CPU(dev);
    LinxCPUClass *lcc = LINX_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    lcc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    cpu_exec_realizefn(cs, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    if (cpu->dfx_watch_addr) {
        CPUWatchpoint *wp = NULL;
        uint64_t addr = cpu->dfx_watch_addr;
        uint32_t len = cpu->dfx_watch_len ? cpu->dfx_watch_len : 8;
        int flags = BP_CPU | BP_STOP_BEFORE_ACCESS |
                    (cpu->dfx_watch_flags & BP_MEM_ACCESS);

        if ((flags & BP_MEM_ACCESS) == 0) {
            flags |= BP_MEM_WRITE;
        }

        if (cpu_watchpoint_insert(cs, addr, len, flags, &wp) < 0) {
            error_setg(&local_err,
                       "linx: failed to insert DFX watchpoint addr=0x%" PRIx64
                       " len=%u flags=0x%x",
                       addr, len, flags);
            error_propagate(errp, local_err);
            return;
        }
        trace_linx_dfx_watchpoint_insert(addr, len, flags);
    }

    /* Create the per-CPU virtual timer after reset initialization. */
    {
        CPULinxState *env = cpu_env(cs);
        if (!env->timer) {
            env->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, linx_timer_cb, cs);
        }
    }
}

static ObjectClass *linx_cpu_class_by_name(const char *cpu_model)
{
    if (!cpu_model || !strcmp(cpu_model, "linx")) {
        return object_class_by_name(TYPE_LINX_CPU_LINX);
    }
    return NULL;
}

static void linx_cpu_init(Object *obj)
{
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps linx_sysemu_ops = {
    .has_work = linx_cpu_has_work,
    .get_phys_page_debug = linx_cpu_get_phys_page_debug,
};

static void linx_cpu_debug_excp_handler(CPUState *cs)
{
    CPULinxState *env = cpu_env(cs);

    if (cs->watchpoint_hit) {
        trace_linx_debug_watchpoint_hit(env->pc, cs->watchpoint_hit->hitaddr,
                                        cs->watchpoint_hit->flags);
        return;
    }

    if (cpu_breakpoint_test(cs, env->pc, BP_CPU) ||
        cpu_breakpoint_test(cs, env->pc, BP_GDB)) {
        trace_linx_debug_breakpoint_hit(env->pc);
    }
}

static bool linx_cpu_debug_check_breakpoint(CPUState *cs)
{
    CPULinxState *env = cpu_env(cs);

    return cpu_breakpoint_test(cs, env->pc, BP_CPU);
}

static bool linx_cpu_debug_check_watchpoint(CPUState *cs, CPUWatchpoint *wp)
{
    (void)cs;
    return (wp->flags & BP_CPU) != 0;
}

static const TCGCPUOps linx_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    /* Core4 collectives synchronously update peer architectural state. */
    .mttcg_supported = false,

    .initialize = linx_translate_init,
    .translate_code = linx_translate_code,
    .get_tb_cpu_state = linx_get_tb_cpu_state,
    .synchronize_from_tb = linx_cpu_synchronize_from_tb,
    .restore_state_to_opc = linx_restore_state_to_opc,
    .mmu_index = linx_cpu_mmu_index,
    .tlb_fill = linx_cpu_tlb_fill,
#if TARGET_LONG_BITS == 32
    .pointer_wrap = cpu_pointer_wrap_uint32,
#else
    .pointer_wrap = linx_pointer_wrap,
#endif
    .cpu_exec_interrupt = linx_cpu_exec_interrupt,
    .cpu_exec_halt = linx_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .debug_excp_handler = linx_cpu_debug_excp_handler,
    .debug_check_breakpoint = linx_cpu_debug_check_breakpoint,
    .debug_check_watchpoint = linx_cpu_debug_check_watchpoint,
    .do_interrupt = linx_cpu_do_interrupt,
};

static bool linx_core4_cpu_binder_live(const LinxCPU *cpu)
{
    const CPULinxState *env = &cpu->env;

    if (env->tile_shared_binder_count != 0u) {
        return true;
    }
    for (unsigned acr = 0; acr < LINX_ACR_COUNT; acr++) {
        if (env->acr_block_state[acr].tile_shared_binder_count != 0u) {
            return true;
        }
    }
    return false;
}

static bool linx_core4_migration_state_live(LinxCore4State *core4)
{
    bool live = false;

    qemu_mutex_lock(&core4->lock);
    for (unsigned tile = 0; tile < LINX_SHARED_TILE_COUNT; tile++) {
        const LinxSharedTileVersion *shared = &core4->shared_tile[tile];

        if (shared->allocation_mask || shared->initialized_mask ||
            shared->per_pe_capacity || shared->allocated_bytes ||
            shared->dtype || shared->producer_bpc) {
            live = true;
            break;
        }
    }
    live |= core4->collective_arrived != 0u;
    for (unsigned pe = 0; !live && pe < LINX_CORE4_PE_COUNT; pe++) {
        if (core4->cpu[pe] != NULL &&
            linx_core4_cpu_binder_live(core4->cpu[pe])) {
            live = true;
        }
    }
    qemu_mutex_unlock(&core4->lock);
    return live;
}

static int linx_cpu_pre_save(void *opaque)
{
    LinxCPU *cpu = opaque;
    CPULinxState *env = &cpu->env;

    /* Machine-owned Core4 Shared Tile/rendezvous state is not serialized. */
    if ((cpu->core4 != NULL && cpu->core4->cpu[1] != NULL) ||
        env->tile_shared_binder_count != 0u) {
        return -EINVAL;
    }

    for (unsigned acr = 0; acr < LINX_ACR_COUNT; acr++) {
        if (acr != (env->acr & 0xfu) &&
            (env->acr_block_state[acr].tile_iot_valid ||
             env->acr_block_state[acr].tile_iot_count)) {
            return -EINVAL;
        }
    }
    uint16_t expected_reserved[LINX_TILE_HAND_COUNT] = { 0 };
    uint16_t expected_pin[LINX_TILE_SLOT_COUNT] = { 0 };
    if (memcmp(expected_reserved, env->tile_hand_reserved,
               sizeof(expected_reserved)) != 0 ||
        memcmp(expected_pin, env->tile_pin_owner,
               sizeof(expected_pin)) != 0) {
        return -EINVAL;
    }
    if (cpu->core4 != NULL &&
        linx_core4_migration_state_live(cpu->core4)) {
        return -EINVAL;
    }
    return 0;
}

static bool linx_cpu_post_load(void *opaque, int version_id, Error **errp)
{
    LinxCPU *cpu = opaque;
    CPULinxState *env = &cpu->env;

    if (version_id >= 12 && version_id < 16) {
        memcpy(env->tile_reg_capacity, env->tile_reg_bytes,
               sizeof(env->tile_reg_capacity));
    }

    if (version_id < 12) {
        /* v11 carried no tile backing or allocator state. */
        env->tile_func = 0;
        env->tile_dtype = 0;
        env->tile_iot_valid = 0;
        env->tile_iot_flags = 0;
        env->tile_iot_dst = 0;
        env->tile_iot_grp = 0;
        env->tile_iot_src0 = 0;
        env->tile_iot_src1 = 0;
        env->tile_iot_reg = 0;
        env->tile_iot_size = 0;
        env->tile_arg_format = 0;
        env->tile_attr_raw = 0;
        env->tile_attr_pad = 0;
        env->tile_attr_dtype = 0;
        env->tile_ior_count = 0;
        memset(env->tile_ior_desc, 0, sizeof(env->tile_ior_desc));
        env->vec_ri_count = 0;
        memset(env->vec_ri_value, 0, sizeof(env->vec_ri_value));
        env->tile_iot_count = 0;
        memset(env->tile_iot_desc, 0, sizeof(env->tile_iot_desc));
        memset(env->tile_iot_src_valid, 0,
               sizeof(env->tile_iot_src_valid));
        memset(env->tile_iot_src_phys, 0, sizeof(env->tile_iot_src_phys));
        memset(env->tile_iot_output_valid, 0,
               sizeof(env->tile_iot_output_valid));
        memset(env->tile_iot_output_phys, 0,
               sizeof(env->tile_iot_output_phys));
        memset(env->tile_hand_live, 0, sizeof(env->tile_hand_live));
        memset(env->tile_hand_reserved, 0,
               sizeof(env->tile_hand_reserved));
        memset(env->tile_hand_order, 0, sizeof(env->tile_hand_order));
        memset(env->tile_hand_count, 0, sizeof(env->tile_hand_count));
        memset(env->tile_pin_owner, 0, sizeof(env->tile_pin_owner));
        env->tile_acc_carrier_valid = 0;
        env->tile_acc_carrier = 0;
        env->tile_acc_sources_valid = 0;
        env->tile_acc_src0 = 0;
        env->tile_acc_src1 = 0;
        memset(env->tile_reg, 0, sizeof(env->tile_reg));
        memset(env->tile_reg_capacity, 0, sizeof(env->tile_reg_capacity));
        memset(env->tile_reg_bytes, 0, sizeof(env->tile_reg_bytes));
        memset(env->tile_reg_elem_bytes, 0, sizeof(env->tile_reg_elem_bytes));
        memset(env->tile_reg_dtype, 0, sizeof(env->tile_reg_dtype));
        memset(env->tile_reg_layout, 0, sizeof(env->tile_reg_layout));
        memset(env->tile_reg_valid_cols, 0,
               sizeof(env->tile_reg_valid_cols));
        memset(env->tile_reg_valid_rows, 0,
               sizeof(env->tile_reg_valid_rows));
        memset(env->tile_reg_cols, 0, sizeof(env->tile_reg_cols));
        memset(env->tile_reg_rows, 0, sizeof(env->tile_reg_rows));
        memset(env->tile_acc, 0, sizeof(env->tile_acc));
        env->tile_acc_bytes = 0;
        env->tile_acc_dtype = 0;
        env->tile_acc_valid = 0;
        env->tile_acc_cols = 0;
        env->tile_acc_rows = 0;
        return true;
    }

    if (version_id == 12) {
        bool nonempty_tile = false;
        for (unsigned tile = 0;
             tile < LINX_TILE_SLOT_COUNT; tile++) {
            nonempty_tile |= env->tile_reg_bytes[tile] != 0;
        }
        for (unsigned hand = 0; hand < LINX_TILE_HAND_COUNT; hand++) {
            nonempty_tile |= env->tile_hand_live[hand] != 0;
        }
        if (env->tile_iot_valid || env->tile_iot_count || nonempty_tile ||
            env->tile_acc_bytes || env->tile_acc_carrier_valid ||
            env->tile_acc_sources_valid) {
            error_setg(errp,
                       "linx: cannot migrate nonempty v12 tile state across "
                       "the ordered-hand and 6-bit B.IOT transition");
            return false;
        }
        memset(env->tile_iot_src_valid, 0,
               sizeof(env->tile_iot_src_valid));
        memset(env->tile_iot_src_phys, 0, sizeof(env->tile_iot_src_phys));
        memset(env->tile_iot_output_valid, 0,
               sizeof(env->tile_iot_output_valid));
        memset(env->tile_iot_output_phys, 0,
               sizeof(env->tile_iot_output_phys));
        memset(env->tile_hand_reserved, 0,
               sizeof(env->tile_hand_reserved));
        memset(env->tile_hand_order, 0, sizeof(env->tile_hand_order));
        memset(env->tile_hand_count, 0, sizeof(env->tile_hand_count));
        memset(env->tile_pin_owner, 0, sizeof(env->tile_pin_owner));
    }
    if (version_id < 14) {
        memset(env->tile_reg_dtype, 0, sizeof(env->tile_reg_dtype));
    }
    if (version_id < 15) {
        bool nonempty_tile = false;

        for (unsigned tile = 0;
             tile < LINX_TILE_SLOT_COUNT; tile++) {
            nonempty_tile |= env->tile_reg_bytes[tile] != 0;
        }
        if (nonempty_tile) {
            error_setg(errp,
                       "linx: cannot migrate nonempty pre-v15 tile state "
                       "without shape metadata");
            return false;
        }
        memset(env->tile_reg_valid_cols, 0,
               sizeof(env->tile_reg_valid_cols));
        memset(env->tile_reg_valid_rows, 0,
               sizeof(env->tile_reg_valid_rows));
        memset(env->tile_reg_cols, 0, sizeof(env->tile_reg_cols));
        memset(env->tile_reg_rows, 0, sizeof(env->tile_reg_rows));
    }
    if (version_id < 17) {
        if (env->tile_acc_bytes != 0) {
            error_setg(errp,
                       "linx: cannot migrate nonempty pre-v17 CUBE ACC "
                       "without numeric type and shape metadata");
            return false;
        }
        env->tile_acc_dtype = 0;
        env->tile_acc_valid = 0;
        env->tile_acc_cols = 0;
        env->tile_acc_rows = 0;
    }

    if (env->tile_ior_count > LINX_TILE_MAX_IOR ||
        env->vec_ri_count > LINX_VEC_RI_MAX ||
        env->tile_iot_count > LINX_TILE_MAX_IOT) {
        error_setg(errp, "linx: invalid migrated tile descriptor counts");
        return false;
    }
    if (env->tile_iot_valid > 1 || env->tile_acc_carrier_valid > 1 ||
        env->tile_acc_sources_valid > 1 ||
        (env->tile_acc_carrier_valid &&
         env->tile_acc_carrier >= LINX_TILE_SLOT_COUNT) ||
        (env->tile_acc_sources_valid &&
         (env->tile_acc_src0 >= LINX_TILE_SLOT_COUNT ||
          env->tile_acc_src1 >= LINX_TILE_SLOT_COUNT))) {
        error_setg(errp, "linx: invalid migrated tile accumulator state");
        return false;
    }
    if (env->tile_acc_bytes > LINX_TILE_MAX_BYTES ||
        (env->tile_acc_bytes & 3u) != 0 || env->tile_acc_valid > 1u ||
        (!env->tile_acc_valid && (env->tile_acc_bytes != 0u ||
                                  env->tile_acc_cols != 0u ||
                                  env->tile_acc_rows != 0u)) ||
        (env->tile_acc_valid &&
         ((env->tile_acc_dtype != 0u && env->tile_acc_dtype != 1u &&
           env->tile_acc_dtype != 16u && env->tile_acc_dtype != 24u) ||
          env->tile_acc_cols == 0u || env->tile_acc_rows == 0u))) {
        error_setg(errp, "linx: invalid migrated tile accumulator footprint");
        return false;
    }
    uint64_t tile_capacity_in_use = 0;
    for (unsigned tile = 0;
         tile < LINX_TILE_SLOT_COUNT; tile++) {
        const uint32_t bytes = env->tile_reg_bytes[tile];
        const uint32_t capacity = env->tile_reg_capacity[tile];
        const uint32_t elem_bytes = env->tile_reg_elem_bytes[tile];
        const uint32_t valid_cols = env->tile_reg_valid_cols[tile];
        const uint32_t valid_rows = env->tile_reg_valid_rows[tile];
        const uint32_t cols = env->tile_reg_cols[tile];
        const uint32_t rows = env->tile_reg_rows[tile];
        const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
        const unsigned depth = tile % LINX_TILE_HAND_DEPTH;
        tile_capacity_in_use += capacity;
        if (bytes > LINX_TILE_MAX_BYTES || (bytes & 3u) != 0 ||
            capacity > LINX_TILE_PE_CAPACITY_BYTES ||
            ((env->tile_hand_live[hand] & LINX_TILE_HAND_BIT(depth)) != 0 &&
             bytes == 0) ||
            (bytes != 0 &&
             (elem_bytes == 0 || valid_cols == 0 || valid_rows == 0 ||
              cols == 0 || rows == 0 || valid_cols > cols ||
              valid_rows > rows ||
              (uint64_t)rows * cols * elem_bytes > bytes))) {
            error_setg(errp, "linx: invalid migrated tile %u state", tile);
            return false;
        }
    }
    if (tile_capacity_in_use > LINX_TILE_PE_CAPACITY_BYTES) {
        error_setg(errp, "linx: migrated tile capacity exceeds PE limit");
        return false;
    }
    for (unsigned hand = 0; hand < LINX_TILE_HAND_COUNT; hand++) {
        uint16_t seen = 0;
        if (env->tile_hand_count[hand] > LINX_TILE_HAND_DEPTH ||
            (env->tile_hand_live[hand] & env->tile_hand_reserved[hand]) != 0) {
            error_setg(errp, "linx: invalid migrated tile hand %u state", hand);
            return false;
        }
        for (unsigned rank = 0; rank < env->tile_hand_count[hand]; rank++) {
            const unsigned tile = env->tile_hand_order[hand][rank];
            const unsigned depth = tile % LINX_TILE_HAND_DEPTH;
            if (tile / LINX_TILE_HAND_DEPTH != hand ||
                (seen & LINX_TILE_HAND_BIT(depth)) != 0 ||
                (env->tile_hand_live[hand] & LINX_TILE_HAND_BIT(depth)) == 0) {
                error_setg(errp,
                           "linx: invalid migrated tile hand %u order", hand);
                return false;
            }
            seen |= LINX_TILE_HAND_BIT(depth);
        }
        if (seen != env->tile_hand_live[hand]) {
            error_setg(errp,
                       "linx: migrated tile hand %u order/live mismatch", hand);
            return false;
        }
    }
    uint64_t planned_output_seen = 0;
    for (unsigned i = 0; i < LINX_TILE_MAX_IOT; i++) {
        if (env->tile_iot_src_valid[i] > 3 ||
            env->tile_iot_output_valid[i] > 1) {
            error_setg(errp,
                       "linx: invalid migrated tile binding %u flags", i);
            return false;
        }
        if (i >= env->tile_iot_count &&
            (env->tile_iot_src_valid[i] ||
             env->tile_iot_output_valid[i])) {
            error_setg(errp,
                       "linx: migrated tile binding %u is outside count", i);
            return false;
        }
        for (unsigned source = 0; source < 2; source++) {
            if ((env->tile_iot_src_valid[i] & (1u << source)) != 0 &&
                env->tile_iot_src_phys[i][source] >=
                    LINX_TILE_SLOT_COUNT) {
                error_setg(errp,
                           "linx: invalid migrated tile binding %u source",
                           i);
                return false;
            }
            if ((env->tile_iot_src_valid[i] & (1u << source)) != 0) {
                const unsigned tile = env->tile_iot_src_phys[i][source];
                const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
                const unsigned depth = tile % LINX_TILE_HAND_DEPTH;
                if ((env->tile_hand_live[hand] & LINX_TILE_HAND_BIT(depth)) == 0 ||
                    env->tile_reg_bytes[tile] == 0) {
                    error_setg(errp,
                               "linx: migrated tile binding %u source is not live",
                               i);
                    return false;
                }
            }
        }
        if (env->tile_iot_output_valid[i]) {
            const unsigned tile = env->tile_iot_output_phys[i];
            if (tile >= LINX_TILE_SLOT_COUNT ||
                (planned_output_seen & (UINT64_C(1) << tile)) != 0 ||
                (env->tile_hand_live[tile / LINX_TILE_HAND_DEPTH] &
                 LINX_TILE_HAND_BIT(tile % LINX_TILE_HAND_DEPTH)) != 0) {
                error_setg(errp,
                           "linx: invalid migrated tile binding %u output",
                           i);
                return false;
            }
            planned_output_seen |= UINT64_C(1) << tile;
        }
    }
    uint16_t expected_reserved[LINX_TILE_HAND_COUNT] = { 0 };
    uint16_t expected_pin[LINX_TILE_SLOT_COUNT] = { 0 };
    if (memcmp(expected_reserved, env->tile_hand_reserved,
               sizeof(expected_reserved)) != 0 ||
        memcmp(expected_pin, env->tile_pin_owner,
               sizeof(expected_pin)) != 0) {
        error_setg(errp,
                   "linx: migrated tile reservation/pin ownership mismatch");
        return false;
    }
    if (env->tile_acc_carrier_valid) {
        const unsigned tile = env->tile_acc_carrier;
        const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
        const unsigned depth = tile % LINX_TILE_HAND_DEPTH;
        if ((env->tile_hand_live[hand] & LINX_TILE_HAND_BIT(depth)) == 0 ||
            env->tile_reg_bytes[tile] == 0) {
            error_setg(errp, "linx: migrated accumulator carrier is not live");
            return false;
        }
    }
    return true;
}

static const VMStateDescription vmstate_linx_cpu = {
    .name = "linx_cpu",
    .version_id = 18,
    .minimum_version_id = 18,
    .pre_save = linx_cpu_pre_save,
    .post_load_errp = linx_cpu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(env.pc, LinxCPU),
        VMSTATE_UINT32(env.cond, LinxCPU),
        VMSTATE_UINT64(env.vec_p, LinxCPU),
        VMSTATE_UINT64(env.tgt, LinxCPU),
        VMSTATE_UINT32(env.carg, LinxCPU),
        VMSTATE_UINT32(env.brtype, LinxCPU),
        VMSTATE_UINT32(env.blocktype, LinxCPU),
        VMSTATE_UINT64(env.body_tpc, LinxCPU),
        VMSTATE_UINT64(env.return_pc, LinxCPU),
        VMSTATE_UINT32(env.in_body, LinxCPU),
        VMSTATE_UINT64(env.tmpl_pc, LinxCPU),
        VMSTATE_UINT32(env.tmpl_kind, LinxCPU),
        VMSTATE_UINT32(env.tmpl_step, LinxCPU),
        VMSTATE_UINT32(env.tmpl_reg_cur, LinxCPU),
        VMSTATE_UINT32(env.tmpl_reg_begin, LinxCPU),
        VMSTATE_UINT32(env.tmpl_reg_end, LinxCPU),
        VMSTATE_UINT64(env.tmpl_stacksize, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_dst, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_src, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_remaining, LinxCPU),
        VMSTATE_UINT64(env.tmpl_mem_value, LinxCPU),
        VMSTATE_UINT32(env.fcsr, LinxCPU),
        VMSTATE_UINT32(env.acr, LinxCPU),
        VMSTATE_UINT64_ARRAY(env.gpr, LinxCPU, LINX_GPR_COUNT),
        VMSTATE_UINT64_ARRAY(env.tq, LinxCPU, 4),
        VMSTATE_UINT64_ARRAY(env.uq, LinxCPU, 4),
        VMSTATE_UINT64_ARRAY(env.vtq, LinxCPU, LINX_VEC_QUEUE_DEPTH),
        VMSTATE_UINT64_ARRAY(env.lb, LinxCPU, 3),
        VMSTATE_UINT64_ARRAY(env.lc, LinxCPU, 3),
        VMSTATE_UINT32_V(env.tile_func, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_dtype, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_valid, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_flags, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_dst, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_grp, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_src0, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_src1, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_reg, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_iot_size, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_arg_format, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_attr_raw, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_attr_pad, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_attr_dtype, LinxCPU, 12),
        VMSTATE_UINT32_V(env.tile_ior_count, LinxCPU, 12),
        VMSTATE_UINT64_ARRAY_V(env.tile_ior_desc, LinxCPU,
                               LINX_TILE_MAX_IOR, 12),
        VMSTATE_UINT32_V(env.vec_ri_count, LinxCPU, 12),
        VMSTATE_UINT64_ARRAY_V(env.vec_ri_value, LinxCPU,
                               LINX_VEC_RI_MAX, 12),
        VMSTATE_UINT32_V(env.tile_iot_count, LinxCPU, 12),
        VMSTATE_UINT64_ARRAY_V(env.tile_iot_desc, LinxCPU,
                               LINX_TILE_MAX_IOT, 12),
        VMSTATE_UINT8_ARRAY_V(env.tile_iot_src_valid, LinxCPU,
                              LINX_TILE_MAX_IOT, 13),
        VMSTATE_UINT8_2DARRAY_V(env.tile_iot_src_phys, LinxCPU,
                                LINX_TILE_MAX_IOT, 2, 13),
        VMSTATE_UINT8_ARRAY_V(env.tile_iot_output_valid, LinxCPU,
                              LINX_TILE_MAX_IOT, 13),
        VMSTATE_UINT8_ARRAY_V(env.tile_iot_output_phys, LinxCPU,
                              LINX_TILE_MAX_IOT, 13),
        VMSTATE_UINT16_ARRAY_V(env.tile_hand_live, LinxCPU,
                               LINX_TILE_HAND_COUNT, 18),
        VMSTATE_UINT16_ARRAY_V(env.tile_hand_reserved, LinxCPU,
                               LINX_TILE_HAND_COUNT, 18),
        VMSTATE_UINT8_2DARRAY_V(env.tile_hand_order, LinxCPU,
                                LINX_TILE_HAND_COUNT,
                                LINX_TILE_HAND_DEPTH, 13),
        VMSTATE_UINT8_ARRAY_V(env.tile_hand_count, LinxCPU,
                              LINX_TILE_HAND_COUNT, 13),
        VMSTATE_UINT16_ARRAY_V(env.tile_pin_owner, LinxCPU,
                               LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT8_V(env.tile_acc_carrier_valid, LinxCPU, 12),
        VMSTATE_UINT8_V(env.tile_acc_carrier, LinxCPU, 12),
        VMSTATE_UINT8_V(env.tile_acc_sources_valid, LinxCPU, 12),
        VMSTATE_UINT8_V(env.tile_acc_src0, LinxCPU, 12),
        VMSTATE_UINT8_V(env.tile_acc_src1, LinxCPU, 12),
        VMSTATE_UINT32_2DARRAY_V(env.tile_reg, LinxCPU,
                                 LINX_TILE_SLOT_COUNT,
                                 LINX_TILE_MAX_WORDS, 18),
        VMSTATE_UINT32_ARRAY_V(env.tile_reg_capacity, LinxCPU,
                               LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT32_ARRAY_V(env.tile_reg_bytes, LinxCPU,
                               LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT8_ARRAY_V(env.tile_reg_elem_bytes, LinxCPU,
                              LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT8_ARRAY_V(env.tile_reg_dtype, LinxCPU,
                              LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT8_ARRAY_V(env.tile_reg_layout, LinxCPU,
                              LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT16_ARRAY_V(env.tile_reg_valid_cols, LinxCPU,
                               LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT16_ARRAY_V(env.tile_reg_valid_rows, LinxCPU,
                               LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT16_ARRAY_V(env.tile_reg_cols, LinxCPU,
                               LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT16_ARRAY_V(env.tile_reg_rows, LinxCPU,
                               LINX_TILE_SLOT_COUNT, 18),
        VMSTATE_UINT32_ARRAY_V(env.tile_acc, LinxCPU,
                               LINX_TILE_MAX_WORDS, 12),
        VMSTATE_UINT32_V(env.tile_acc_bytes, LinxCPU, 12),
        VMSTATE_UINT8_V(env.tile_acc_dtype, LinxCPU, 17),
        VMSTATE_UINT8_V(env.tile_acc_valid, LinxCPU, 17),
        VMSTATE_UINT16_V(env.tile_acc_cols, LinxCPU, 17),
        VMSTATE_UINT16_V(env.tile_acc_rows, LinxCPU, 17),
        VMSTATE_UINT64(env.insn_pc_next, LinxCPU),
        VMSTATE_UINT64_ARRAY(env.ssr, LinxCPU, LINX_SSR_COUNT),
        VMSTATE_UINT64_2DARRAY(env.ssr_acr, LinxCPU, LINX_ACR_COUNT, LINX_SSR_COUNT),
        VMSTATE_UINT64_ARRAY(env.irq_level_acr, LinxCPU, LINX_ACR_COUNT),
        VMSTATE_UINT64(env.lr_addr, LinxCPU),
        VMSTATE_UINT32(env.lr_size, LinxCPU),
        VMSTATE_UINT32(env.lr_valid, LinxCPU),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property linx_cpu_properties[] = {
    DEFINE_PROP_UINT64("dfx-watch-addr", LinxCPU, dfx_watch_addr, 0),
    DEFINE_PROP_UINT32("dfx-watch-len", LinxCPU, dfx_watch_len, 0),
    DEFINE_PROP_UINT32("dfx-watch-flags", LinxCPU, dfx_watch_flags, BP_MEM_WRITE),
};

static void linx_cpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CPUClass *cc = CPU_CLASS(klass);
    LinxCPUClass *lcc = LINX_CPU_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_parent_realize(dc, linx_cpu_realize,
                                    &lcc->parent_realize);
    dc->vmsd = &vmstate_linx_cpu;
    device_class_set_props(dc, linx_cpu_properties);

    resettable_class_set_parent_phases(rc, NULL, linx_cpu_reset_hold, NULL,
                                       &lcc->parent_phases);

    cc->class_by_name = linx_cpu_class_by_name;
    cc->dump_state = linx_cpu_dump_state;
    cc->set_pc = linx_cpu_set_pc;
    cc->get_pc = linx_cpu_get_pc;
    cc->sysemu_ops = &linx_sysemu_ops;
    cc->tcg_ops = &linx_tcg_ops;
}

static const TypeInfo linx_cpu_base_type_info = {
    .name = TYPE_LINX_CPU,
    .parent = TYPE_CPU,
    .instance_size = sizeof(LinxCPU),
    .instance_align = __alignof__(LinxCPU),
    .instance_init = linx_cpu_init,
    .abstract = true,
    .class_size = sizeof(LinxCPUClass),
    .class_init = linx_cpu_class_init,
};

static const TypeInfo linx_cpu_type_info = {
    .name = TYPE_LINX_CPU_LINX,
    .parent = TYPE_LINX_CPU,
};

static void linx_cpu_register_types(void)
{
    type_register_static(&linx_cpu_base_type_info);
    type_register_static(&linx_cpu_type_info);
}

type_init(linx_cpu_register_types)
