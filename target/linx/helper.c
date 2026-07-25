/*
 * LinxISA helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "linx/model/emulator/minst_record_c.h"
#include "qemu/bswap.h"
#include "cpu.h"
#include "trace.h"
#include "opcode_meta.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/helper-retaddr.h"
#include "accel/accel-cpu-ops.h"
#include "fpu/softfloat-helpers.h"
#include "accel/tcg/probe.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "exec/memopidx.h"
#include "accel/tcg/cpu-ldst-common.h"
#include "accel/tcg/internal-common.h"
#include "exec/cputlb.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include <inttypes.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Optional compatibility addend configured from $LINX_CALLFRAME_SIZE. */
extern uint64_t linx_callframe_size;

static uint64_t linx_bstart_cache_stat_resets;
static uint64_t linx_bstart_cache_stat_page_resets;
static uint64_t linx_bstart_cache_stat_page_reset_entries;

static inline size_t linx_bstart_cache_slot(uint64_t target)
{
    uint64_t key = target >> 1;

    key ^= key >> 12;
    key ^= key >> 24;
    return (size_t)(key & (LINX_BSTART_CACHE_SIZE - 1u));
}

static inline void linx_bstart_cache_reset(CPULinxState *env)
{
    linx_bstart_cache_stat_resets++;
    memset(env->bstart_cache_valid, 0, sizeof(env->bstart_cache_valid));
}

static inline void linx_bstart_cache_reset_page(CPULinxState *env, uint64_t addr)
{
    const uint64_t page = addr & TARGET_PAGE_MASK;
    uint64_t reset_entries = 0;

    linx_bstart_cache_stat_page_resets++;
    for (size_t i = 0; i < LINX_BSTART_CACHE_SIZE; i++) {
        if (env->bstart_cache_valid[i] &&
            (env->bstart_cache_tag[i] & TARGET_PAGE_MASK) == page) {
            env->bstart_cache_valid[i] = 0;
            reset_entries++;
        }
    }
    linx_bstart_cache_stat_page_reset_entries += reset_entries;
}

static bool linx_is_bstart_at_addr(CPULinxState *env, uint64_t pc);
static inline int linx_env_mmu_index(CPULinxState *env);
static bool linx_parse_u64(const char *s, uint64_t *out);
static inline bool linx_env_enabled(const char *name);
static inline uint32_t linx_ssr_low12(uint32_t ssrid);
static bool linx_debug_read_guest_u64(CPULinxState *env, uint64_t addr,
                                      uint64_t *value);
static void linx_debug_dump_guest_units(CPULinxState *env, uint64_t addr,
                                        unsigned count, const char *label,
                                        unsigned width);
static void linx_debug_dump_guest_words(CPULinxState *env, uint64_t addr,
                                        unsigned count, const char *label);

typedef enum LinxTlbInvOp {
    LINX_TLB_INV_IALL,
    LINX_TLB_INV_IA,
    LINX_TLB_INV_IV,
    LINX_TLB_INV_IAV,
} LinxTlbInvOp;

static bool linx_print_insn_count_inited;
static bool linx_print_insn_count_enabled;
static bool linx_semihost_inited;
static bool linx_semihost_enabled;

#define LINX_DEBUG_PC_WATCH_DUMP_SOURCE_MAX 8
#define LINX_DEBUG_PC_WATCH_DUMP_OFFSET_MAX 8
#define LINX_DEBUG_PC_WATCH_DUMP_PTR_OFFSET_MAX 8

static bool linx_debug_local_inited;
static bool linx_debug_local_enabled;
static bool linx_debug_body_replay_inited;
static bool linx_debug_body_replay_enabled;
static bool linx_debug_acre_stderr_inited;
static bool linx_debug_acre_stderr_enabled;
static bool linx_acre_trace_inited;
static bool linx_acre_trace_enabled;
static bool linx_acre_trace_pc_filter_enabled;
static uint64_t linx_acre_trace_pc_lo;
static uint64_t linx_acre_trace_pc_hi = UINT64_MAX;
static bool linx_acre_trace_bpc_filter_enabled;
static uint64_t linx_acre_trace_bpc_lo;
static uint64_t linx_acre_trace_bpc_hi = UINT64_MAX;
static bool linx_acre_trace_count_filter_enabled;
static uint64_t linx_acre_trace_count_lo;
static uint64_t linx_acre_trace_count_hi = UINT64_MAX;
static bool linx_acre_trace_target_filter_enabled;
static uint64_t linx_acre_trace_target;
static bool linx_acre_trace_rra_filter_enabled;
static uint64_t linx_acre_trace_rra;
static bool linx_acre_trace_trap_filter_enabled;
static uint64_t linx_acre_trace_trap;
static uint64_t linx_acre_trace_limit;
static uint64_t linx_acre_trace_emitted;
static bool linx_acre_trace_regs_enabled;
static unsigned linx_acre_trace_code_bytes;
static bool linx_queue_trace_inited;
static bool linx_queue_trace_enabled;
static bool linx_queue_trace_pc_filter_enabled;
static uint64_t linx_queue_trace_pc_lo;
static uint64_t linx_queue_trace_pc_hi = UINT64_MAX;
static bool linx_queue_trace_bpc_filter_enabled;
static uint64_t linx_queue_trace_bpc_lo;
static uint64_t linx_queue_trace_bpc_hi = UINT64_MAX;
static bool linx_queue_trace_count_filter_enabled;
static uint64_t linx_queue_trace_count_lo;
static uint64_t linx_queue_trace_count_hi = UINT64_MAX;
static bool linx_queue_trace_all;
static uint64_t linx_queue_trace_limit;
static uint64_t linx_queue_trace_emitted;
static bool linx_queue_trace_last_valid;
static uint64_t linx_queue_trace_last_tq[4];
static uint64_t linx_queue_trace_last_uq[4];
static uint64_t linx_queue_trace_last_tgt;
static uint64_t linx_queue_trace_last_body_tpc;
static uint64_t linx_queue_trace_last_return_pc;
static uint32_t linx_queue_trace_last_acr;
static uint32_t linx_queue_trace_last_in_body;
static uint32_t linx_queue_trace_last_blocktype;
static uint32_t linx_queue_trace_last_brtype;
static uint32_t linx_queue_trace_last_call_ra_set;
static uint32_t linx_queue_trace_last_call_setret_pending;

enum {
    LINX_EBARG_IDX_0 = 0xF40,
    LINX_EBARG_IDX_TQ0 = 0xF45,
    LINX_EBARG_IDX_TQ1 = 0xF46,
    LINX_EBARG_IDX_TQ2 = 0xF47,
    LINX_EBARG_IDX_TQ3 = 0xF48,
    LINX_EBARG_IDX_UQ0 = 0xF49,
    LINX_EBARG_IDX_UQ1 = 0xF4A,
    LINX_EBARG_IDX_UQ2 = 0xF4B,
    LINX_EBARG_IDX_UQ3 = 0xF4C,
    LINX_EBARG_IDX_LB = 0xF4D,
    LINX_EBARG_IDX_LC = 0xF4E,
};
static bool linx_debug_work_grab_inited;
static bool linx_debug_work_grab_enabled;
static unsigned linx_debug_work_grab_emits;
static bool linx_debug_pc_watch_inited;
static unsigned linx_debug_pc_watch_count;
static uint64_t linx_debug_pc_watch[16];
static uint64_t linx_debug_pc_watch_hits[16];
static uint64_t linx_debug_pc_watch_printed[16];
static uint64_t linx_debug_pc_watch_count_lo;
static uint64_t linx_debug_pc_watch_count_hi = UINT64_MAX;
static uint64_t linx_debug_pc_watch_hit_lo;
static uint64_t linx_debug_pc_watch_hit_hi = UINT64_MAX;
static uint64_t linx_debug_pc_watch_hit_limit;
static bool linx_debug_pc_watch_match_source_enabled;
static unsigned linx_debug_pc_watch_match_kind;
static unsigned linx_debug_pc_watch_match_index;
static const char *linx_debug_pc_watch_match_name;
static uint64_t linx_debug_pc_watch_match_value;
static uint64_t linx_debug_pc_watch_match_mask = UINT64_MAX;
static unsigned linx_debug_pc_watch_dump_words;
static unsigned linx_debug_pc_watch_dump_width = 8;
static uint64_t linx_debug_pc_watch_dump_offset;
static unsigned linx_debug_pc_watch_dump_offset_count;
static uint64_t linx_debug_pc_watch_dump_offsets[LINX_DEBUG_PC_WATCH_DUMP_OFFSET_MAX];
static unsigned linx_debug_pc_watch_dump_ptr_offset_count;
static uint64_t linx_debug_pc_watch_dump_ptr_offsets[LINX_DEBUG_PC_WATCH_DUMP_PTR_OFFSET_MAX];
static unsigned linx_debug_pc_watch_dump_kind;
static unsigned linx_debug_pc_watch_dump_index = LINX_REG_A0;
static const char *linx_debug_pc_watch_dump_name = "a0";
static unsigned linx_debug_pc_watch_dump_source_count;
static unsigned linx_debug_pc_watch_dump_source_kinds[LINX_DEBUG_PC_WATCH_DUMP_SOURCE_MAX];
static unsigned linx_debug_pc_watch_dump_source_indexes[LINX_DEBUG_PC_WATCH_DUMP_SOURCE_MAX];
static const char *linx_debug_pc_watch_dump_source_names[LINX_DEBUG_PC_WATCH_DUMP_SOURCE_MAX];
static unsigned linx_debug_pc_watch_dump_code_bytes;
static unsigned linx_debug_pc_watch_dump_phys_bytes;
static bool linx_debug_pc_watch_exit;
static bool linx_debug_pc_watch_dump_call_ring;
static bool linx_debug_pc_watch_regs_enabled;
static bool linx_debug_pc_watch_print_enabled = true;
static bool linx_debug_pc_watch_ring_enabled;
static uint64_t linx_debug_pc_watch_ring_size;
static uint64_t linx_debug_pc_watch_ring_next;
static uint64_t linx_debug_pc_watch_ring_count;
static bool linx_debug_pc_watch_ring_mem_enabled;
static unsigned linx_debug_pc_watch_ring_mem_kind;
static unsigned linx_debug_pc_watch_ring_mem_index;
static const char *linx_debug_pc_watch_ring_mem_name;
static uint64_t linx_debug_pc_watch_ring_mem_offset;
static bool linx_pc_sample_inited;
static uint64_t linx_pc_sample_interval;
static bool linx_pc_sample_filter_enabled;
static uint64_t linx_pc_sample_filter_lo;
static uint64_t linx_pc_sample_filter_hi;
static uint64_t linx_pc_sample_last_bucket = UINT64_MAX;
static bool linx_heartbeat_inited;
static uint64_t linx_heartbeat_interval;
static uint64_t linx_heartbeat_last_bucket = UINT64_MAX;
static uint64_t linx_heartbeat_last_count;
static uint64_t linx_heartbeat_last_pc;
static uint64_t linx_heartbeat_last_bpc;
static uint64_t linx_heartbeat_last_tpc;
static uint64_t linx_heartbeat_same_site_repeats;
static uint64_t linx_heartbeat_same_site_warn;
static bool linx_heartbeat_same_site_reported;
static bool linx_heartbeat_extended_enabled;
static bool linx_heartbeat_regs_enabled;
static unsigned linx_heartbeat_dump_code_bytes;
static bool linx_frame_stats_inited;
static bool linx_frame_stats_enabled;
static bool linx_frame_restore_host_load_inited;
static bool linx_frame_restore_host_load_enabled;
static bool linx_frame_single_restore_host_load_inited;
static bool linx_frame_single_restore_host_load_enabled;
static bool linx_frame_restore_host_verify_inited;
static bool linx_frame_restore_host_verify_enabled;
static bool linx_frame_shape_hot_inited;
static bool linx_frame_shape_hot_enabled;
static bool linx_frame_single_reg_fast_inited;
static bool linx_frame_single_reg_fast_enabled;
static bool linx_frame_page_fast_inited;
static bool linx_frame_page_fast_enabled;
static uint64_t linx_frame_restore_host_verify_emit_limit;
static uint64_t linx_frame_restore_host_verify_emitted;
static uint64_t linx_frame_stat_fentry_calls;
static uint64_t linx_frame_stat_fentry_save_probes;
static uint64_t linx_frame_stat_fentry_save_slots;
static uint64_t linx_frame_stat_fentry_host_stores;
static uint64_t linx_frame_stat_fentry_fallback_stores;
static uint64_t linx_frame_stat_fexit_calls;
static uint64_t linx_frame_stat_fret_stk_calls;
static uint64_t linx_frame_stat_fret_ra_calls;
static uint64_t linx_frame_stat_restore_slots;
static uint64_t linx_frame_stat_restore_host_loads;
static uint64_t linx_frame_stat_restore_fallback_loads;
static uint64_t linx_frame_stat_restore_host_verify_loads;
static uint64_t linx_frame_stat_restore_host_verify_mismatches;
static uint64_t linx_frame_stat_ret_fast_hits;
static uint64_t linx_frame_stat_ret_checks;
static uint64_t linx_frame_stat_single_fast_fentry;
static uint64_t linx_frame_stat_single_fast_fret_stk;
static uint64_t linx_frame_stat_page_fast_fentry;
static uint64_t linx_frame_stat_page_fast_restore;
static bool linx_tlb_trace_inited;
static bool linx_tlb_trace_enabled;
static bool linx_tlb_trace_pc_filter_enabled;
static uint64_t linx_tlb_trace_pc_lo;
static uint64_t linx_tlb_trace_pc_hi = UINT64_MAX;
static bool linx_tlb_trace_count_filter_enabled;
static uint64_t linx_tlb_trace_count_lo;
static uint64_t linx_tlb_trace_count_hi = UINT64_MAX;
static uint64_t linx_tlb_trace_limit = 64;
static uint64_t linx_tlb_trace_emitted;
static unsigned linx_tlb_trace_code_bytes;
static bool linx_tlb_stats_inited;
static bool linx_tlb_stats_enabled;
static bool linx_tlb_inv_hot_inited;
static bool linx_tlb_inv_hot_enabled;
static bool linx_fcmp_trace_inited;
static bool linx_fcmp_trace_enabled;
static bool linx_fcmp_trace_pc_filter_enabled;
static uint64_t linx_fcmp_trace_pc_lo;
static uint64_t linx_fcmp_trace_pc_hi = UINT64_MAX;
static bool linx_fcmp_trace_count_filter_enabled;
static uint64_t linx_fcmp_trace_count_lo;
static uint64_t linx_fcmp_trace_count_hi = UINT64_MAX;
static uint64_t linx_fcmp_trace_limit;
static uint64_t linx_fcmp_trace_emitted;
static uint32_t linx_fcmp_trace_op_mask;
static bool linx_tp_trace_inited;
static bool linx_tp_trace_enabled;
static bool linx_tp_trace_ssr_enabled;
static bool linx_tp_trace_reads_enabled;
static uint64_t linx_tp_trace_limit;
static uint64_t linx_tp_trace_emitted;
static bool linx_call_trace_inited;
static bool linx_call_trace_enabled;
static bool linx_call_trace_filter_enabled;
static uint64_t linx_call_trace_filter_lo;
static uint64_t linx_call_trace_filter_hi;
static bool linx_call_trace_count_filter_enabled;
static uint64_t linx_call_trace_count_lo;
static uint64_t linx_call_trace_count_hi;
static uint64_t linx_call_trace_limit;
static uint64_t linx_call_trace_emitted;
static bool linx_call_trace_ring_enabled;
static uint64_t linx_call_trace_ring_size;
static uint64_t linx_call_trace_ring_next;
static uint64_t linx_call_trace_ring_count;
static bool linx_fentry_trace_inited;
static bool linx_fentry_trace_enabled;
static bool linx_fentry_trace_pc_filter_enabled;
static uint64_t linx_fentry_trace_pc_lo;
static uint64_t linx_fentry_trace_pc_hi;
static bool linx_fentry_trace_count_filter_enabled;
static uint64_t linx_fentry_trace_count_lo;
static uint64_t linx_fentry_trace_count_hi;
static bool linx_fentry_trace_ra_filter_enabled;
static uint64_t linx_fentry_trace_ra;
static bool linx_fentry_trace_sp_filter_enabled;
static uint64_t linx_fentry_trace_sp;
static bool linx_fentry_trace_new_sp_filter_enabled;
static uint64_t linx_fentry_trace_new_sp;
static uint64_t linx_fentry_trace_limit;
static uint64_t linx_fentry_trace_emitted;
static unsigned linx_fentry_trace_dump_words;
static bool linx_fentry_trace_regs_enabled;
static bool linx_fret_stk_trace_inited;
static bool linx_fret_stk_trace_enabled;
static bool linx_fret_stk_trace_pc_filter_enabled;
static uint64_t linx_fret_stk_trace_pc_lo;
static uint64_t linx_fret_stk_trace_pc_hi;
static bool linx_fret_stk_trace_count_filter_enabled;
static uint64_t linx_fret_stk_trace_count_lo;
static uint64_t linx_fret_stk_trace_count_hi;
static bool linx_fret_stk_trace_ra_filter_enabled;
static uint64_t linx_fret_stk_trace_ra;
static uint64_t linx_fret_stk_trace_limit;
static uint64_t linx_fret_stk_trace_emitted;
static unsigned linx_fret_stk_trace_dump_words;
static bool linx_fret_stk_trace_regs_enabled;
static bool linx_mem_trace_inited;
static bool linx_mem_trace_enabled;
static bool linx_mem_trace_addr_filter_enabled;
static uint64_t linx_mem_trace_addr;
static uint64_t linx_mem_trace_size;
static uint64_t linx_mem_trace_limit;
static uint64_t linx_mem_trace_emitted;
static bool linx_mem_trace_loads = true;
static bool linx_mem_trace_stores = true;
static bool linx_mem_trace_pc_filter_enabled;
static uint64_t linx_mem_trace_pc_lo;
static uint64_t linx_mem_trace_pc_hi;
static bool linx_mem_trace_count_filter_enabled;
static uint64_t linx_mem_trace_count_lo;
static uint64_t linx_mem_trace_count_hi;
static bool linx_mem_trace_context_enabled;
static bool linx_mem_trace_pre_enabled;
static bool linx_mem_trace_regs_enabled;
static bool linx_mem_trace_acr_filter_enabled;
static uint8_t linx_mem_trace_acr_filter;
static bool linx_syscall_trace_inited;
static bool linx_syscall_trace_enabled;
static bool linx_syscall_trace_nr_filter_enabled;
static uint64_t linx_syscall_trace_nrs[16];
static unsigned linx_syscall_trace_nr_count;
static uint64_t linx_syscall_trace_limit;
static uint64_t linx_syscall_trace_emitted;
static bool linx_syscall_trace_pc_filter_enabled;
static uint64_t linx_syscall_trace_pc_lo;
static uint64_t linx_syscall_trace_pc_hi;
static bool linx_syscall_trace_strings_enabled;
static bool linx_syscall_trace_regs_enabled;
static uint64_t linx_syscall_trace_string_max = 96;
static bool linx_syscall_trace_dump_arg_enabled;
static unsigned linx_syscall_trace_dump_arg;
static unsigned linx_syscall_trace_dump_arg_count;
static unsigned linx_syscall_trace_dump_args[6];
static uint64_t linx_syscall_trace_dump_bytes;
static bool linx_cfi_trace_inited;
static bool linx_cfi_trace_enabled;
static bool linx_bstart_cache_revalidate_inited;
static bool linx_bstart_cache_revalidate_enabled;
static bool linx_bstart_cache_stats_inited;
static bool linx_bstart_cache_stats_enabled;
static uint64_t linx_bstart_cache_stats_interval;
static uint64_t linx_bstart_cache_stat_checks;
static uint64_t linx_bstart_cache_stat_hits;
static uint64_t linx_bstart_cache_stat_revalidations;
static uint64_t linx_bstart_cache_stat_continuations;
static uint64_t linx_bstart_cache_stat_fallthroughs;
static uint64_t linx_bstart_cache_stat_bstarts;
static uint64_t linx_bstart_cache_stat_defers;
static uint64_t linx_bstart_cache_stat_bad;
static uint64_t linx_bstart_cache_stat_inserts;

#define LINX_CALL_TRACE_RING_MAX 128
#define LINX_DEBUG_PC_WATCH_RING_MAX 128

typedef struct LinxCallTraceRingEntry {
    uint32_t event;
    uint32_t acr;
    uint32_t brtype;
    uint32_t call_ra_set;
    uint32_t call_setret_pending;
    uint32_t in_body;
    uint32_t tmpl_kind;
    uint32_t tmpl_step;
    uint64_t pc;
    uint64_t extra0;
    uint64_t extra1;
    uint64_t count;
    uint64_t envpc;
    uint64_t bpc;
    uint64_t tpc;
    uint64_t cstate;
    uint64_t tgt;
    uint64_t ra;
    uint64_t sp;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t body_tpc;
    uint64_t return_pc;
    uint64_t tmpl_pc;
} LinxCallTraceRingEntry;

static LinxCallTraceRingEntry linx_call_trace_ring[LINX_CALL_TRACE_RING_MAX];

typedef struct LinxDebugPcWatchRingEntry {
    uint32_t watch_index;
    uint32_t acr;
    uint32_t cond;
    uint32_t carg;
    uint32_t brtype;
    uint32_t in_body;
    uint32_t blocktype;
    uint32_t call_ra_set;
    uint32_t call_setret_pending;
    uint32_t mem_ok;
    uint32_t mem_kind;
    uint32_t mem_index;
    uint64_t pc;
    uint64_t hit;
    uint64_t printed;
    uint64_t count;
    uint64_t envpc;
    uint64_t bpc;
    uint64_t tpc;
    uint64_t cstate;
    uint64_t tgt;
    uint64_t body_tpc;
    uint64_t return_pc;
    uint64_t tp;
    uint64_t mem_base;
    uint64_t mem_addr;
    uint64_t mem_value;
    uint64_t gpr[LINX_GPR_COUNT];
    uint64_t tq[4];
    uint64_t uq[4];
} LinxDebugPcWatchRingEntry;

static LinxDebugPcWatchRingEntry
    linx_debug_pc_watch_ring[LINX_DEBUG_PC_WATCH_RING_MAX];

enum {
    LINX_CALL_TRACE_SETRET = 1,
    LINX_CALL_TRACE_CALL_COMMIT = 2,
    LINX_CALL_TRACE_FENTRY = 3,
    LINX_CALL_TRACE_FRET_STK = 4,
    LINX_CALL_TRACE_ACRE_ENTER = 5,
    LINX_CALL_TRACE_ACRE_STAGED = 6,
};

static const char *const linx_gpr_names[LINX_GPR_COUNT] = {
    "zero", "sp", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "ra", "s0", "s1", "s2", "s3", "s4",
    "s5", "s6", "s7", "s8", "x0", "x1", "x2", "x3",
};

static void linx_fprint_gprs(FILE *f, CPULinxState *env)
{
    for (unsigned i = 0; i < LINX_GPR_COUNT; i++) {
        fprintf(f, " %s=0x%" PRIx64, linx_gpr_names[i], env->gpr[i]);
    }
}

static bool linx_parse_gpr_name(const char *s, unsigned *out)
{
    uint64_t n;

    if (!s || !s[0]) {
        return false;
    }

    for (unsigned i = 0; i < LINX_GPR_COUNT; i++) {
        if (g_ascii_strcasecmp(s, linx_gpr_names[i]) == 0) {
            *out = i;
            return true;
        }
    }

    if ((s[0] == 'r' || s[0] == 'R') &&
        linx_parse_u64(s + 1, &n) && n < LINX_GPR_COUNT) {
        *out = n;
        return true;
    }

    return false;
}

enum {
    LINX_DEBUG_PC_WATCH_DUMP_GPR = 0,
    LINX_DEBUG_PC_WATCH_DUMP_TQ = 1,
    LINX_DEBUG_PC_WATCH_DUMP_UQ = 2,
    LINX_DEBUG_PC_WATCH_DUMP_TP = 3,
};

static bool linx_debug_pc_watch_parse_dump_source(const char *s)
{
    uint64_t n;
    unsigned gpr;

    if (!s || !s[0]) {
        return false;
    }

    if (linx_parse_gpr_name(s, &gpr)) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_GPR;
        linx_debug_pc_watch_dump_index = gpr;
        linx_debug_pc_watch_dump_name = linx_gpr_names[gpr];
        return true;
    }

    if ((g_ascii_strcasecmp(s, "tp") == 0) ||
        (g_ascii_strcasecmp(s, "ssr0") == 0)) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_TP;
        linx_debug_pc_watch_dump_index = 0;
        linx_debug_pc_watch_dump_name = "tp";
        return true;
    }

    if ((s[0] == 't' || s[0] == 'T') &&
        (s[1] == 'q' || s[1] == 'Q') &&
        linx_parse_u64(s + 2, &n) && n < 4) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_TQ;
        linx_debug_pc_watch_dump_index = n;
        linx_debug_pc_watch_dump_name = "tq";
        return true;
    }

    if ((s[0] == 'u' || s[0] == 'U') &&
        (s[1] == 'q' || s[1] == 'Q') &&
        linx_parse_u64(s + 2, &n) && n < 4) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_UQ;
        linx_debug_pc_watch_dump_index = n;
        linx_debug_pc_watch_dump_name = "uq";
        return true;
    }

    if ((s[0] == 't' || s[0] == 'T') &&
        s[1] == '#' &&
        linx_parse_u64(s + 2, &n) && n >= 1 && n <= 4) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_TQ;
        linx_debug_pc_watch_dump_index = n - 1;
        linx_debug_pc_watch_dump_name = "tq";
        return true;
    }

    if ((s[0] == 'u' || s[0] == 'U') &&
        s[1] == '#' &&
        linx_parse_u64(s + 2, &n) && n >= 1 && n <= 4) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_UQ;
        linx_debug_pc_watch_dump_index = n - 1;
        linx_debug_pc_watch_dump_name = "uq";
        return true;
    }

    if ((s[0] == 't' || s[0] == 'T') &&
        linx_parse_u64(s + 1, &n) && n >= 1 && n <= 4) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_TQ;
        linx_debug_pc_watch_dump_index = n - 1;
        linx_debug_pc_watch_dump_name = "tq";
        return true;
    }

    if ((s[0] == 'u' || s[0] == 'U') &&
        linx_parse_u64(s + 1, &n) && n >= 1 && n <= 4) {
        linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_UQ;
        linx_debug_pc_watch_dump_index = n - 1;
        linx_debug_pc_watch_dump_name = "uq";
        return true;
    }

    return false;
}

static bool linx_debug_pc_watch_parse_source_copy(const char *s,
                                                  unsigned *kind,
                                                  unsigned *index,
                                                  const char **name)
{
    const unsigned old_kind = linx_debug_pc_watch_dump_kind;
    const unsigned old_index = linx_debug_pc_watch_dump_index;
    const char *old_name = linx_debug_pc_watch_dump_name;
    const bool ok = linx_debug_pc_watch_parse_dump_source(s);

    if (ok) {
        *kind = linx_debug_pc_watch_dump_kind;
        *index = linx_debug_pc_watch_dump_index;
        *name = linx_debug_pc_watch_dump_name;
    }
    linx_debug_pc_watch_dump_kind = old_kind;
    linx_debug_pc_watch_dump_index = old_index;
    linx_debug_pc_watch_dump_name = old_name;
    return ok;
}

static void linx_debug_pc_watch_parse_dump_sources(const char *s)
{
    char *copy;
    char *saveptr = NULL;
    char *tok;

    if (!s || !s[0]) {
        return;
    }

    copy = g_strdup(s);
    for (tok = strtok_r(copy, ",", &saveptr);
         tok &&
         linx_debug_pc_watch_dump_source_count <
             ARRAY_SIZE(linx_debug_pc_watch_dump_source_kinds);
         tok = strtok_r(NULL, ",", &saveptr)) {
        char *trimmed = g_strstrip(tok);
        if (!trimmed[0]) {
            continue;
        }
        if (linx_debug_pc_watch_parse_dump_source(trimmed)) {
            const unsigned index = linx_debug_pc_watch_dump_source_count++;
            linx_debug_pc_watch_dump_source_kinds[index] =
                linx_debug_pc_watch_dump_kind;
            linx_debug_pc_watch_dump_source_indexes[index] =
                linx_debug_pc_watch_dump_index;
            linx_debug_pc_watch_dump_source_names[index] =
                linx_debug_pc_watch_dump_name;
        }
    }
    g_free(copy);
}

static void linx_debug_pc_watch_parse_dump_offsets(const char *s)
{
    char *copy;
    char *saveptr = NULL;
    char *tok;

    if (!s || !s[0]) {
        return;
    }

    copy = g_strdup(s);
    for (tok = strtok_r(copy, ",", &saveptr);
         tok &&
         linx_debug_pc_watch_dump_offset_count <
             ARRAY_SIZE(linx_debug_pc_watch_dump_offsets);
         tok = strtok_r(NULL, ",", &saveptr)) {
        uint64_t offset;
        char *trimmed = g_strstrip(tok);
        if (!trimmed[0]) {
            continue;
        }
        if (linx_parse_u64(trimmed, &offset)) {
            linx_debug_pc_watch_dump_offsets[
                linx_debug_pc_watch_dump_offset_count++] = offset;
        }
    }
    g_free(copy);
}

static void linx_debug_pc_watch_parse_dump_ptr_offsets(const char *s)
{
    char *copy;
    char *saveptr = NULL;
    char *tok;

    if (!s || !s[0]) {
        return;
    }

    copy = g_strdup(s);
    for (tok = strtok_r(copy, ",", &saveptr);
         tok &&
         linx_debug_pc_watch_dump_ptr_offset_count <
             ARRAY_SIZE(linx_debug_pc_watch_dump_ptr_offsets);
         tok = strtok_r(NULL, ",", &saveptr)) {
        uint64_t offset;
        char *trimmed = g_strstrip(tok);
        if (!trimmed[0]) {
            continue;
        }
        if (linx_parse_u64(trimmed, &offset)) {
            linx_debug_pc_watch_dump_ptr_offsets[
                linx_debug_pc_watch_dump_ptr_offset_count++] = offset;
        }
    }
    g_free(copy);
}

static uint64_t linx_debug_pc_watch_dump_addr_for(CPULinxState *env,
                                                  unsigned kind,
                                                  unsigned index)
{
    switch (kind) {
    case LINX_DEBUG_PC_WATCH_DUMP_TQ:
        return env->tq[index];
    case LINX_DEBUG_PC_WATCH_DUMP_UQ:
        return env->uq[index];
    case LINX_DEBUG_PC_WATCH_DUMP_TP:
        return env->ssr[0];
    case LINX_DEBUG_PC_WATCH_DUMP_GPR:
    default:
        return env->gpr[index];
    }
}

static bool linx_debug_read_guest_u64(CPULinxState *env, uint64_t addr,
                                      uint64_t *value)
{
    CPUState *cs = env_cpu(env);

    *value = 0;
    if (cpu_memory_rw_debug(cs, addr, (uint8_t *)value, sizeof(*value), 0) == 0) {
        return true;
    }
    if ((addr >> 48) == 0xff60u || (addr >> 48) == 0xff80u ||
        (addr >> 48) == 0xffffu) {
        const uint64_t low_alias = addr & UINT64_C(0x7fffffff);

        if (cpu_memory_rw_debug(cs, low_alias, (uint8_t *)value,
                                sizeof(*value), 0) == 0) {
            return true;
        }
    }
    return false;
}

static void linx_debug_pc_watch_dump_words_for_source(CPULinxState *env,
                                                      unsigned kind,
                                                      unsigned index,
                                                      const char *name,
                                                      uint64_t offset)
{
    char label[48];
    uint64_t base = linx_debug_pc_watch_dump_addr_for(env, kind, index);
    uint64_t addr = base + offset;

    if (!base) {
        return;
    }

    if (kind == LINX_DEBUG_PC_WATCH_DUMP_GPR) {
        g_snprintf(label, sizeof(label), "  %s+0x%" PRIx64,
                   name, offset);
    } else if (kind == LINX_DEBUG_PC_WATCH_DUMP_TP) {
        g_snprintf(label, sizeof(label), "  tp+0x%" PRIx64, offset);
    } else {
        g_snprintf(label, sizeof(label), "  %s%u+0x%" PRIx64,
                   name, index, offset);
    }
    linx_debug_dump_guest_units(env, addr,
                                linx_debug_pc_watch_dump_words,
                                label,
                                linx_debug_pc_watch_dump_width);
}

static void linx_debug_pc_watch_dump_ptr_for_source(CPULinxState *env,
                                                    unsigned kind,
                                                    unsigned index,
                                                    const char *name,
                                                    uint64_t offset)
{
    char label[80];
    uint64_t ptr;
    uint64_t base = linx_debug_pc_watch_dump_addr_for(env, kind, index);
    uint64_t slot = base + offset;

    if (!base) {
        return;
    }

    if (!linx_debug_read_guest_u64(env, slot, &ptr)) {
        if (kind == LINX_DEBUG_PC_WATCH_DUMP_GPR) {
            fprintf(stderr, "  %s+0x%" PRIx64 "-><fault> @0x%" PRIx64 "\n",
                    name, offset, slot);
        } else if (kind == LINX_DEBUG_PC_WATCH_DUMP_TP) {
            fprintf(stderr, "  tp+0x%" PRIx64 "-><fault> @0x%" PRIx64 "\n",
                    offset, slot);
        } else {
            fprintf(stderr, "  %s%u+0x%" PRIx64 "-><fault> @0x%" PRIx64 "\n",
                    name, index, offset, slot);
        }
        return;
    }
    if (!ptr) {
        return;
    }

    if (kind == LINX_DEBUG_PC_WATCH_DUMP_GPR) {
        g_snprintf(label, sizeof(label), "  %s+0x%" PRIx64 "->0x%" PRIx64,
                   name, offset, ptr);
    } else if (kind == LINX_DEBUG_PC_WATCH_DUMP_TP) {
        g_snprintf(label, sizeof(label), "  tp+0x%" PRIx64 "->0x%" PRIx64,
                   offset, ptr);
    } else {
        g_snprintf(label, sizeof(label), "  %s%u+0x%" PRIx64 "->0x%" PRIx64,
                   name, index, offset, ptr);
    }
    linx_debug_dump_guest_units(env, ptr,
                                linx_debug_pc_watch_dump_words,
                                label,
                                linx_debug_pc_watch_dump_width);
}

static void linx_debug_pc_watch_dump_words_for_source_offsets(
    CPULinxState *env, unsigned kind, unsigned index, const char *name)
{
    if (linx_debug_pc_watch_dump_offset_count) {
        for (unsigned i = 0; i < linx_debug_pc_watch_dump_offset_count; i++) {
            linx_debug_pc_watch_dump_words_for_source(
                env, kind, index, name, linx_debug_pc_watch_dump_offsets[i]);
        }
    } else {
        linx_debug_pc_watch_dump_words_for_source(
            env, kind, index, name, linx_debug_pc_watch_dump_offset);
    }
    for (unsigned i = 0; i < linx_debug_pc_watch_dump_ptr_offset_count; i++) {
        linx_debug_pc_watch_dump_ptr_for_source(
            env, kind, index, name, linx_debug_pc_watch_dump_ptr_offsets[i]);
    }
}

static inline bool linx_print_insn_count(void)
{
    if (!linx_print_insn_count_inited) {
        const char *v = getenv("LINX_PRINT_INSN_COUNT");
        linx_print_insn_count_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_print_insn_count_inited = true;
    }
    return linx_print_insn_count_enabled;
}

static void linx_pc_sample_init(void)
{
    if (linx_pc_sample_inited) {
        return;
    }

    uint64_t interval = 0;
    const char *interval_s = getenv("LINX_PC_SAMPLE_INTERVAL");
    if (interval_s && interval_s[0] && strcmp(interval_s, "0") != 0 &&
        linx_parse_u64(interval_s, &interval)) {
        linx_pc_sample_interval = interval;
    }

    uint64_t lo = 0;
    uint64_t hi = 0;
    const char *lo_s = getenv("LINX_PC_SAMPLE_FILTER_PC_LO");
    const char *hi_s = getenv("LINX_PC_SAMPLE_FILTER_PC_HI");
    if (lo_s && lo_s[0] && strcmp(lo_s, "0") != 0 &&
        hi_s && hi_s[0] && strcmp(hi_s, "0") != 0 &&
        linx_parse_u64(lo_s, &lo) && linx_parse_u64(hi_s, &hi)) {
        linx_pc_sample_filter_lo = MIN(lo, hi);
        linx_pc_sample_filter_hi = MAX(lo, hi);
        linx_pc_sample_filter_enabled = true;
    }

    linx_pc_sample_inited = true;
}

void HELPER(linx_pc_sample)(CPULinxState *env, uint64_t pc)
{
    linx_pc_sample_init();
    if (linx_pc_sample_interval == 0) {
        return;
    }
    if (linx_pc_sample_filter_enabled &&
        (pc < linx_pc_sample_filter_lo || pc > linx_pc_sample_filter_hi)) {
        return;
    }

    uint64_t bucket = env->insn_count / linx_pc_sample_interval;
    if (bucket == linx_pc_sample_last_bucket) {
        return;
    }
    linx_pc_sample_last_bucket = bucket;

    fprintf(stderr,
            "LINX_PC_SAMPLE count=%" PRIu64
            " pc=0x%" PRIx64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " acr=%u sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64
            "\n",
            env->insn_count, pc, env->bpc, env->body_tpc, env->acr,
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1]);
    fflush(stderr);
}

static void linx_fprint_guest_code_bytes(FILE *f, CPULinxState *env,
                                         const char *label, uint64_t pc,
                                         unsigned count)
{
    uint8_t bytes[32] = { 0 };
    int rc = cpu_memory_rw_debug(env_cpu(env), pc, bytes, count, 0);

    fprintf(f, " %s=0x%" PRIx64 " %s_rc=%d %s_bytes=",
            label, pc, label, rc, label);
    if (rc == 0) {
        for (unsigned i = 0; i < count; i++) {
            fprintf(f, "%02x", bytes[i]);
        }
    } else {
        fputs("<fault>", f);
    }
}

static void linx_fprint_guest_phys_bytes(FILE *f, CPULinxState *env,
                                         const char *label, uint64_t va,
                                         unsigned count)
{
    uint8_t bytes[32] = { 0 };
    CPUState *cs = env_cpu(env);
    hwaddr page = cpu_get_phys_page_debug(cs, (vaddr)va);

    fprintf(f, " %s=0x%" PRIx64, label, va);
    if (page == (hwaddr)-1) {
        fprintf(f, " %s_pa=<fault> %s_phys_bytes=<fault>",
                label, label);
        return;
    }

    hwaddr pa = (page & TARGET_PAGE_MASK) |
                (hwaddr)(va & (TARGET_PAGE_SIZE - 1));
    MemTxResult rc = address_space_read(&address_space_memory, pa,
                                        MEMTXATTRS_UNSPECIFIED, bytes, count);
    fprintf(f, " %s_pa=0x%" HWADDR_PRIx " %s_phys_rc=%d %s_phys_bytes=",
            label, pa, label, (int)rc, label);
    if (rc == MEMTX_OK) {
        for (unsigned i = 0; i < count; i++) {
            fprintf(f, "%02x", bytes[i]);
        }
    } else {
        fputs("<fault>", f);
    }
}

static void linx_heartbeat_init(void)
{
    if (linx_heartbeat_inited) {
        return;
    }

    const char *interval_s = getenv("LINX_HEARTBEAT_INTERVAL");
    if (!interval_s || !interval_s[0] || strcmp(interval_s, "0") == 0) {
        interval_s = getenv("LINX_QEMU_HEARTBEAT_INTERVAL");
    }
    if (interval_s && interval_s[0] && strcmp(interval_s, "0") != 0) {
        uint64_t interval = 0;
        if (linx_parse_u64(interval_s, &interval)) {
            linx_heartbeat_interval = interval;
        }
    }
    linx_heartbeat_regs_enabled =
        linx_env_enabled("LINX_HEARTBEAT_REGS") ||
        linx_env_enabled("LINX_QEMU_HEARTBEAT_REGS");

    const char *code_s = getenv("LINX_HEARTBEAT_CODE_BYTES");
    if (!code_s || !code_s[0] || strcmp(code_s, "0") == 0) {
        code_s = getenv("LINX_QEMU_HEARTBEAT_CODE_BYTES");
    }
    if (code_s && code_s[0] && strcmp(code_s, "0") != 0) {
        uint64_t bytes = 0;
        if (linx_parse_u64(code_s, &bytes) && bytes != 0) {
            linx_heartbeat_dump_code_bytes = MIN((uint64_t)32, bytes);
        }
    }

    const char *warn_s = getenv("LINX_HEARTBEAT_SAME_SITE_WARN");
    if (!warn_s || !warn_s[0] || strcmp(warn_s, "0") == 0) {
        warn_s = getenv("LINX_QEMU_HEARTBEAT_SAME_SITE_WARN");
    }
    if (warn_s && warn_s[0] && strcmp(warn_s, "0") != 0) {
        (void)linx_parse_u64(warn_s, &linx_heartbeat_same_site_warn);
    }

    linx_heartbeat_extended_enabled =
        linx_env_enabled("LINX_HEARTBEAT_EXTENDED") ||
        linx_env_enabled("LINX_QEMU_HEARTBEAT_EXTENDED") ||
        linx_env_enabled("LINX_MMU_CACHE_STATS") ||
        linx_env_enabled("LINX_QEMU_MMU_CACHE_STATS") ||
        linx_env_enabled("LINX_TLB_STATS") ||
        linx_env_enabled("LINX_QEMU_TLB_STATS") ||
        linx_env_enabled("LINX_TLB_FILL_STATS") ||
        linx_env_enabled("LINX_QEMU_TLB_FILL_STATS");

    linx_heartbeat_inited = true;
}

static inline bool linx_frame_stats_env_enabled(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

static inline bool linx_frame_stats_enabled_p(void)
{
    if (!linx_frame_stats_inited) {
        linx_frame_stats_enabled =
            linx_frame_stats_env_enabled("LINX_QEMU_FRAME_STATS") ||
            linx_frame_stats_env_enabled("LINX_FRAME_STATS");
        linx_frame_stats_inited = true;
    }
    return linx_frame_stats_enabled;
}

static inline bool linx_frame_restore_host_load_enabled_p(void)
{
    if (!linx_frame_restore_host_load_inited) {
        linx_frame_restore_host_load_enabled =
            linx_frame_stats_env_enabled("LINX_QEMU_FRAME_RESTORE_HOST_LOAD") ||
            linx_frame_stats_env_enabled("LINX_FRAME_RESTORE_HOST_LOAD");
        linx_frame_restore_host_load_inited = true;
    }
    return linx_frame_restore_host_load_enabled;
}

static inline bool linx_frame_single_restore_host_load_enabled_p(void)
{
    if (!linx_frame_single_restore_host_load_inited) {
        linx_frame_single_restore_host_load_enabled =
            linx_frame_stats_env_enabled("LINX_QEMU_FRAME_SINGLE_RESTORE_HOST_LOAD") ||
            linx_frame_stats_env_enabled("LINX_FRAME_SINGLE_RESTORE_HOST_LOAD");
        linx_frame_single_restore_host_load_inited = true;
    }
    return linx_frame_single_restore_host_load_enabled;
}

static inline bool linx_frame_restore_host_verify_enabled_p(void)
{
    if (!linx_frame_restore_host_verify_inited) {
        const char *limit_s;

        linx_frame_restore_host_verify_enabled =
            linx_frame_stats_env_enabled("LINX_QEMU_FRAME_RESTORE_HOST_VERIFY") ||
            linx_frame_stats_env_enabled("LINX_FRAME_RESTORE_HOST_VERIFY");
        limit_s = getenv("LINX_QEMU_FRAME_RESTORE_HOST_VERIFY_LIMIT");
        if (!limit_s || !limit_s[0]) {
            limit_s = getenv("LINX_FRAME_RESTORE_HOST_VERIFY_LIMIT");
        }
        if (limit_s && limit_s[0]) {
            (void)linx_parse_u64(limit_s,
                                 &linx_frame_restore_host_verify_emit_limit);
        } else {
            linx_frame_restore_host_verify_emit_limit = 16;
        }
        linx_frame_restore_host_verify_inited = true;
    }
    return linx_frame_restore_host_verify_enabled;
}

static const char *linx_frame_shape_kind_name(unsigned kind)
{
    switch ((LinxTemplateKind)kind) {
    case LINX_TEMPLATE_FENTRY:
        return "fentry";
    case LINX_TEMPLATE_FEXIT:
        return "fexit";
    case LINX_TEMPLATE_FRET_RA:
        return "fret_ra";
    case LINX_TEMPLATE_FRET_STK:
        return "fret_stk";
    default:
        return "unknown";
    }
}

static inline bool linx_frame_shape_hot_enabled_p(void)
{
    if (!linx_frame_shape_hot_inited) {
        linx_frame_shape_hot_enabled =
            linx_frame_stats_env_enabled("LINX_QEMU_FRAME_SHAPE_HOT") ||
            linx_frame_stats_env_enabled("LINX_FRAME_SHAPE_HOT");
        linx_frame_shape_hot_inited = true;
    }
    return linx_frame_shape_hot_enabled;
}

static inline bool linx_frame_single_reg_fast_enabled_p(void)
{
    if (!linx_frame_single_reg_fast_inited) {
        linx_frame_single_reg_fast_enabled =
            linx_frame_stats_env_enabled("LINX_QEMU_FRAME_SINGLE_REG_FAST") ||
            linx_frame_stats_env_enabled("LINX_FRAME_SINGLE_REG_FAST");
        linx_frame_single_reg_fast_inited = true;
    }
    return linx_frame_single_reg_fast_enabled;
}

static inline bool linx_frame_page_fast_enabled_p(void)
{
    if (!linx_frame_page_fast_inited) {
        linx_frame_page_fast_enabled =
            linx_frame_stats_env_enabled("LINX_QEMU_FRAME_PAGE_FAST") ||
            linx_frame_stats_env_enabled("LINX_FRAME_PAGE_FAST");
        linx_frame_page_fast_inited = true;
    }
    return linx_frame_page_fast_enabled;
}

static inline bool linx_frame_single_reg_fast_shape(uint32_t reg_begin,
                                                    uint32_t reg_end,
                                                    uint64_t stacksize)
{
    return reg_begin == reg_end &&
           reg_begin != LINX_REG_ZERO &&
           reg_begin < LINX_GPR_COUNT &&
           stacksize >= 8;
}

static inline bool linx_frame_dense_reg_range(int begin, int end, int count)
{
    return begin >= 2 && begin < LINX_GPR_COUNT &&
           end >= 2 && end < LINX_GPR_COUNT &&
           count > 1 && count <= (LINX_GPR_COUNT - 2);
}

static inline bool linx_frame_page_fast_shape(int begin, int end,
                                              uint64_t stacksize, int count)
{
    return linx_frame_page_fast_enabled_p() &&
           linx_frame_dense_reg_range(begin, end, count) &&
           stacksize >= ((uint64_t)count * 8ull);
}

static inline bool linx_frame_range_one_page(uint64_t low, uint64_t bytes)
{
    return bytes > 0 &&
           bytes <= TARGET_PAGE_SIZE &&
           ((low ^ (low + bytes - 1)) & TARGET_PAGE_MASK) == 0;
}

static void linx_frame_shape_hot_record_slow(CPULinxState *env,
                                             LinxTemplateKind kind,
                                             unsigned begin, unsigned end,
                                             uint64_t stacksize,
                                             unsigned frame_slots)
{
    int found = -1;
    int empty = -1;
    int min_slot = 0;
    uint64_t min_count = UINT64_MAX;

    env->frame_shape_hot_active = 1;
    for (unsigned i = 0; i < LINX_FRAME_SHAPE_HOT_SLOTS; i++) {
        if (!env->frame_shape_hot_valid[i]) {
            if (empty < 0) {
                empty = (int)i;
            }
            continue;
        }
        if (env->frame_shape_hot_kind[i] == (uint8_t)kind &&
            env->frame_shape_hot_begin[i] == (uint8_t)begin &&
            env->frame_shape_hot_end[i] == (uint8_t)end &&
            env->frame_shape_hot_stacksize[i] == stacksize) {
            found = (int)i;
            break;
        }
        if (env->frame_shape_hot_count[i] < min_count) {
            min_count = env->frame_shape_hot_count[i];
            min_slot = (int)i;
        }
    }

    int slot = found;
    if (slot < 0) {
        slot = empty >= 0 ? empty : min_slot;
        if (empty < 0) {
            env->frame_shape_hot_evictions++;
        }
        env->frame_shape_hot_valid[slot] = 1;
        env->frame_shape_hot_count[slot] = 0;
        env->frame_shape_hot_emit_count[slot] = 0;
        env->frame_shape_hot_frame_slots[slot] = 0;
        env->frame_shape_hot_kind[slot] = (uint8_t)kind;
        env->frame_shape_hot_begin[slot] = (uint8_t)begin;
        env->frame_shape_hot_end[slot] = (uint8_t)end;
        env->frame_shape_hot_stacksize[slot] = stacksize;
        env->frame_shape_hot_reg_count[slot] = (uint8_t)MIN(frame_slots, 255u);
    }

    env->frame_shape_hot_count[slot]++;
    env->frame_shape_hot_frame_slots[slot] += frame_slots;
}

static inline void linx_frame_shape_hot_record(CPULinxState *env,
                                               LinxTemplateKind kind,
                                               unsigned begin, unsigned end,
                                               uint64_t stacksize,
                                               unsigned frame_slots)
{
    if (unlikely(linx_frame_shape_hot_enabled_p())) {
        linx_frame_shape_hot_record_slow(env, kind, begin, end, stacksize,
                                         frame_slots);
    }
}

static inline void linx_frame_stats_emit_heartbeat(void)
{
    if (!linx_frame_stats_enabled_p()) {
        return;
    }

    fprintf(stderr,
            " fr_fentry=%" PRIu64
            " fr_save_probe=%" PRIu64
            " fr_save_slot=%" PRIu64
            " fr_save_host=%" PRIu64
            " fr_save_fallback=%" PRIu64
            " fr_fexit=%" PRIu64
            " fr_fret_stk=%" PRIu64
            " fr_fret_ra=%" PRIu64
            " fr_restore_slot=%" PRIu64
            " fr_restore_host=%" PRIu64
            " fr_restore_fallback=%" PRIu64
            " fr_restore_verify=%" PRIu64
            " fr_restore_mismatch=%" PRIu64
            " fr_ret_fast=%" PRIu64
            " fr_ret_check=%" PRIu64
            " fr_single_fast_fentry=%" PRIu64
            " fr_single_fast_fret_stk=%" PRIu64
            " fr_page_fast_fentry=%" PRIu64
            " fr_page_fast_restore=%" PRIu64,
            linx_frame_stat_fentry_calls,
            linx_frame_stat_fentry_save_probes,
            linx_frame_stat_fentry_save_slots,
            linx_frame_stat_fentry_host_stores,
            linx_frame_stat_fentry_fallback_stores,
            linx_frame_stat_fexit_calls,
            linx_frame_stat_fret_stk_calls,
            linx_frame_stat_fret_ra_calls,
            linx_frame_stat_restore_slots,
            linx_frame_stat_restore_host_loads,
            linx_frame_stat_restore_fallback_loads,
            linx_frame_stat_restore_host_verify_loads,
            linx_frame_stat_restore_host_verify_mismatches,
            linx_frame_stat_ret_fast_hits,
            linx_frame_stat_ret_checks,
            linx_frame_stat_single_fast_fentry,
            linx_frame_stat_single_fast_fret_stk,
            linx_frame_stat_page_fast_fentry,
            linx_frame_stat_page_fast_restore);
}

static void linx_heartbeat_emit_frame_shape_hot(CPULinxState *env)
{
    if (!env->frame_shape_hot_active) {
        return;
    }

    int top0 = -1;
    int top1 = -1;
    for (unsigned i = 0; i < LINX_FRAME_SHAPE_HOT_SLOTS; i++) {
        if (!env->frame_shape_hot_valid[i]) {
            continue;
        }
        const uint64_t delta =
            env->frame_shape_hot_count[i] - env->frame_shape_hot_emit_count[i];
        const uint64_t top0_delta =
            top0 >= 0 ? env->frame_shape_hot_count[top0] -
                        env->frame_shape_hot_emit_count[top0] : 0;
        const uint64_t top1_delta =
            top1 >= 0 ? env->frame_shape_hot_count[top1] -
                        env->frame_shape_hot_emit_count[top1] : 0;
        if (top0 < 0 || delta > top0_delta) {
            top1 = top0;
            top0 = (int)i;
        } else if (top1 < 0 || delta > top1_delta) {
            top1 = (int)i;
        }
    }

    const uint64_t top0_count = top0 >= 0 ? env->frame_shape_hot_count[top0] : 0;
    const uint64_t top0_delta =
        top0 >= 0 ? env->frame_shape_hot_count[top0] -
                    env->frame_shape_hot_emit_count[top0] : 0;
    const uint64_t top0_stack =
        top0 >= 0 ? env->frame_shape_hot_stacksize[top0] : 0;
    const uint64_t top0_frame_slots =
        top0 >= 0 ? env->frame_shape_hot_frame_slots[top0] : 0;
    const unsigned top0_kind =
        top0 >= 0 ? env->frame_shape_hot_kind[top0] : LINX_TEMPLATE_FENTRY;
    const unsigned top0_begin =
        top0 >= 0 ? env->frame_shape_hot_begin[top0] : 0;
    const unsigned top0_end =
        top0 >= 0 ? env->frame_shape_hot_end[top0] : 0;
    const unsigned top0_regs =
        top0 >= 0 ? env->frame_shape_hot_reg_count[top0] : 0;

    const uint64_t top1_count = top1 >= 0 ? env->frame_shape_hot_count[top1] : 0;
    const uint64_t top1_delta =
        top1 >= 0 ? env->frame_shape_hot_count[top1] -
                    env->frame_shape_hot_emit_count[top1] : 0;
    const uint64_t top1_stack =
        top1 >= 0 ? env->frame_shape_hot_stacksize[top1] : 0;
    const uint64_t top1_frame_slots =
        top1 >= 0 ? env->frame_shape_hot_frame_slots[top1] : 0;
    const unsigned top1_kind =
        top1 >= 0 ? env->frame_shape_hot_kind[top1] : LINX_TEMPLATE_FENTRY;
    const unsigned top1_begin =
        top1 >= 0 ? env->frame_shape_hot_begin[top1] : 0;
    const unsigned top1_end =
        top1 >= 0 ? env->frame_shape_hot_end[top1] : 0;
    const unsigned top1_regs =
        top1 >= 0 ? env->frame_shape_hot_reg_count[top1] : 0;

    fprintf(stderr,
            "LINX_FRAME_SHAPE_HOT count=%" PRIu64
            " evictions=%" PRIu64
            " slots=%u"
            " top0_count=%" PRIu64
            " top0_delta=%" PRIu64
            " top0_kind=%s top0_kindid=%u"
            " top0_begin=%u top0_end=%u"
            " top0_stack=%" PRIu64
            " top0_regs=%u"
            " top0_frame_slots=%" PRIu64
            " top1_count=%" PRIu64
            " top1_delta=%" PRIu64
            " top1_kind=%s top1_kindid=%u"
            " top1_begin=%u top1_end=%u"
            " top1_stack=%" PRIu64
            " top1_regs=%u"
            " top1_frame_slots=%" PRIu64
            "\n",
            env->insn_count, env->frame_shape_hot_evictions,
            LINX_FRAME_SHAPE_HOT_SLOTS,
            top0_count, top0_delta, linx_frame_shape_kind_name(top0_kind),
            top0_kind, top0_begin, top0_end, top0_stack, top0_regs,
            top0_frame_slots,
            top1_count, top1_delta, linx_frame_shape_kind_name(top1_kind),
            top1_kind, top1_begin, top1_end, top1_stack, top1_regs,
            top1_frame_slots);

    for (unsigned i = 0; i < LINX_FRAME_SHAPE_HOT_SLOTS; i++) {
        if (env->frame_shape_hot_valid[i]) {
            env->frame_shape_hot_emit_count[i] =
                env->frame_shape_hot_count[i];
        }
    }
}

static inline void linx_tcg_tb_stats_emit_heartbeat(void)
{
    LinxTcgTBStats stats;

    linx_tcg_tb_stats_snapshot(&stats);
    if (stats.lookup == 0 && stats.exec == 0 && stats.gen == 0) {
        return;
    }

    fprintf(stderr,
            " tbs_exec=%" PRIu64
            " tbs_lookup=%" PRIu64
            " tbs_jmp_hit=%" PRIu64
            " tbs_hash_hit=%" PRIu64
            " tbs_miss=%" PRIu64
            " tbs_gen=%" PRIu64
            " tbs_flush=%" PRIu64
            " tbs_phys_inv=%" PRIu64
            " tbs_code_used=%" PRIu64
            " tbs_code_size=%" PRIu64,
            stats.exec, stats.lookup, stats.jmp_hit, stats.hash_hit,
            stats.miss, stats.gen, stats.flush, stats.phys_invalidate,
            stats.code_used, stats.code_size);
}

static void linx_tcg_tb_hot_emit_entry(const char *prefix,
                                       const LinxTcgTBHotEntry *entry)
{
    fprintf(stderr,
            " %s_pc=0x%" PRIx64
            " %s_lookup=%" PRIu64
            " %s_delta=%" PRIu64
            " %s_jmp=%" PRIu64
            " %s_hash=%" PRIu64
            " %s_miss=%" PRIu64,
            prefix, entry->pc,
            prefix, entry->lookup,
            prefix, entry->delta,
            prefix, entry->jmp_hit,
            prefix, entry->hash_hit,
            prefix, entry->miss);
}

static inline void linx_tcg_tb_hot_emit_heartbeat(CPULinxState *env)
{
    LinxTcgTBHotStats stats;

    linx_tcg_tb_hot_snapshot(&stats);
    if (!stats.seen) {
        return;
    }

    fprintf(stderr,
            "LINX_TB_HOT count=%" PRIu64
            " evictions=%" PRIu64
            " slots=%" PRIu64,
            env->insn_count, stats.evictions, stats.slots);
    linx_tcg_tb_hot_emit_entry("top0", &stats.top0);
    linx_tcg_tb_hot_emit_entry("top1", &stats.top1);
    fputc('\n', stderr);
}

static uint64_t linx_heartbeat_next_count(uint64_t bucket)
{
    if (linx_heartbeat_interval == 0 ||
        bucket >= UINT64_MAX / linx_heartbeat_interval) {
        return UINT64_MAX;
    }
    return (bucket + 1) * linx_heartbeat_interval;
}

static void linx_heartbeat_emit_tlb_fill_hot(CPULinxState *env)
{
    if (!env->tlb_fill_hot_active) {
        return;
    }

    int top0 = -1;
    int top1 = -1;
    for (unsigned i = 0; i < LINX_TLB_FILL_HOT_SLOTS; i++) {
        if (!env->tlb_fill_hot_valid[i]) {
            continue;
        }
        if (top0 < 0 ||
            env->tlb_fill_hot_count[i] > env->tlb_fill_hot_count[top0]) {
            top1 = top0;
            top0 = (int)i;
        } else if (top1 < 0 ||
                   env->tlb_fill_hot_count[i] > env->tlb_fill_hot_count[top1]) {
            top1 = (int)i;
        }
    }

    const uint64_t top0_count = top0 >= 0 ? env->tlb_fill_hot_count[top0] : 0;
    const uint64_t top0_page = top0 >= 0 ? env->tlb_fill_hot_page[top0] : 0;
    const uint64_t top0_va = top0 >= 0 ? env->tlb_fill_hot_last_va[top0] : 0;
    const uint64_t top0_pa = top0 >= 0 ? env->tlb_fill_hot_last_pa[top0] : 0;
    const uint64_t top0_pc = top0 >= 0 ? env->tlb_fill_hot_last_pc[top0] : 0;
    const uint64_t top0_bpc = top0 >= 0 ? env->tlb_fill_hot_last_bpc[top0] : 0;
    const unsigned top0_access = top0 >= 0 ? env->tlb_fill_hot_access[top0] : 0;
    const unsigned top0_mmu = top0 >= 0 ? env->tlb_fill_hot_mmu[top0] : 0;
    const unsigned top0_probe = top0 >= 0 ? env->tlb_fill_hot_probe[top0] : 0;
    const unsigned top0_prot = top0 >= 0 ? env->tlb_fill_hot_prot[top0] : 0;
    const unsigned top0_cause = top0 >= 0 ? env->tlb_fill_hot_cause[top0] : 0;
    const unsigned top0_acr = top0 >= 0 ? env->tlb_fill_hot_acr[top0] : 0;

    const uint64_t top1_count = top1 >= 0 ? env->tlb_fill_hot_count[top1] : 0;
    const uint64_t top1_page = top1 >= 0 ? env->tlb_fill_hot_page[top1] : 0;
    const uint64_t top1_va = top1 >= 0 ? env->tlb_fill_hot_last_va[top1] : 0;
    const uint64_t top1_pa = top1 >= 0 ? env->tlb_fill_hot_last_pa[top1] : 0;
    const uint64_t top1_pc = top1 >= 0 ? env->tlb_fill_hot_last_pc[top1] : 0;
    const uint64_t top1_bpc = top1 >= 0 ? env->tlb_fill_hot_last_bpc[top1] : 0;
    const unsigned top1_access = top1 >= 0 ? env->tlb_fill_hot_access[top1] : 0;
    const unsigned top1_mmu = top1 >= 0 ? env->tlb_fill_hot_mmu[top1] : 0;
    const unsigned top1_probe = top1 >= 0 ? env->tlb_fill_hot_probe[top1] : 0;
    const unsigned top1_prot = top1 >= 0 ? env->tlb_fill_hot_prot[top1] : 0;
    const unsigned top1_cause = top1 >= 0 ? env->tlb_fill_hot_cause[top1] : 0;
    const unsigned top1_acr = top1 >= 0 ? env->tlb_fill_hot_acr[top1] : 0;

    fprintf(stderr,
            "LINX_TLB_FILL_HOT count=%" PRIu64
            " evictions=%" PRIu64
            " inserts=%" PRIu64
            " last_hits=%" PRIu64
            " slot_hits=%" PRIu64
            " slots=%u"
            " top0_count=%" PRIu64
            " top0_page=0x%" PRIx64
            " top0_last_va=0x%" PRIx64
            " top0_last_pa=0x%" PRIx64
            " top0_access=%u top0_mmu=%u top0_probe=%u"
            " top0_prot=0x%x top0_cause=0x%x top0_acr=%u"
            " top0_pc=0x%" PRIx64
            " top0_bpc=0x%" PRIx64
            " top1_count=%" PRIu64
            " top1_page=0x%" PRIx64
            " top1_last_va=0x%" PRIx64
            " top1_last_pa=0x%" PRIx64
            " top1_access=%u top1_mmu=%u top1_probe=%u"
            " top1_prot=0x%x top1_cause=0x%x top1_acr=%u"
            " top1_pc=0x%" PRIx64
            " top1_bpc=0x%" PRIx64
            "\n",
            env->insn_count, env->tlb_fill_hot_evictions,
            env->tlb_fill_hot_inserts, env->tlb_fill_hot_last_hits,
            env->tlb_fill_hot_slot_hits,
            LINX_TLB_FILL_HOT_SLOTS,
            top0_count, top0_page, top0_va, top0_pa,
            top0_access, top0_mmu, top0_probe, top0_prot, top0_cause,
            top0_acr, top0_pc, top0_bpc,
            top1_count, top1_page, top1_va, top1_pa,
            top1_access, top1_mmu, top1_probe, top1_prot, top1_cause,
            top1_acr, top1_pc, top1_bpc);
}

static const char *linx_tlb_inv_op_name(unsigned op)
{
    switch ((LinxTlbInvOp)op) {
    case LINX_TLB_INV_IALL:
        return "iall";
    case LINX_TLB_INV_IA:
        return "ia";
    case LINX_TLB_INV_IV:
        return "iv";
    case LINX_TLB_INV_IAV:
        return "iav";
    }
    return "unknown";
}

static void linx_heartbeat_emit_tlb_inv_hot(CPULinxState *env)
{
    if (!env->tlb_inv_hot_active) {
        return;
    }

    int top0 = -1;
    int top1 = -1;
    for (unsigned i = 0; i < LINX_TLB_INV_HOT_SLOTS; i++) {
        if (!env->tlb_inv_hot_valid[i]) {
            continue;
        }
        const uint64_t delta =
            env->tlb_inv_hot_count[i] - env->tlb_inv_hot_emit_count[i];
        const uint64_t top0_delta =
            top0 >= 0 ? env->tlb_inv_hot_count[top0] -
                        env->tlb_inv_hot_emit_count[top0] : 0;
        const uint64_t top1_delta =
            top1 >= 0 ? env->tlb_inv_hot_count[top1] -
                        env->tlb_inv_hot_emit_count[top1] : 0;
        if (top0 < 0 || delta > top0_delta) {
            top1 = top0;
            top0 = (int)i;
        } else if (top1 < 0 || delta > top1_delta) {
            top1 = (int)i;
        }
    }

    const uint64_t top0_count = top0 >= 0 ? env->tlb_inv_hot_count[top0] : 0;
    const uint64_t top0_delta =
        top0 >= 0 ? env->tlb_inv_hot_count[top0] -
                    env->tlb_inv_hot_emit_count[top0] : 0;
    const uint64_t top0_pc = top0 >= 0 ? env->tlb_inv_hot_pc[top0] : 0;
    const uint64_t top0_bpc = top0 >= 0 ? env->tlb_inv_hot_last_bpc[top0] : 0;
    const uint64_t top0_operand =
        top0 >= 0 ? env->tlb_inv_hot_last_operand[top0] : 0;
    const uint64_t top0_page =
        top0 >= 0 ? env->tlb_inv_hot_last_page[top0] : 0;
    const unsigned top0_op = top0 >= 0 ? env->tlb_inv_hot_op[top0] : 0;
    const unsigned top0_acr = top0 >= 0 ? env->tlb_inv_hot_acr[top0] : 0;

    const uint64_t top1_count = top1 >= 0 ? env->tlb_inv_hot_count[top1] : 0;
    const uint64_t top1_delta =
        top1 >= 0 ? env->tlb_inv_hot_count[top1] -
                    env->tlb_inv_hot_emit_count[top1] : 0;
    const uint64_t top1_pc = top1 >= 0 ? env->tlb_inv_hot_pc[top1] : 0;
    const uint64_t top1_bpc = top1 >= 0 ? env->tlb_inv_hot_last_bpc[top1] : 0;
    const uint64_t top1_operand =
        top1 >= 0 ? env->tlb_inv_hot_last_operand[top1] : 0;
    const uint64_t top1_page =
        top1 >= 0 ? env->tlb_inv_hot_last_page[top1] : 0;
    const unsigned top1_op = top1 >= 0 ? env->tlb_inv_hot_op[top1] : 0;
    const unsigned top1_acr = top1 >= 0 ? env->tlb_inv_hot_acr[top1] : 0;

    fprintf(stderr,
            "LINX_TLB_INV_HOT count=%" PRIu64
            " evictions=%" PRIu64
            " slots=%u"
            " top0_count=%" PRIu64
            " top0_delta=%" PRIu64
            " top0_op=%s top0_opid=%u"
            " top0_pc=0x%" PRIx64
            " top0_bpc=0x%" PRIx64
            " top0_operand=0x%" PRIx64
            " top0_page=0x%" PRIx64
            " top0_acr=%u"
            " top1_count=%" PRIu64
            " top1_delta=%" PRIu64
            " top1_op=%s top1_opid=%u"
            " top1_pc=0x%" PRIx64
            " top1_bpc=0x%" PRIx64
            " top1_operand=0x%" PRIx64
            " top1_page=0x%" PRIx64
            " top1_acr=%u"
            "\n",
            env->insn_count, env->tlb_inv_hot_evictions,
            LINX_TLB_INV_HOT_SLOTS,
            top0_count, top0_delta, linx_tlb_inv_op_name(top0_op), top0_op,
            top0_pc, top0_bpc, top0_operand, top0_page, top0_acr,
            top1_count, top1_delta, linx_tlb_inv_op_name(top1_op), top1_op,
            top1_pc, top1_bpc, top1_operand, top1_page, top1_acr);

    for (unsigned i = 0; i < LINX_TLB_INV_HOT_SLOTS; i++) {
        if (env->tlb_inv_hot_valid[i]) {
            env->tlb_inv_hot_emit_count[i] = env->tlb_inv_hot_count[i];
        }
    }
}

void HELPER(linx_heartbeat)(CPULinxState *env, uint64_t pc)
{
    linx_heartbeat_init();
    if (linx_heartbeat_interval == 0) {
        env->heartbeat_next_count = UINT64_MAX;
        return;
    }

    uint64_t bucket = env->insn_count / linx_heartbeat_interval;
    const uint64_t next_count = linx_heartbeat_next_count(bucket);
    const bool have_previous = linx_heartbeat_last_bucket != UINT64_MAX;
    if (bucket == linx_heartbeat_last_bucket) {
        env->heartbeat_next_count = next_count;
        return;
    }
    env->heartbeat_next_count = next_count;

    const bool same_site =
        have_previous &&
        pc == linx_heartbeat_last_pc &&
        env->bpc == linx_heartbeat_last_bpc &&
        env->body_tpc == linx_heartbeat_last_tpc;
    const char *progress =
        !have_previous ? "first" :
        same_site ? "same-site" : "site-change";

    if (same_site) {
        linx_heartbeat_same_site_repeats++;
    } else {
        linx_heartbeat_same_site_repeats = 0;
        linx_heartbeat_same_site_reported = false;
    }

    const uint64_t last_count = linx_heartbeat_last_count;
    const uint64_t delta = have_previous ? env->insn_count - last_count : 0;
    linx_heartbeat_last_bucket = bucket;
    linx_heartbeat_last_count = env->insn_count;
    linx_heartbeat_last_pc = pc;
    linx_heartbeat_last_bpc = env->bpc;
    linx_heartbeat_last_tpc = env->body_tpc;

    fprintf(stderr,
            "LINX_HEARTBEAT host_ms=%" PRId64
            " count=%" PRIu64
            " delta=%" PRIu64
            " pc=0x%" PRIx64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " envpc=0x%" PRIx64
            " acr=%u cstate=0x%" PRIx64
            " brtype=%u tgt=0x%" PRIx64
            " in_body=%u progress=%s same_site=%" PRIu64,
            qemu_clock_get_ms(QEMU_CLOCK_REALTIME),
            env->insn_count, delta, pc, env->bpc, env->body_tpc,
            env->pc, env->acr & 0xFu, env->ssr[0x20],
            env->brtype, env->tgt, env->in_body,
            progress, linx_heartbeat_same_site_repeats);
    if (linx_heartbeat_extended_enabled) {
        fprintf(stderr,
            " mmuc_hit=%" PRIu64
            " mmuc_miss=%" PRIu64
            " mmuc_fill=%" PRIu64
            " mmuc_flush=%" PRIu64
            " mmuc_flush_page=%" PRIu64
            " mmuc_col=%" PRIu64
            " mmuc_vhit=%" PRIu64
            " mmuc_vfill=%" PRIu64
            " mmuc_hit4k=%" PRIu64
            " mmuc_hit2m=%" PRIu64
            " mmuc_hit1g=%" PRIu64
            " mmuc_hit512g=%" PRIu64
            " mmuc_fill4k=%" PRIu64
            " mmuc_fill2m=%" PRIu64
            " mmuc_fill1g=%" PRIu64
            " mmuc_fill512g=%" PRIu64
            " mmuc_col4k=%" PRIu64
            " mmuc_col2m=%" PRIu64
            " mmuc_col1g=%" PRIu64
            " mmuc_col512g=%" PRIu64
            " tlbi_iall=%" PRIu64
            " tlbi_ia=%" PRIu64
            " tlbi_iv=%" PRIu64
            " tlbi_iav=%" PRIu64
            " tlbi_last_count=%" PRIu64
            " tlbi_last_pc=0x%" PRIx64
            " tlbi_last_bpc=0x%" PRIx64
            " tlbi_last_operand=0x%" PRIx64
            " tlbi_last_acr=%u"
            " tlbf_total=%" PRIu64
            " tlbf_fetch=%" PRIu64
            " tlbf_load=%" PRIu64
            " tlbf_store=%" PRIu64
            " tlbf_probe=%" PRIu64
            " tlbf_ok=%" PRIu64
            " tlbf_fault=%" PRIu64
            " tlbf_user=%" PRIu64
            " tlbf_user_fetch=%" PRIu64
            " tlbf_user_load=%" PRIu64
            " tlbf_user_store=%" PRIu64
            " tlbf_kernel=%" PRIu64
            " tlbf_kernel_fetch=%" PRIu64
            " tlbf_kernel_load=%" PRIu64
            " tlbf_kernel_store=%" PRIu64
            " tlbf_other=%" PRIu64
            " tlbf_last_count=%" PRIu64
            " tlbf_last_pc=0x%" PRIx64
            " tlbf_last_bpc=0x%" PRIx64
            " tlbf_last_va=0x%" PRIx64
            " tlbf_last_pa=0x%" PRIx64
            " tlbf_last_access=%u"
            " tlbf_last_mmu=%u"
            " tlbf_last_prot=0x%x"
            " tlbf_last_cause=0x%x"
            " tlbf_last_acr=%u"
            " sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " tp=0x%" PRIx64
            " etemp1=0x%" PRIx64
            " etemp0_1=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64
            " a2=0x%" PRIx64
            " a3=0x%" PRIx64
            " a4=0x%" PRIx64
            " a5=0x%" PRIx64
            " a6=0x%" PRIx64
            " a7=0x%" PRIx64,
            env->mmu_cache_hits, env->mmu_cache_misses,
            env->mmu_cache_fills, env->mmu_cache_flushes,
            env->mmu_cache_page_flushes,
            env->mmu_cache_collisions,
            env->mmu_cache_victim_hits,
            env->mmu_cache_victim_fills,
            env->mmu_cache_hit_4k, env->mmu_cache_hit_2m,
            env->mmu_cache_hit_1g, env->mmu_cache_hit_512g,
            env->mmu_cache_fill_4k, env->mmu_cache_fill_2m,
            env->mmu_cache_fill_1g, env->mmu_cache_fill_512g,
            env->mmu_cache_collision_4k, env->mmu_cache_collision_2m,
            env->mmu_cache_collision_1g, env->mmu_cache_collision_512g,
            env->tlb_inv_iall, env->tlb_inv_ia,
            env->tlb_inv_iv, env->tlb_inv_iav,
            env->tlb_inv_last_count, env->tlb_inv_last_pc,
            env->tlb_inv_last_bpc, env->tlb_inv_last_operand,
            env->tlb_inv_last_acr,
            env->tlb_fill_total, env->tlb_fill_fetch,
            env->tlb_fill_load, env->tlb_fill_store,
            env->tlb_fill_probe, env->tlb_fill_ok,
            env->tlb_fill_fault, env->tlb_fill_user,
            env->tlb_fill_user_fetch, env->tlb_fill_user_load,
            env->tlb_fill_user_store, env->tlb_fill_kernel,
            env->tlb_fill_kernel_fetch, env->tlb_fill_kernel_load,
            env->tlb_fill_kernel_store, env->tlb_fill_other,
            env->tlb_fill_last_count,
            env->tlb_fill_last_pc, env->tlb_fill_last_bpc,
            env->tlb_fill_last_va, env->tlb_fill_last_pa,
            env->tlb_fill_last_access, env->tlb_fill_last_mmu_idx,
            env->tlb_fill_last_prot, env->tlb_fill_last_cause,
            env->tlb_fill_last_acr,
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->ssr[0x0000], env->ssr_acr[1][0xF05],
            env->ssr_acr[1][0xF06],
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1],
            env->gpr[LINX_REG_A2], env->gpr[LINX_REG_A3],
            env->gpr[LINX_REG_A4], env->gpr[LINX_REG_A5],
            env->gpr[LINX_REG_A6], env->gpr[LINX_REG_A7]);
    }
    linx_frame_stats_emit_heartbeat();
    linx_tcg_tb_stats_emit_heartbeat();
    fprintf(stderr, "\n");
    linx_heartbeat_emit_tlb_fill_hot(env);
    linx_heartbeat_emit_tlb_inv_hot(env);
    linx_heartbeat_emit_frame_shape_hot(env);
    linx_tcg_tb_hot_emit_heartbeat(env);
    if (linx_heartbeat_regs_enabled) {
        fprintf(stderr,
                "LINX_HEARTBEAT_REGS count=%" PRIu64
                " pc=0x%" PRIx64
                " bpc=0x%" PRIx64
                " tpc=0x%" PRIx64,
                env->insn_count, pc, env->bpc, env->body_tpc);
        linx_fprint_gprs(stderr, env);
        fputc('\n', stderr);
    }
    if (linx_heartbeat_dump_code_bytes) {
        fprintf(stderr,
                "LINX_HEARTBEAT_CODE count=%" PRIu64,
                env->insn_count);
        linx_fprint_guest_code_bytes(stderr, env, "pc", pc,
                                     linx_heartbeat_dump_code_bytes);
        linx_fprint_guest_code_bytes(stderr, env, "bpc", env->bpc,
                                     linx_heartbeat_dump_code_bytes);
        fputc('\n', stderr);
    }
    if (linx_heartbeat_same_site_warn &&
        linx_heartbeat_same_site_repeats >= linx_heartbeat_same_site_warn &&
        !linx_heartbeat_same_site_reported) {
        fprintf(stderr,
                "LINX_HEARTBEAT_STALL count=%" PRIu64
                " repeats=%" PRIu64
                " threshold=%" PRIu64
                " delta=%" PRIu64
                " pc=0x%" PRIx64
                " bpc=0x%" PRIx64
                " tpc=0x%" PRIx64
                " envpc=0x%" PRIx64
                " acr=%u cstate=0x%" PRIx64
                " status=same-site-running\n",
                env->insn_count, linx_heartbeat_same_site_repeats,
                linx_heartbeat_same_site_warn, delta, pc, env->bpc,
                env->body_tpc, env->pc, env->acr & 0xFu, env->ssr[0x20]);
        linx_heartbeat_same_site_reported = true;
    }
    fflush(stderr);
}

#define LINX_FCMP_TRACE_OP_FEQ (1u << 0)
#define LINX_FCMP_TRACE_OP_FLT (1u << 1)
#define LINX_FCMP_TRACE_OP_FGE (1u << 2)
#define LINX_FCMP_TRACE_OP_ALL \
    (LINX_FCMP_TRACE_OP_FEQ | \
     LINX_FCMP_TRACE_OP_FLT | \
     LINX_FCMP_TRACE_OP_FGE)

static const char *linx_env_nonzero2(const char *name, const char *alias)
{
    const char *v = getenv(name);

    if (v && v[0] && strcmp(v, "0") != 0) {
        return v;
    }
    v = getenv(alias);
    if (v && v[0] && strcmp(v, "0") != 0) {
        return v;
    }
    return NULL;
}

static const char *linx_env_value2(const char *name, const char *alias)
{
    const char *v = getenv(name);

    if (v && v[0]) {
        return v;
    }
    v = getenv(alias);
    if (v && v[0]) {
        return v;
    }
    return NULL;
}

static void linx_acre_trace_init(void)
{
    uint64_t lo = 0;
    uint64_t hi = UINT64_MAX;
    const char *lo_s;
    const char *hi_s;
    const char *value_s;

    if (linx_acre_trace_inited) {
        return;
    }
    linx_acre_trace_inited = true;

    linx_acre_trace_enabled =
        linx_env_enabled("LINX_ACRE_TRACE") ||
        linx_env_enabled("LINX_QEMU_ACRE_TRACE");

    lo_s = linx_env_value2("LINX_ACRE_TRACE_PC_LO",
                           "LINX_QEMU_ACRE_TRACE_PC_LO");
    hi_s = linx_env_value2("LINX_ACRE_TRACE_PC_HI",
                           "LINX_QEMU_ACRE_TRACE_PC_HI");
    const bool have_pc_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_pc_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_pc_lo || have_pc_hi) {
        linx_acre_trace_pc_lo = MIN(lo, hi);
        linx_acre_trace_pc_hi = MAX(lo, hi);
        linx_acre_trace_pc_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_ACRE_TRACE_PC",
                              "LINX_QEMU_ACRE_TRACE_PC");
    if (value_s && linx_parse_u64(value_s, &lo)) {
        linx_acre_trace_pc_lo = lo;
        linx_acre_trace_pc_hi = lo;
        linx_acre_trace_pc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_env_value2("LINX_ACRE_TRACE_BPC_LO",
                           "LINX_QEMU_ACRE_TRACE_BPC_LO");
    hi_s = linx_env_value2("LINX_ACRE_TRACE_BPC_HI",
                           "LINX_QEMU_ACRE_TRACE_BPC_HI");
    const bool have_bpc_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_bpc_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_bpc_lo || have_bpc_hi) {
        linx_acre_trace_bpc_lo = MIN(lo, hi);
        linx_acre_trace_bpc_hi = MAX(lo, hi);
        linx_acre_trace_bpc_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_ACRE_TRACE_BPC",
                              "LINX_QEMU_ACRE_TRACE_BPC");
    if (value_s && linx_parse_u64(value_s, &lo)) {
        linx_acre_trace_bpc_lo = lo;
        linx_acre_trace_bpc_hi = lo;
        linx_acre_trace_bpc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_env_value2("LINX_ACRE_TRACE_COUNT_LO",
                           "LINX_QEMU_ACRE_TRACE_COUNT_LO");
    hi_s = linx_env_value2("LINX_ACRE_TRACE_COUNT_HI",
                           "LINX_QEMU_ACRE_TRACE_COUNT_HI");
    const bool have_count_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_count_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_count_lo || have_count_hi) {
        linx_acre_trace_count_lo = MIN(lo, hi);
        linx_acre_trace_count_hi = MAX(lo, hi);
        linx_acre_trace_count_filter_enabled = true;
    }

    value_s = linx_env_value2("LINX_ACRE_TRACE_TARGET",
                              "LINX_QEMU_ACRE_TRACE_TARGET");
    if (value_s && linx_parse_u64(value_s, &linx_acre_trace_target)) {
        linx_acre_trace_target_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_ACRE_TRACE_RRA",
                              "LINX_QEMU_ACRE_TRACE_RRA");
    if (value_s && linx_parse_u64(value_s, &linx_acre_trace_rra)) {
        linx_acre_trace_rra_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_ACRE_TRACE_TRAPNUM",
                              "LINX_QEMU_ACRE_TRACE_TRAPNUM");
    if (value_s && linx_parse_u64(value_s, &linx_acre_trace_trap)) {
        linx_acre_trace_trap_filter_enabled = true;
    }

    value_s = linx_env_value2("LINX_ACRE_TRACE_LIMIT",
                              "LINX_QEMU_ACRE_TRACE_LIMIT");
    if (value_s) {
        (void)linx_parse_u64(value_s, &linx_acre_trace_limit);
    } else {
        linx_acre_trace_limit = 64;
    }

    value_s = linx_env_nonzero2("LINX_ACRE_TRACE_CODE_BYTES",
                                "LINX_QEMU_ACRE_TRACE_CODE_BYTES");
    if (value_s) {
        uint64_t bytes = 0;
        if (linx_parse_u64(value_s, &bytes) && bytes != 0) {
            linx_acre_trace_code_bytes = MIN(bytes, (uint64_t)32);
        }
    }

    linx_acre_trace_regs_enabled =
        linx_env_enabled("LINX_ACRE_TRACE_REGS") ||
        linx_env_enabled("LINX_QEMU_ACRE_TRACE_REGS") ||
        linx_env_enabled("LINX_TRACE_REGS");
}

static bool linx_acre_trace_matches(CPULinxState *env, uint32_t target,
                                    uint32_t rra_type, uint64_t trapno,
                                    uint64_t resume_pc, uint64_t resume_bpc)
{
    linx_acre_trace_init();
    if (!linx_acre_trace_enabled) {
        return false;
    }
    if (linx_acre_trace_limit &&
        linx_acre_trace_emitted >= linx_acre_trace_limit) {
        return false;
    }
    if (linx_acre_trace_pc_filter_enabled &&
        (resume_pc < linx_acre_trace_pc_lo ||
         resume_pc > linx_acre_trace_pc_hi)) {
        return false;
    }
    if (linx_acre_trace_bpc_filter_enabled &&
        (resume_bpc < linx_acre_trace_bpc_lo ||
         resume_bpc > linx_acre_trace_bpc_hi)) {
        return false;
    }
    if (linx_acre_trace_count_filter_enabled &&
        (env->insn_count < linx_acre_trace_count_lo ||
         env->insn_count > linx_acre_trace_count_hi)) {
        return false;
    }
    if (linx_acre_trace_target_filter_enabled &&
        target != linx_acre_trace_target) {
        return false;
    }
    if (linx_acre_trace_rra_filter_enabled &&
        rra_type != linx_acre_trace_rra) {
        return false;
    }
    if (linx_acre_trace_trap_filter_enabled &&
        (trapno & 0x3fu) != linx_acre_trace_trap) {
        return false;
    }
    return true;
}

static void linx_acre_trace_maybe_emit(CPULinxState *env, const char *phase,
                                       uint32_t mgr, uint32_t target,
                                       uint32_t rra_type, bool bi,
                                       uint64_t trapno, uint64_t ecstate,
                                       uint64_t resume_pc,
                                       uint64_t resume_bpc,
                                       uint64_t resume_tpc)
{
    if (!linx_acre_trace_matches(env, target, rra_type, trapno,
                                 resume_pc, resume_bpc)) {
        return;
    }

    const LinxAcrBlockState *target_state =
        target < LINX_ACR_COUNT ? &env->acr_block_state[target] : NULL;

    linx_acre_trace_emitted++;
    fprintf(stderr,
            "LINX_ACRE_TRACE phase=%s count=%" PRIu64
            " mgr=%u target=%u rra=%u bi=%u"
            " trapno=0x%" PRIx64 " trapnum=%" PRIu64
            " ecstate=0x%" PRIx64
            " resume=0x%" PRIx64
            " resume_bpc=0x%" PRIx64
            " resume_tpc=0x%" PRIx64
            " pc=0x%" PRIx64
            " bpc=0x%" PRIx64
            " cstate=0x%" PRIx64
            " acr=%u in_body=%u blocktype=%u brtype=%u"
            " tgt=0x%" PRIx64
            " body_tpc=0x%" PRIx64
            " return_pc=0x%" PRIx64
            " call_ra_set=%u call_setret_pending=%u"
            " sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " tp=0x%" PRIx64
            " etemp1=0x%" PRIx64
            " ipending1=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64
            " a2=0x%" PRIx64
            " a3=0x%" PRIx64
            " a4=0x%" PRIx64
            " a5=0x%" PRIx64
            " a6=0x%" PRIx64
            " a7=0x%" PRIx64
            " ebarg_tq0=0x%" PRIx64 " ebarg_tq1=0x%" PRIx64
            " ebarg_tq2=0x%" PRIx64 " ebarg_tq3=0x%" PRIx64
            " ebarg_uq0=0x%" PRIx64 " ebarg_uq1=0x%" PRIx64
            " ebarg_uq2=0x%" PRIx64 " ebarg_uq3=0x%" PRIx64
            " saved_tq0=0x%" PRIx64 " saved_tq1=0x%" PRIx64
            " saved_tq2=0x%" PRIx64 " saved_tq3=0x%" PRIx64
            " saved_uq0=0x%" PRIx64 " saved_uq1=0x%" PRIx64
            " saved_uq2=0x%" PRIx64 " saved_uq3=0x%" PRIx64
            " tq0=0x%" PRIx64 " tq1=0x%" PRIx64
            " tq2=0x%" PRIx64 " tq3=0x%" PRIx64
            " uq0=0x%" PRIx64 " uq1=0x%" PRIx64
            " uq2=0x%" PRIx64 " uq3=0x%" PRIx64
            "\n",
            phase, env->insn_count, mgr, target, rra_type, bi ? 1u : 0u,
            trapno, trapno & 0x3fu, ecstate, resume_pc, resume_bpc,
            resume_tpc, env->pc, env->bpc, env->ssr[0x20],
            env->acr & 0xFu, env->in_body, env->blocktype, env->brtype,
            env->tgt, env->body_tpc, env->return_pc, env->call_ra_set,
            env->call_setret_pending, env->gpr[LINX_REG_SP],
            env->gpr[LINX_REG_RA], env->ssr[0],
            env->ssr_acr[1][0xF05], env->ssr_acr[1][0xF08],
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1],
            env->gpr[LINX_REG_A2], env->gpr[LINX_REG_A3],
            env->gpr[LINX_REG_A4], env->gpr[LINX_REG_A5],
            env->gpr[LINX_REG_A6], env->gpr[LINX_REG_A7],
            env->ssr_acr[mgr][LINX_EBARG_IDX_TQ0],
            env->ssr_acr[mgr][LINX_EBARG_IDX_TQ1],
            env->ssr_acr[mgr][LINX_EBARG_IDX_TQ2],
            env->ssr_acr[mgr][LINX_EBARG_IDX_TQ3],
            env->ssr_acr[mgr][LINX_EBARG_IDX_UQ0],
            env->ssr_acr[mgr][LINX_EBARG_IDX_UQ1],
            env->ssr_acr[mgr][LINX_EBARG_IDX_UQ2],
            env->ssr_acr[mgr][LINX_EBARG_IDX_UQ3],
            target_state ? target_state->tq[0] : 0,
            target_state ? target_state->tq[1] : 0,
            target_state ? target_state->tq[2] : 0,
            target_state ? target_state->tq[3] : 0,
            target_state ? target_state->uq[0] : 0,
            target_state ? target_state->uq[1] : 0,
            target_state ? target_state->uq[2] : 0,
            target_state ? target_state->uq[3] : 0,
            env->tq[0], env->tq[1], env->tq[2], env->tq[3],
            env->uq[0], env->uq[1], env->uq[2], env->uq[3]);

    if (linx_acre_trace_regs_enabled) {
        fprintf(stderr, "LINX_ACRE_REGS phase=%s count=%" PRIu64,
                phase, env->insn_count);
        linx_fprint_gprs(stderr, env);
        fputc('\n', stderr);
    }
    if (linx_acre_trace_code_bytes) {
        fprintf(stderr, "LINX_ACRE_CODE phase=%s count=%" PRIu64,
                phase, env->insn_count);
        linx_fprint_guest_code_bytes(stderr, env, "resume", resume_pc,
                                     linx_acre_trace_code_bytes);
        linx_fprint_guest_code_bytes(stderr, env, "bpc", resume_bpc,
                                     linx_acre_trace_code_bytes);
        if (resume_tpc) {
            linx_fprint_guest_code_bytes(stderr, env, "tpc", resume_tpc,
                                         linx_acre_trace_code_bytes);
        }
        fputc('\n', stderr);
    }
    fflush(stderr);
}

static void linx_queue_trace_init(void)
{
    uint64_t lo = 0;
    uint64_t hi = UINT64_MAX;
    const char *lo_s;
    const char *hi_s;
    const char *value_s;

    if (linx_queue_trace_inited) {
        return;
    }
    linx_queue_trace_inited = true;

    linx_queue_trace_enabled =
        linx_env_enabled("LINX_QUEUE_TRACE") ||
        linx_env_enabled("LINX_QEMU_QUEUE_TRACE");

    lo_s = linx_env_value2("LINX_QUEUE_TRACE_PC_LO",
                           "LINX_QEMU_QUEUE_TRACE_PC_LO");
    hi_s = linx_env_value2("LINX_QUEUE_TRACE_PC_HI",
                           "LINX_QEMU_QUEUE_TRACE_PC_HI");
    const bool have_pc_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_pc_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_pc_lo || have_pc_hi) {
        linx_queue_trace_pc_lo = MIN(lo, hi);
        linx_queue_trace_pc_hi = MAX(lo, hi);
        linx_queue_trace_pc_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_QUEUE_TRACE_PC",
                              "LINX_QEMU_QUEUE_TRACE_PC");
    if (value_s && linx_parse_u64(value_s, &lo)) {
        linx_queue_trace_pc_lo = lo;
        linx_queue_trace_pc_hi = lo;
        linx_queue_trace_pc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_env_value2("LINX_QUEUE_TRACE_BPC_LO",
                           "LINX_QEMU_QUEUE_TRACE_BPC_LO");
    hi_s = linx_env_value2("LINX_QUEUE_TRACE_BPC_HI",
                           "LINX_QEMU_QUEUE_TRACE_BPC_HI");
    const bool have_bpc_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_bpc_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_bpc_lo || have_bpc_hi) {
        linx_queue_trace_bpc_lo = MIN(lo, hi);
        linx_queue_trace_bpc_hi = MAX(lo, hi);
        linx_queue_trace_bpc_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_QUEUE_TRACE_BPC",
                              "LINX_QEMU_QUEUE_TRACE_BPC");
    if (value_s && linx_parse_u64(value_s, &lo)) {
        linx_queue_trace_bpc_lo = lo;
        linx_queue_trace_bpc_hi = lo;
        linx_queue_trace_bpc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_env_value2("LINX_QUEUE_TRACE_COUNT_LO",
                           "LINX_QEMU_QUEUE_TRACE_COUNT_LO");
    hi_s = linx_env_value2("LINX_QUEUE_TRACE_COUNT_HI",
                           "LINX_QEMU_QUEUE_TRACE_COUNT_HI");
    const bool have_count_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_count_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_count_lo || have_count_hi) {
        linx_queue_trace_count_lo = MIN(lo, hi);
        linx_queue_trace_count_hi = MAX(lo, hi);
        linx_queue_trace_count_filter_enabled = true;
    }

    value_s = linx_env_value2("LINX_QUEUE_TRACE_LIMIT",
                              "LINX_QEMU_QUEUE_TRACE_LIMIT");
    if (value_s) {
        (void)linx_parse_u64(value_s, &linx_queue_trace_limit);
    } else {
        linx_queue_trace_limit = 128;
    }

    linx_queue_trace_all =
        linx_env_enabled("LINX_QUEUE_TRACE_ALL") ||
        linx_env_enabled("LINX_QEMU_QUEUE_TRACE_ALL");
}

static bool linx_queue_trace_state_changed(CPULinxState *env)
{
    if (!linx_queue_trace_last_valid) {
        return true;
    }
    if ((env->acr & 0xFu) != linx_queue_trace_last_acr ||
        env->in_body != linx_queue_trace_last_in_body ||
        env->blocktype != linx_queue_trace_last_blocktype ||
        env->brtype != linx_queue_trace_last_brtype ||
        env->tgt != linx_queue_trace_last_tgt ||
        env->body_tpc != linx_queue_trace_last_body_tpc ||
        env->return_pc != linx_queue_trace_last_return_pc ||
        env->call_ra_set != linx_queue_trace_last_call_ra_set ||
        env->call_setret_pending != linx_queue_trace_last_call_setret_pending) {
        return true;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (env->tq[i] != linx_queue_trace_last_tq[i] ||
            env->uq[i] != linx_queue_trace_last_uq[i]) {
            return true;
        }
    }
    return false;
}

static void linx_queue_trace_remember(CPULinxState *env)
{
    for (unsigned i = 0; i < 4; i++) {
        linx_queue_trace_last_tq[i] = env->tq[i];
        linx_queue_trace_last_uq[i] = env->uq[i];
    }
    linx_queue_trace_last_tgt = env->tgt;
    linx_queue_trace_last_body_tpc = env->body_tpc;
    linx_queue_trace_last_return_pc = env->return_pc;
    linx_queue_trace_last_acr = env->acr & 0xFu;
    linx_queue_trace_last_in_body = env->in_body;
    linx_queue_trace_last_blocktype = env->blocktype;
    linx_queue_trace_last_brtype = env->brtype;
    linx_queue_trace_last_call_ra_set = env->call_ra_set;
    linx_queue_trace_last_call_setret_pending = env->call_setret_pending;
    linx_queue_trace_last_valid = true;
}

static void linx_queue_trace_probe(CPULinxState *env, uint64_t pc)
{
    linx_queue_trace_init();
    if (!linx_queue_trace_enabled) {
        return;
    }
    if (linx_queue_trace_limit &&
        linx_queue_trace_emitted >= linx_queue_trace_limit) {
        return;
    }
    if (linx_queue_trace_pc_filter_enabled &&
        (pc < linx_queue_trace_pc_lo || pc > linx_queue_trace_pc_hi)) {
        return;
    }
    if (linx_queue_trace_bpc_filter_enabled &&
        (env->bpc < linx_queue_trace_bpc_lo ||
         env->bpc > linx_queue_trace_bpc_hi)) {
        return;
    }
    if (linx_queue_trace_count_filter_enabled &&
        (env->insn_count < linx_queue_trace_count_lo ||
         env->insn_count > linx_queue_trace_count_hi)) {
        return;
    }
    if (!linx_queue_trace_all && !linx_queue_trace_state_changed(env)) {
        return;
    }

    linx_queue_trace_emitted++;
    fprintf(stderr,
            "LINX_QUEUE_TRACE seq=%" PRIu64
            " count=%" PRIu64
            " pc=0x%" PRIx64
            " envpc=0x%" PRIx64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " acr=%u cstate=0x%" PRIx64
            " in_body=%u blocktype=%u brtype=%u"
            " tgt=0x%" PRIx64
            " body_tpc=0x%" PRIx64
            " return_pc=0x%" PRIx64
            " call_ra_set=%u call_setret_pending=%u"
            " sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64
            " a2=0x%" PRIx64
            " a3=0x%" PRIx64
            " a4=0x%" PRIx64
            " a5=0x%" PRIx64
            " a6=0x%" PRIx64
            " a7=0x%" PRIx64
            " tq0=0x%" PRIx64 " tq1=0x%" PRIx64
            " tq2=0x%" PRIx64 " tq3=0x%" PRIx64
            " uq0=0x%" PRIx64 " uq1=0x%" PRIx64
            " uq2=0x%" PRIx64 " uq3=0x%" PRIx64
            "\n",
            linx_queue_trace_emitted, env->insn_count, pc, env->pc,
            env->bpc, env->body_tpc, env->acr & 0xFu, env->ssr[0x20],
            env->in_body, env->blocktype, env->brtype, env->tgt,
            env->body_tpc, env->return_pc, env->call_ra_set,
            env->call_setret_pending, env->gpr[LINX_REG_SP],
            env->gpr[LINX_REG_RA], env->gpr[LINX_REG_A0],
            env->gpr[LINX_REG_A1], env->gpr[LINX_REG_A2],
            env->gpr[LINX_REG_A3], env->gpr[LINX_REG_A4],
            env->gpr[LINX_REG_A5], env->gpr[LINX_REG_A6],
            env->gpr[LINX_REG_A7], env->tq[0], env->tq[1],
            env->tq[2], env->tq[3], env->uq[0], env->uq[1],
            env->uq[2], env->uq[3]);
    fflush(stderr);
    linx_queue_trace_remember(env);
}

static void linx_restore_bstate_from_ebarg(CPULinxState *env, uint32_t mgr)
{
    const uint64_t lb = env->ssr_acr[mgr][LINX_EBARG_IDX_LB];
    const uint64_t lc = env->ssr_acr[mgr][LINX_EBARG_IDX_LC];

    env->blocktype = env->ssr_acr[mgr][LINX_EBARG_IDX_0] & 0x1fu;
    env->tq[0] = env->ssr_acr[mgr][LINX_EBARG_IDX_TQ0];
    env->tq[1] = env->ssr_acr[mgr][LINX_EBARG_IDX_TQ1];
    env->tq[2] = env->ssr_acr[mgr][LINX_EBARG_IDX_TQ2];
    env->tq[3] = env->ssr_acr[mgr][LINX_EBARG_IDX_TQ3];
    env->uq[0] = env->ssr_acr[mgr][LINX_EBARG_IDX_UQ0];
    env->uq[1] = env->ssr_acr[mgr][LINX_EBARG_IDX_UQ1];
    env->uq[2] = env->ssr_acr[mgr][LINX_EBARG_IDX_UQ2];
    env->uq[3] = env->ssr_acr[mgr][LINX_EBARG_IDX_UQ3];
    env->lb[0] = lb & 0xffffu;
    env->lb[1] = (lb >> 16) & 0xffffu;
    env->lb[2] = (lb >> 32) & 0xffffu;
    env->lc[0] = lc & 0xffffu;
    env->lc[1] = (lc >> 16) & 0xffffu;
    env->lc[2] = (lc >> 32) & 0xffffu;
}

static void linx_tlb_trace_init(void)
{
    if (linx_tlb_trace_inited) {
        return;
    }

    linx_tlb_trace_enabled =
        linx_env_enabled("LINX_TLB_TRACE") ||
        linx_env_enabled("LINX_QEMU_TLB_TRACE");

    uint64_t lo = 0;
    uint64_t hi = UINT64_MAX;
    const char *lo_s = linx_env_nonzero2("LINX_TLB_TRACE_PC_LO",
                                         "LINX_QEMU_TLB_TRACE_PC_LO");
    const char *hi_s = linx_env_nonzero2("LINX_TLB_TRACE_PC_HI",
                                         "LINX_QEMU_TLB_TRACE_PC_HI");
    const bool have_pc_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_pc_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_pc_lo || have_pc_hi) {
        linx_tlb_trace_pc_lo = MIN(lo, hi);
        linx_tlb_trace_pc_hi = MAX(lo, hi);
        linx_tlb_trace_pc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_env_nonzero2("LINX_TLB_TRACE_COUNT_LO",
                             "LINX_QEMU_TLB_TRACE_COUNT_LO");
    hi_s = linx_env_nonzero2("LINX_TLB_TRACE_COUNT_HI",
                             "LINX_QEMU_TLB_TRACE_COUNT_HI");
    const bool have_count_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_count_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_count_lo || have_count_hi) {
        linx_tlb_trace_count_lo = MIN(lo, hi);
        linx_tlb_trace_count_hi = MAX(lo, hi);
        linx_tlb_trace_count_filter_enabled = true;
    }

    const char *limit_s = linx_env_nonzero2("LINX_TLB_TRACE_LIMIT",
                                            "LINX_QEMU_TLB_TRACE_LIMIT");
    if (limit_s) {
        (void)linx_parse_u64(limit_s, &linx_tlb_trace_limit);
    }

    const char *code_s = linx_env_nonzero2("LINX_TLB_TRACE_CODE_BYTES",
                                           "LINX_QEMU_TLB_TRACE_CODE_BYTES");
    if (code_s) {
        uint64_t bytes = 0;
        if (linx_parse_u64(code_s, &bytes) && bytes != 0) {
            linx_tlb_trace_code_bytes = MIN((uint64_t)32, bytes);
        }
    }

    linx_tlb_trace_inited = true;
}

static bool linx_tlb_trace_addr_matches(uint64_t addr)
{
    return addr >= linx_tlb_trace_pc_lo &&
           addr <= linx_tlb_trace_pc_hi;
}

static bool linx_tlb_trace_matches(CPULinxState *env, uint64_t pc)
{
    if (!linx_tlb_trace_enabled) {
        return false;
    }
    if (linx_tlb_trace_limit != 0 &&
        linx_tlb_trace_emitted >= linx_tlb_trace_limit) {
        return false;
    }
    if (linx_tlb_trace_count_filter_enabled &&
        (env->insn_count < linx_tlb_trace_count_lo ||
         env->insn_count > linx_tlb_trace_count_hi)) {
        return false;
    }
    if (linx_tlb_trace_pc_filter_enabled &&
        !linx_tlb_trace_addr_matches(pc) &&
        !linx_tlb_trace_addr_matches(env->bpc) &&
        !linx_tlb_trace_addr_matches(env->body_tpc)) {
        return false;
    }
    return true;
}

static void linx_tlb_trace_emit(CPULinxState *env, const char *op,
                                uint64_t pc, uint64_t operand,
                                bool have_operand)
{
    linx_tlb_trace_init();
    if (!linx_tlb_trace_matches(env, pc)) {
        return;
    }

    linx_tlb_trace_emitted++;
    fprintf(stderr,
            "LINX_TLB_TRACE op=%s count=%" PRIu64
            " emitted=%" PRIu64
            " pc=0x%" PRIx64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " envpc=0x%" PRIx64
            " acr=%u cstate=0x%" PRIx64
            " brtype=%u tgt=0x%" PRIx64
            " in_body=%u sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " tp=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64,
            op, env->insn_count, linx_tlb_trace_emitted,
            pc, env->bpc, env->body_tpc, env->pc,
            env->acr & 0xFu, env->ssr[0x20],
            env->brtype, env->tgt, env->in_body,
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->ssr[0x0000], env->gpr[LINX_REG_A0],
            env->gpr[LINX_REG_A1]);
    if (have_operand) {
        fprintf(stderr, " operand=0x%" PRIx64, operand);
    }
    if (linx_tlb_trace_code_bytes) {
        linx_fprint_guest_code_bytes(stderr, env, "pc", pc,
                                     linx_tlb_trace_code_bytes);
        linx_fprint_guest_code_bytes(stderr, env, "bpc", env->bpc,
                                     linx_tlb_trace_code_bytes);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static bool linx_tlb_stats_enabled_p(void)
{
    if (!linx_tlb_stats_inited) {
        linx_tlb_stats_enabled =
            linx_env_nonzero2("LINX_TLB_STATS",
                              "LINX_QEMU_TLB_STATS") != NULL;
        linx_tlb_stats_inited = true;
    }
    return linx_tlb_stats_enabled;
}

static bool linx_tlb_inv_hot_enabled_p(void)
{
    if (!linx_tlb_inv_hot_inited) {
        linx_tlb_inv_hot_enabled =
            linx_env_nonzero2("LINX_TLB_INV_HOT",
                              "LINX_QEMU_TLB_INV_HOT") != NULL;
        linx_tlb_inv_hot_inited = true;
    }
    return linx_tlb_inv_hot_enabled;
}

static uint64_t linx_tlb_inv_operand_page(LinxTlbInvOp op, uint64_t operand)
{
    uint64_t addr = operand;

    if (op == LINX_TLB_INV_IAV) {
        addr = operand & ((UINT64_C(1) << 44) - 1);
    } else if (op != LINX_TLB_INV_IV) {
        return operand;
    }
    return addr & TARGET_PAGE_MASK;
}

static void linx_tlb_inv_hot_record(CPULinxState *env, LinxTlbInvOp op,
                                    uint64_t pc, uint64_t operand)
{
    if (!linx_tlb_inv_hot_enabled_p()) {
        return;
    }

    int found = -1;
    int empty = -1;
    int min_slot = 0;
    uint64_t min_count = UINT64_MAX;

    env->tlb_inv_hot_active = 1;
    for (unsigned i = 0; i < LINX_TLB_INV_HOT_SLOTS; i++) {
        if (!env->tlb_inv_hot_valid[i]) {
            if (empty < 0) {
                empty = (int)i;
            }
            continue;
        }
        if (env->tlb_inv_hot_op[i] == (uint8_t)op &&
            env->tlb_inv_hot_pc[i] == pc) {
            found = (int)i;
            break;
        }
        if (env->tlb_inv_hot_count[i] < min_count) {
            min_count = env->tlb_inv_hot_count[i];
            min_slot = (int)i;
        }
    }

    int slot = found;
    if (slot < 0) {
        slot = empty >= 0 ? empty : min_slot;
        if (empty < 0) {
            env->tlb_inv_hot_evictions++;
        }
        env->tlb_inv_hot_valid[slot] = 1;
        env->tlb_inv_hot_count[slot] = 0;
        env->tlb_inv_hot_emit_count[slot] = 0;
        env->tlb_inv_hot_op[slot] = (uint8_t)op;
        env->tlb_inv_hot_pc[slot] = pc;
    }

    env->tlb_inv_hot_count[slot]++;
    env->tlb_inv_hot_last_bpc[slot] = env->bpc;
    env->tlb_inv_hot_last_operand[slot] = operand;
    env->tlb_inv_hot_last_page[slot] = linx_tlb_inv_operand_page(op, operand);
    env->tlb_inv_hot_acr[slot] = env->acr & 0xFu;
}

static void linx_tlb_stats_record(CPULinxState *env, LinxTlbInvOp op,
                                  uint64_t pc, uint64_t operand)
{
    linx_tlb_inv_hot_record(env, op, pc, operand);

    if (!linx_tlb_stats_enabled_p()) {
        return;
    }

    switch (op) {
    case LINX_TLB_INV_IALL:
        env->tlb_inv_iall++;
        break;
    case LINX_TLB_INV_IA:
        env->tlb_inv_ia++;
        break;
    case LINX_TLB_INV_IV:
        env->tlb_inv_iv++;
        break;
    case LINX_TLB_INV_IAV:
        env->tlb_inv_iav++;
        break;
    }

    env->tlb_inv_last_count = env->insn_count;
    env->tlb_inv_last_pc = pc;
    env->tlb_inv_last_bpc = env->bpc;
    env->tlb_inv_last_operand = operand;
    env->tlb_inv_last_acr = env->acr & 0xFu;
}

static void linx_fcmp_trace_init(void)
{
    if (linx_fcmp_trace_inited) {
        return;
    }

    linx_fcmp_trace_enabled =
        linx_env_enabled("LINX_FCMP_TRACE") ||
        linx_env_enabled("LINX_FP_TRACE");

    uint64_t lo = 0;
    uint64_t hi = 0;
    const char *lo_s = linx_env_nonzero2("LINX_FCMP_TRACE_PC_LO",
                                         "LINX_FP_TRACE_PC_LO");
    const char *hi_s = linx_env_nonzero2("LINX_FCMP_TRACE_PC_HI",
                                         "LINX_FP_TRACE_PC_HI");
    if (lo_s && hi_s && linx_parse_u64(lo_s, &lo) &&
        linx_parse_u64(hi_s, &hi)) {
        linx_fcmp_trace_pc_lo = MIN(lo, hi);
        linx_fcmp_trace_pc_hi = MAX(lo, hi);
        linx_fcmp_trace_pc_filter_enabled = true;
    }

    lo = 0;
    hi = 0;
    lo_s = getenv("LINX_FCMP_TRACE_COUNT_LO");
    hi_s = getenv("LINX_FCMP_TRACE_COUNT_HI");
    if ((!lo_s || !lo_s[0]) && (!hi_s || !hi_s[0])) {
        lo_s = getenv("LINX_FP_TRACE_COUNT_LO");
        hi_s = getenv("LINX_FP_TRACE_COUNT_HI");
    }
    if (lo_s && lo_s[0] && hi_s && hi_s[0] &&
        linx_parse_u64(lo_s, &lo) && linx_parse_u64(hi_s, &hi)) {
        linx_fcmp_trace_count_lo = MIN(lo, hi);
        linx_fcmp_trace_count_hi = MAX(lo, hi);
        linx_fcmp_trace_count_filter_enabled = true;
    }

    const char *limit_s = linx_env_nonzero2("LINX_FCMP_TRACE_LIMIT",
                                            "LINX_FP_TRACE_LIMIT");
    if (limit_s) {
        (void)linx_parse_u64(limit_s, &linx_fcmp_trace_limit);
    }

    linx_fcmp_trace_op_mask = LINX_FCMP_TRACE_OP_ALL;
    const char *op_s = linx_env_nonzero2("LINX_FCMP_TRACE_OP",
                                         "LINX_FP_TRACE_OP");
    if (op_s) {
        uint32_t mask = 0;

        if (strstr(op_s, "feq")) {
            mask |= LINX_FCMP_TRACE_OP_FEQ;
        }
        if (strstr(op_s, "flt")) {
            mask |= LINX_FCMP_TRACE_OP_FLT;
        }
        if (strstr(op_s, "fge")) {
            mask |= LINX_FCMP_TRACE_OP_FGE;
        }
        if (mask != 0) {
            linx_fcmp_trace_op_mask = mask;
        }
    }

    linx_fcmp_trace_inited = true;
}

static bool linx_fcmp_trace_addr_matches(uint64_t addr)
{
    return addr >= linx_fcmp_trace_pc_lo &&
           addr <= linx_fcmp_trace_pc_hi;
}

static bool linx_fcmp_trace_matches(CPULinxState *env, uint32_t op_mask)
{
    if (!linx_fcmp_trace_enabled) {
        return false;
    }
    if ((linx_fcmp_trace_op_mask & op_mask) == 0) {
        return false;
    }
    if (linx_fcmp_trace_limit != 0 &&
        linx_fcmp_trace_emitted >= linx_fcmp_trace_limit) {
        return false;
    }
    if (linx_fcmp_trace_count_filter_enabled &&
        (env->insn_count < linx_fcmp_trace_count_lo ||
         env->insn_count > linx_fcmp_trace_count_hi)) {
        return false;
    }
    if (linx_fcmp_trace_pc_filter_enabled &&
        !linx_fcmp_trace_addr_matches(env->pc) &&
        !linx_fcmp_trace_addr_matches(env->bpc) &&
        !linx_fcmp_trace_addr_matches(env->body_tpc)) {
        return false;
    }
    return true;
}

static const char *linx_fcmp_trace_type_name(uint32_t srctype)
{
    switch (srctype & 0x3u) {
    case 0:
        return "fd";
    case 1:
        return "fs";
    default:
        return "illegal";
    }
}

static void linx_fcmp_trace_emit(CPULinxState *env, const char *op,
                                 uint32_t op_mask, uint64_t lhs,
                                 uint64_t rhs, uint32_t srctype,
                                 bool result)
{
    linx_fcmp_trace_init();
    if (!linx_fcmp_trace_matches(env, op_mask)) {
        return;
    }

    linx_fcmp_trace_emitted++;
    fprintf(stderr,
            "LINX_FCMP_TRACE op=%s count=%" PRIu64
            " emitted=%" PRIu64
            " pc=0x%" PRIx64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " srctype=%u type=%s"
            " lhs=0x%" PRIx64
            " rhs=0x%" PRIx64
            " result=%u fcsr=0x%08x",
            op, env->insn_count, linx_fcmp_trace_emitted,
            env->pc, env->bpc, env->body_tpc,
            srctype, linx_fcmp_trace_type_name(srctype),
            lhs, rhs, result ? 1u : 0u, env->fcsr);

    switch (srctype & 0x3u) {
    case 0: {
        union {
            uint64_t u;
            double d;
        } lhs64 = { .u = float64_val((float64)lhs) },
          rhs64 = { .u = float64_val((float64)rhs) };

        fprintf(stderr, " lhs_f64=%.17g rhs_f64=%.17g",
                lhs64.d, rhs64.d);
        break;
    }
    case 1: {
        union {
            uint32_t u;
            float f;
        } lhs32 = { .u = float32_val((float32)(uint32_t)lhs) },
          rhs32 = { .u = float32_val((float32)(uint32_t)rhs) };

        fprintf(stderr, " lhs_f32=%.9g rhs_f32=%.9g",
                (double)lhs32.f, (double)rhs32.f);
        break;
    }
    default:
        break;
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static const char *linx_call_trace_event_name(uint32_t event)
{
    switch (event) {
    case LINX_CALL_TRACE_SETRET:
        return "setret";
    case LINX_CALL_TRACE_CALL_COMMIT:
        return "call_commit";
    case LINX_CALL_TRACE_FENTRY:
        return "fentry";
    case LINX_CALL_TRACE_FRET_STK:
        return "fret_stk";
    case LINX_CALL_TRACE_ACRE_ENTER:
        return "acre_enter";
    case LINX_CALL_TRACE_ACRE_STAGED:
        return "acre_staged";
    default:
        return "unknown";
    }
}

static void linx_call_trace_init(void)
{
    if (linx_call_trace_inited) {
        return;
    }

    const char *enabled_s = getenv("LINX_CALL_TRACE");
    linx_call_trace_enabled =
        enabled_s && enabled_s[0] && strcmp(enabled_s, "0") != 0;

    uint64_t lo = 0;
    uint64_t hi = 0;
    const char *lo_s = getenv("LINX_CALL_TRACE_PC_LO");
    const char *hi_s = getenv("LINX_CALL_TRACE_PC_HI");
    if (lo_s && lo_s[0] && strcmp(lo_s, "0") != 0 &&
        hi_s && hi_s[0] && strcmp(hi_s, "0") != 0 &&
        linx_parse_u64(lo_s, &lo) && linx_parse_u64(hi_s, &hi)) {
        linx_call_trace_filter_lo = MIN(lo, hi);
        linx_call_trace_filter_hi = MAX(lo, hi);
        linx_call_trace_filter_enabled = true;
    }

    lo = 0;
    hi = 0;
    lo_s = getenv("LINX_CALL_TRACE_COUNT_LO");
    hi_s = getenv("LINX_CALL_TRACE_COUNT_HI");
    if (lo_s && lo_s[0] && strcmp(lo_s, "0") != 0 &&
        hi_s && hi_s[0] && strcmp(hi_s, "0") != 0 &&
        linx_parse_u64(lo_s, &lo) && linx_parse_u64(hi_s, &hi)) {
        linx_call_trace_count_lo = MIN(lo, hi);
        linx_call_trace_count_hi = MAX(lo, hi);
        linx_call_trace_count_filter_enabled = true;
    }

    const char *limit_s = getenv("LINX_CALL_TRACE_LIMIT");
    if (limit_s && limit_s[0] && strcmp(limit_s, "0") != 0) {
        (void)linx_parse_u64(limit_s, &linx_call_trace_limit);
    }

    const char *ring_s = getenv("LINX_CALL_TRACE_RING");
    linx_call_trace_ring_enabled =
        ring_s && ring_s[0] && strcmp(ring_s, "0") != 0;
    if (linx_call_trace_ring_enabled) {
        uint64_t size = 0;
        const char *size_s = getenv("LINX_CALL_TRACE_RING_SIZE");

        linx_call_trace_ring_size = 64;
        if (size_s && size_s[0] && strcmp(size_s, "0") != 0 &&
            linx_parse_u64(size_s, &size)) {
            linx_call_trace_ring_size = MIN(size, (uint64_t)LINX_CALL_TRACE_RING_MAX);
            linx_call_trace_ring_size = MAX(linx_call_trace_ring_size, 1);
        }
    }

    linx_call_trace_inited = true;
}

static bool linx_call_trace_addr_matches(uint64_t addr)
{
    return !linx_call_trace_filter_enabled ||
           (addr >= linx_call_trace_filter_lo &&
            addr <= linx_call_trace_filter_hi);
}

static bool linx_call_trace_matches(CPULinxState *env, uint64_t pc,
                                    uint64_t extra0, uint64_t extra1)
{
    if (!linx_call_trace_enabled) {
        return false;
    }
    if (linx_call_trace_count_filter_enabled &&
        (env->insn_count < linx_call_trace_count_lo ||
         env->insn_count > linx_call_trace_count_hi)) {
        return false;
    }
    if (linx_call_trace_limit &&
        linx_call_trace_emitted >= linx_call_trace_limit) {
        return false;
    }
    if (!linx_call_trace_addr_matches(pc) &&
        !linx_call_trace_addr_matches(extra0) &&
        !linx_call_trace_addr_matches(extra1) &&
        !linx_call_trace_addr_matches(env->gpr[LINX_REG_RA])) {
        return false;
    }
    linx_call_trace_emitted++;
    return true;
}

static void linx_call_trace_ring_record(CPULinxState *env, uint32_t event,
                                        uint64_t pc, uint64_t extra0,
                                        uint64_t extra1)
{
    LinxCallTraceRingEntry *entry;

    if (!linx_call_trace_ring_enabled) {
        return;
    }

    entry = &linx_call_trace_ring[linx_call_trace_ring_next];
    *entry = (LinxCallTraceRingEntry) {
        .event = event,
        .acr = env->acr & 0xFu,
        .brtype = env->brtype,
        .call_ra_set = env->call_ra_set,
        .call_setret_pending = env->call_setret_pending,
        .in_body = env->in_body,
        .tmpl_kind = env->tmpl_kind,
        .tmpl_step = env->tmpl_step,
        .pc = pc,
        .extra0 = extra0,
        .extra1 = extra1,
        .count = env->insn_count,
        .envpc = env->pc,
        .bpc = env->bpc,
        .tpc = env->body_tpc,
        .cstate = env->ssr[0x20],
        .tgt = env->tgt,
        .ra = env->gpr[LINX_REG_RA],
        .sp = env->gpr[LINX_REG_SP],
        .a0 = env->gpr[LINX_REG_A0],
        .a1 = env->gpr[LINX_REG_A1],
        .a2 = env->gpr[LINX_REG_A2],
        .body_tpc = env->body_tpc,
        .return_pc = env->return_pc,
        .tmpl_pc = env->tmpl_pc,
    };

    linx_call_trace_ring_next =
        (linx_call_trace_ring_next + 1) % linx_call_trace_ring_size;
    if (linx_call_trace_ring_count < linx_call_trace_ring_size) {
        linx_call_trace_ring_count++;
    }
}

void linx_call_trace_dump_recent(CPULinxState *env, const char *reason,
                                 uint64_t fault_pc)
{
    uint64_t entries;
    uint64_t start;

    linx_call_trace_init();
    if (!linx_call_trace_ring_enabled || linx_call_trace_ring_count == 0) {
        return;
    }

    entries = linx_call_trace_ring_count;
    start = (linx_call_trace_ring_next + linx_call_trace_ring_size - entries) %
            linx_call_trace_ring_size;
    fprintf(stderr,
            "LINX_CALL_TRACE_RING reason=%s fault_pc=0x%" PRIx64
            " fault_count=%" PRIu64 " entries=%" PRIu64 "\n",
            reason ? reason : "unknown", fault_pc, env->insn_count, entries);

    for (uint64_t i = 0; i < entries; i++) {
        const LinxCallTraceRingEntry *entry =
            &linx_call_trace_ring[(start + i) % linx_call_trace_ring_size];

        fprintf(stderr,
                "LINX_CALL_TRACE_RING_ENTRY idx=%" PRIu64
                " age=%" PRIu64
                " event=%s pc=0x%" PRIx64
                " extra0=0x%" PRIx64 " extra1=0x%" PRIx64
                " count=%" PRIu64
                " envpc=0x%" PRIx64 " bpc=0x%" PRIx64
                " tpc=0x%" PRIx64 " acr=%u cstate=0x%" PRIx64
                " brtype=%u tgt=0x%" PRIx64
                " ra=0x%" PRIx64 " sp=0x%" PRIx64
                " a0=0x%" PRIx64 " a1=0x%" PRIx64
                " a2=0x%" PRIx64
                " call_ra_set=%u call_setret_pending=%u"
                " in_body=%u body_tpc=0x%" PRIx64
                " return_pc=0x%" PRIx64
                " tmpl_kind=%u tmpl_pc=0x%" PRIx64
                " tmpl_step=%u\n",
                i, entries - i - 1, linx_call_trace_event_name(entry->event),
                entry->pc, entry->extra0, entry->extra1, entry->count,
                entry->envpc, entry->bpc, entry->tpc, entry->acr,
                entry->cstate, entry->brtype, entry->tgt, entry->ra,
                entry->sp, entry->a0, entry->a1, entry->a2,
                entry->call_ra_set, entry->call_setret_pending,
                entry->in_body, entry->body_tpc, entry->return_pc,
                entry->tmpl_kind, entry->tmpl_pc, entry->tmpl_step);
    }
    fflush(stderr);
}

static void linx_mem_trace_init(void)
{
    uint64_t value = 0;
    const char *addr_s;
    const char *size_s;
    const char *limit_s;
    const char *access_s;
    const char *acr_s;
    const char *lo_s;
    const char *hi_s;
    const char *count_lo_s;
    const char *count_hi_s;
    const char *enabled_s;
    uint64_t lo = 0;
    uint64_t hi = 0;

    if (linx_mem_trace_inited) {
        return;
    }

    enabled_s = getenv("LINX_MEM_TRACE");
    if (enabled_s && enabled_s[0] && strcmp(enabled_s, "0") != 0) {
        linx_mem_trace_limit = 128;
        linx_mem_trace_enabled = true;
    }

    addr_s = getenv("LINX_MEM_TRACE_ADDR");
    if (addr_s && addr_s[0] && linx_parse_u64(addr_s, &value)) {
        linx_mem_trace_addr = value;
        linx_mem_trace_size = 8;
        linx_mem_trace_limit = 128;
        linx_mem_trace_enabled = true;
        linx_mem_trace_addr_filter_enabled = true;
    }

    size_s = getenv("LINX_MEM_TRACE_SIZE");
    if (linx_mem_trace_enabled && linx_mem_trace_addr_filter_enabled &&
        size_s && size_s[0] &&
        strcmp(size_s, "0") != 0 && linx_parse_u64(size_s, &value)) {
        linx_mem_trace_size = value;
    }
    if (linx_mem_trace_addr_filter_enabled && linx_mem_trace_size == 0) {
        linx_mem_trace_size = 1;
    }

    limit_s = getenv("LINX_MEM_TRACE_LIMIT");
    if (linx_mem_trace_enabled && limit_s && limit_s[0] &&
        linx_parse_u64(limit_s, &value)) {
        linx_mem_trace_limit = value;
    }

    access_s = getenv("LINX_MEM_TRACE_ACCESS");
    if (linx_mem_trace_enabled && access_s && access_s[0]) {
        if (strcmp(access_s, "load") == 0 ||
            strcmp(access_s, "loads") == 0) {
            linx_mem_trace_loads = true;
            linx_mem_trace_stores = false;
        } else if (strcmp(access_s, "store") == 0 ||
                   strcmp(access_s, "stores") == 0) {
            linx_mem_trace_loads = false;
            linx_mem_trace_stores = true;
        } else {
            linx_mem_trace_loads = true;
            linx_mem_trace_stores = true;
        }
    }

    linx_mem_trace_context_enabled =
        linx_mem_trace_enabled && linx_env_enabled("LINX_MEM_TRACE_CONTEXT");
    linx_mem_trace_pre_enabled =
        linx_mem_trace_enabled && linx_env_enabled("LINX_MEM_TRACE_PRE");
    linx_mem_trace_regs_enabled =
        linx_mem_trace_enabled && linx_env_enabled("LINX_MEM_TRACE_REGS");

    acr_s = getenv("LINX_MEM_TRACE_ACR");
    if (linx_mem_trace_enabled && acr_s && acr_s[0] &&
        strcmp(acr_s, "any") != 0 && strcmp(acr_s, "all") != 0) {
        if (linx_parse_u64(acr_s, &value) && value <= 0xf) {
            linx_mem_trace_acr_filter_enabled = true;
            linx_mem_trace_acr_filter = (uint8_t)value;
        }
    }

    lo_s = getenv("LINX_MEM_TRACE_PC_LO");
    hi_s = getenv("LINX_MEM_TRACE_PC_HI");
    if (linx_mem_trace_enabled &&
        lo_s && lo_s[0] && strcmp(lo_s, "0") != 0 &&
        hi_s && hi_s[0] && strcmp(hi_s, "0") != 0 &&
        linx_parse_u64(lo_s, &lo) && linx_parse_u64(hi_s, &hi)) {
        linx_mem_trace_pc_lo = MIN(lo, hi);
        linx_mem_trace_pc_hi = MAX(lo, hi);
        linx_mem_trace_pc_filter_enabled = true;
    }

    count_lo_s = getenv("LINX_MEM_TRACE_COUNT_LO");
    count_hi_s = getenv("LINX_MEM_TRACE_COUNT_HI");
    if (linx_mem_trace_enabled &&
        count_lo_s && count_hi_s &&
        linx_parse_u64(count_lo_s, &lo) && linx_parse_u64(count_hi_s, &hi)) {
        linx_mem_trace_count_lo = MIN(lo, hi);
        linx_mem_trace_count_hi = MAX(lo, hi);
        linx_mem_trace_count_filter_enabled = true;
    }

    linx_mem_trace_inited = true;
}

static bool linx_mem_trace_ranges_overlap(uint64_t a, uint64_t a_size,
                                          uint64_t b, uint64_t b_size)
{
    uint64_t a_end;
    uint64_t b_end;

    if (a_size == 0) {
        a_size = 1;
    }
    if (b_size == 0) {
        b_size = 1;
    }

    a_end = a + a_size - 1;
    b_end = b + b_size - 1;
    if (a_end < a) {
        a_end = UINT64_MAX;
    }
    if (b_end < b) {
        b_end = UINT64_MAX;
    }

    return a <= b_end && b <= a_end;
}

static void linx_mem_trace_probe(CPULinxState *env, bool is_store,
                                 uint64_t pc, uint64_t addr, uint32_t size,
                                 uint64_t value, bool pre_access)
{
    linx_mem_trace_init();
    if (!linx_mem_trace_enabled) {
        return;
    }
    if (pre_access && !linx_mem_trace_pre_enabled) {
        return;
    }
    if (linx_mem_trace_limit &&
        linx_mem_trace_emitted >= linx_mem_trace_limit) {
        return;
    }
    if (is_store && !linx_mem_trace_stores) {
        return;
    }
    if (!is_store && !linx_mem_trace_loads) {
        return;
    }
    if (linx_mem_trace_acr_filter_enabled &&
        (uint8_t)(env->acr & 0xfu) != linx_mem_trace_acr_filter) {
        return;
    }
    if (linx_mem_trace_pc_filter_enabled &&
        (pc < linx_mem_trace_pc_lo || pc > linx_mem_trace_pc_hi)) {
        return;
    }
    if (linx_mem_trace_count_filter_enabled &&
        (env->insn_count < linx_mem_trace_count_lo ||
         env->insn_count > linx_mem_trace_count_hi)) {
        return;
    }
    if (linx_mem_trace_addr_filter_enabled &&
        !linx_mem_trace_ranges_overlap(addr, size,
                                       linx_mem_trace_addr,
                                       linx_mem_trace_size)) {
        return;
    }

    linx_mem_trace_emitted++;
    fprintf(stderr,
            "LINX_MEM_TRACE access=%s pc=0x%" PRIx64
            " addr=0x%" PRIx64 " size=%u value=0x%" PRIx64
            " count=%" PRIu64 " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64 " envpc=0x%" PRIx64
            " acr=%u cstate=0x%" PRIx64,
            is_store ? "store" : "load", pc, addr, size, value,
            env->insn_count, env->bpc, env->body_tpc, env->pc,
            (unsigned)(env->acr & 0xFu), env->ssr[0x20]);
    if (pre_access) {
        fprintf(stderr, " phase=pre");
    }
    if (linx_mem_trace_context_enabled) {
        const int mmu_idx = ((env->acr & 0xFu) == 2) ? 1 : 0;
        fprintf(stderr,
                " mmu_idx=%d ttbr0=0x%" PRIx64
                " ttbr1=0x%" PRIx64 " tcr=0x%" PRIx64,
                mmu_idx, env->ssr_acr[1][0xF10], env->ssr_acr[1][0xF11],
                env->ssr_acr[1][0xF12]);
    }
    if (linx_mem_trace_regs_enabled) {
        fprintf(stderr,
                " tq0=0x%" PRIx64 " tq1=0x%" PRIx64
                " tq2=0x%" PRIx64 " tq3=0x%" PRIx64
                " uq0=0x%" PRIx64 " uq1=0x%" PRIx64
                " uq2=0x%" PRIx64 " uq3=0x%" PRIx64,
                env->tq[0], env->tq[1], env->tq[2], env->tq[3],
                env->uq[0], env->uq[1], env->uq[2], env->uq[3]);
    }
    fprintf(stderr,
            " ra=0x%" PRIx64 " sp=0x%" PRIx64
            " a0=0x%" PRIx64 " a1=0x%" PRIx64
            " a2=0x%" PRIx64 "\n",
            env->gpr[LINX_REG_RA], env->gpr[LINX_REG_SP],
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1],
            env->gpr[LINX_REG_A2]);
    fflush(stderr);
}

void HELPER(linx_mem_trace_load_pre)(CPULinxState *env, uint64_t pc,
                                     uint64_t addr, uint32_t size,
                                     uint64_t value)
{
    linx_mem_trace_probe(env, false, pc, addr, size, value, true);
}

void HELPER(linx_mem_trace_load)(CPULinxState *env, uint64_t pc, uint64_t addr,
                                 uint32_t size, uint64_t value)
{
    linx_mem_trace_probe(env, false, pc, addr, size, value, false);
}

void HELPER(linx_mem_trace_store)(CPULinxState *env, uint64_t pc, uint64_t addr,
                                  uint32_t size, uint64_t value)
{
    linx_mem_trace_probe(env, true, pc, addr, size, value, false);
}

static void linx_syscall_trace_init(void)
{
    if (linx_syscall_trace_inited) {
        return;
    }

    const char *enabled_s = getenv("LINX_SYSCALL_TRACE");
    linx_syscall_trace_enabled =
        enabled_s && enabled_s[0] && strcmp(enabled_s, "0") != 0;

    const char *nr_s = getenv("LINX_SYSCALL_TRACE_NR");
    if (nr_s && nr_s[0] && strcmp(nr_s, "0") != 0) {
        char *copy = g_strdup(nr_s);
        char *saveptr = NULL;
        char *tok;

        for (tok = strtok_r(copy, ",", &saveptr);
             tok && linx_syscall_trace_nr_count < ARRAY_SIZE(linx_syscall_trace_nrs);
             tok = strtok_r(NULL, ",", &saveptr)) {
            uint64_t nr = 0;
            char *trimmed = g_strstrip(tok);

            if (!trimmed[0]) {
                continue;
            }
            if (linx_parse_u64(trimmed, &nr)) {
                bool duplicate = false;

                for (unsigned i = 0; i < linx_syscall_trace_nr_count; i++) {
                    if (linx_syscall_trace_nrs[i] == nr) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    linx_syscall_trace_nrs[
                        linx_syscall_trace_nr_count++] = nr;
                }
            }
        }
        g_free(copy);
        linx_syscall_trace_nr_filter_enabled =
            linx_syscall_trace_nr_count != 0;
    }

    const char *limit_s = getenv("LINX_SYSCALL_TRACE_LIMIT");
    if (limit_s && limit_s[0] && strcmp(limit_s, "0") != 0) {
        (void)linx_parse_u64(limit_s, &linx_syscall_trace_limit);
    }

    uint64_t lo = 0;
    uint64_t hi = 0;
    const char *lo_s = getenv("LINX_SYSCALL_TRACE_PC_LO");
    const char *hi_s = getenv("LINX_SYSCALL_TRACE_PC_HI");
    if (lo_s && lo_s[0] && strcmp(lo_s, "0") != 0 &&
        hi_s && hi_s[0] && strcmp(hi_s, "0") != 0 &&
        linx_parse_u64(lo_s, &lo) && linx_parse_u64(hi_s, &hi)) {
        linx_syscall_trace_pc_lo = MIN(lo, hi);
        linx_syscall_trace_pc_hi = MAX(lo, hi);
        linx_syscall_trace_pc_filter_enabled = true;
    }

    const char *strings_s = getenv("LINX_SYSCALL_TRACE_STRINGS");
    linx_syscall_trace_strings_enabled =
        strings_s && strings_s[0] && strcmp(strings_s, "0") != 0;

    linx_syscall_trace_regs_enabled =
        linx_env_enabled("LINX_SYSCALL_TRACE_REGS") ||
        linx_env_enabled("LINX_TRACE_REGS");

    const char *string_max_s = getenv("LINX_SYSCALL_TRACE_STRING_MAX");
    if (string_max_s && string_max_s[0] &&
        strcmp(string_max_s, "0") != 0) {
        (void)linx_parse_u64(string_max_s, &linx_syscall_trace_string_max);
    }
    if (linx_syscall_trace_string_max == 0) {
        linx_syscall_trace_string_max = 1;
    }
    if (linx_syscall_trace_string_max > 255) {
        linx_syscall_trace_string_max = 255;
    }

    const char *dump_args_s = getenv("LINX_SYSCALL_TRACE_DUMP_ARGS");
    if (dump_args_s && dump_args_s[0]) {
        char *copy = g_strdup(dump_args_s);
        char *saveptr = NULL;
        char *tok;

        for (tok = strtok_r(copy, ",", &saveptr);
             tok && linx_syscall_trace_dump_arg_count < ARRAY_SIZE(linx_syscall_trace_dump_args);
             tok = strtok_r(NULL, ",", &saveptr)) {
            uint64_t arg = 0;
            char *trimmed = g_strstrip(tok);

            if (!trimmed[0]) {
                continue;
            }
            if (linx_parse_u64(trimmed, &arg) && arg < 6) {
                bool duplicate = false;

                for (unsigned i = 0; i < linx_syscall_trace_dump_arg_count; i++) {
                    if (linx_syscall_trace_dump_args[i] == (unsigned)arg) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    linx_syscall_trace_dump_args[
                        linx_syscall_trace_dump_arg_count++] = (unsigned)arg;
                }
            }
        }
        g_free(copy);
    }

    const char *dump_arg_s = getenv("LINX_SYSCALL_TRACE_DUMP_ARG");
    if (dump_arg_s && dump_arg_s[0]) {
        uint64_t arg = 0;
        if (linx_parse_u64(dump_arg_s, &arg) && arg < 6) {
            linx_syscall_trace_dump_arg_enabled = true;
            linx_syscall_trace_dump_arg = (unsigned)arg;
            bool duplicate = false;

            for (unsigned i = 0; i < linx_syscall_trace_dump_arg_count; i++) {
                if (linx_syscall_trace_dump_args[i] == linx_syscall_trace_dump_arg) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate &&
                linx_syscall_trace_dump_arg_count < ARRAY_SIZE(linx_syscall_trace_dump_args)) {
                linx_syscall_trace_dump_args[
                    linx_syscall_trace_dump_arg_count++] = linx_syscall_trace_dump_arg;
            }
        }
    }

    const char *dump_bytes_s = getenv("LINX_SYSCALL_TRACE_DUMP_BYTES");
    if (dump_bytes_s && dump_bytes_s[0] &&
        strcmp(dump_bytes_s, "0") != 0) {
        uint64_t bytes = 0;
        if (linx_parse_u64(dump_bytes_s, &bytes)) {
            linx_syscall_trace_dump_bytes = MIN((uint64_t)256, bytes);
        }
    }
    if ((linx_syscall_trace_dump_arg_enabled ||
         linx_syscall_trace_dump_arg_count != 0) &&
        linx_syscall_trace_dump_bytes == 0) {
        linx_syscall_trace_dump_bytes = 64;
    }

    linx_syscall_trace_inited = true;
}

static void linx_syscall_trace_emit_regs(CPULinxState *env,
                                         const char *phase, uint64_t nr,
                                         uint64_t bpc, uint64_t tpc)
{
    if (!linx_syscall_trace_regs_enabled) {
        return;
    }

    fprintf(stderr,
            "LINX_SYSCALL_REGS phase=%s nr=%" PRIu64
            " count=%" PRIu64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64,
            phase, nr, env->insn_count, bpc, tpc);
    linx_fprint_gprs(stderr, env);
    fputc('\n', stderr);
}

static bool linx_syscall_trace_matches(uint64_t nr, uint64_t bpc)
{
    if (!linx_syscall_trace_enabled) {
        return false;
    }
    if (linx_syscall_trace_nr_filter_enabled) {
        bool matched = false;

        for (unsigned i = 0; i < linx_syscall_trace_nr_count; i++) {
            if (linx_syscall_trace_nrs[i] == nr) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    if (linx_syscall_trace_pc_filter_enabled &&
        (bpc < linx_syscall_trace_pc_lo || bpc > linx_syscall_trace_pc_hi)) {
        return false;
    }
    if (linx_syscall_trace_limit != 0 &&
        linx_syscall_trace_emitted >= linx_syscall_trace_limit) {
        return false;
    }
    return true;
}

static bool linx_syscall_read_guest_string(CPULinxState *env, uint64_t addr,
                                           char *buf, size_t buf_size,
                                           bool *truncated)
{
    CPUState *cs = env_cpu(env);
    size_t limit = MIN((size_t)linx_syscall_trace_string_max, buf_size - 1);

    *truncated = true;
    if (addr == 0 || buf_size < 2) {
        buf[0] = '\0';
        return false;
    }

    for (size_t i = 0; i < limit; i++) {
        uint8_t ch = 0;
        if (cpu_memory_rw_debug(cs, addr + i, &ch, 1, 0) != 0) {
            buf[i] = '\0';
            return false;
        }
        buf[i] = (char)ch;
        if (ch == 0) {
            *truncated = false;
            return true;
        }
    }

    buf[limit] = '\0';
    return true;
}

static void linx_syscall_fprint_guest_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '\\':
            fputs("\\\\", f);
            break;
        case '"':
            fputs("\\\"", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            if (*p >= 0x20 && *p < 0x7f) {
                fputc(*p, f);
            } else {
                fprintf(f, "\\x%02x", *p);
            }
            break;
        }
    }
    fputc('"', f);
}

static unsigned linx_syscall_trace_string_args(uint64_t nr,
                                               unsigned args[2])
{
    switch (nr) {
    case 33:  /* mknodat */
    case 34:  /* mkdirat */
    case 35:  /* unlinkat */
    case 48:  /* faccessat */
    case 53:  /* fchmodat */
    case 54:  /* fchownat */
    case 56:  /* openat */
    case 78:  /* readlinkat */
    case 79:  /* newfstatat */
    case 291: /* statx */
        args[0] = 1;
        return 1;
    case 36:  /* symlinkat */
        args[0] = 0;
        args[1] = 2;
        return 2;
    case 37:  /* linkat */
    case 38:  /* renameat */
        args[0] = 1;
        args[1] = 3;
        return 2;
    case 49:  /* chdir */
    case 51:  /* chroot */
    case 221: /* execve */
        args[0] = 0;
        return 1;
    default:
        return 0;
    }
}

static void linx_syscall_trace_emit_strings(CPULinxState *env, uint64_t nr,
                                            uint64_t bpc, uint64_t tpc)
{
    unsigned arg_idx[2];
    char str[256];
    unsigned count;

    if (!linx_syscall_trace_strings_enabled) {
        return;
    }

    count = linx_syscall_trace_string_args(nr, arg_idx);
    for (unsigned i = 0; i < count; i++) {
        bool truncated = false;
        uint64_t addr = env->gpr[LINX_REG_A0 + arg_idx[i]];
        bool ok = linx_syscall_read_guest_string(env, addr, str, sizeof(str),
                                                 &truncated);
        fprintf(stderr,
                "LINX_SYSCALL_ARGSTR nr=%" PRIu64
                " count=%" PRIu64
                " bpc=0x%" PRIx64
                " tpc=0x%" PRIx64
                " arg=%u"
                " addr=0x%" PRIx64
                " ok=%u"
                " truncated=%u"
                " value=",
                nr, env->insn_count, bpc, tpc, arg_idx[i], addr,
                ok ? 1 : 0, truncated ? 1 : 0);
        if (ok) {
            linx_syscall_fprint_guest_string(stderr, str);
            if (truncated) {
                fputs("...", stderr);
            }
        } else {
            fputs("<unreadable>", stderr);
        }
        fputc('\n', stderr);
    }
}

static void linx_syscall_trace_emit_argdump(CPULinxState *env, uint64_t nr,
                                            uint64_t bpc, uint64_t tpc,
                                            uint64_t ret)
{
    CPUState *cs;

    if (linx_syscall_trace_dump_arg_count == 0 ||
        linx_syscall_trace_dump_bytes == 0) {
        return;
    }

    cs = env_cpu(env);
    for (unsigned arg_i = 0; arg_i < linx_syscall_trace_dump_arg_count; arg_i++) {
        uint8_t bytes[256] = { 0 };
        const unsigned arg = linx_syscall_trace_dump_args[arg_i];
        const uint64_t addr = env->syscall_trace_args[arg];
        const int rc = cpu_memory_rw_debug(cs, addr, bytes,
                                           linx_syscall_trace_dump_bytes, 0);

        fprintf(stderr,
                "LINX_SYSCALL_ARGDUMP phase=return nr=%" PRIu64
                " count=%" PRIu64
                " bpc=0x%" PRIx64
                " tpc=0x%" PRIx64
                " arg=%u"
                " addr=0x%" PRIx64
                " bytes=%" PRIu64
                " rc=%d"
                " ret=0x%" PRIx64
                " data=",
                nr, env->insn_count, bpc, tpc, arg,
                addr, linx_syscall_trace_dump_bytes, rc, ret);
        if (rc == 0) {
            for (uint64_t i = 0; i < linx_syscall_trace_dump_bytes; i++) {
                fprintf(stderr, "%02x", bytes[i]);
            }
        } else {
            fputs("<fault>", stderr);
        }
        fputc('\n', stderr);
    }
}

static void linx_syscall_trace_unpaired_maybe_emit(CPULinxState *env,
                                                   uint64_t next_nr,
                                                   uint64_t next_bpc,
                                                   uint64_t next_tpc)
{
    if (!env->syscall_trace_pending || !env->syscall_trace_entry_emitted) {
        return;
    }
    if (linx_syscall_trace_limit != 0 &&
        linx_syscall_trace_emitted >= linx_syscall_trace_limit) {
        return;
    }

    linx_syscall_trace_emitted++;
    fprintf(stderr,
            "LINX_SYSCALL_UNPAIRED nr=%" PRIu64
            " count=%" PRIu64
            " entry_bpc=0x%" PRIx64
            " entry_tpc=0x%" PRIx64
            " entry_pc_next=0x%" PRIx64
            " next_nr=%" PRIu64
            " next_bpc=0x%" PRIx64
            " next_tpc=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64
            " a2=0x%" PRIx64
            " a3=0x%" PRIx64
            " a4=0x%" PRIx64
            " a5=0x%" PRIx64
            " sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " cstate=0x%" PRIx64,
            env->syscall_trace_nr, env->insn_count,
            env->syscall_trace_bpc, env->syscall_trace_tpc,
            env->syscall_trace_pc_next, next_nr, next_bpc, next_tpc,
            env->syscall_trace_args[0], env->syscall_trace_args[1],
            env->syscall_trace_args[2], env->syscall_trace_args[3],
            env->syscall_trace_args[4], env->syscall_trace_args[5],
            env->syscall_trace_sp, env->syscall_trace_ra,
            env->syscall_trace_cstate);
    fputc('\n', stderr);
    fflush(stderr);
}

static void linx_syscall_trace_maybe_emit(CPULinxState *env, uint32_t src_acr,
                                          uint32_t dst_acr, uint64_t bpc,
                                          uint64_t tpc, uint64_t pc_next)
{
    const uint64_t nr = env->gpr[LINX_REG_A7];

    linx_syscall_trace_init();
    linx_syscall_trace_unpaired_maybe_emit(env, nr, bpc, tpc);

    env->syscall_trace_pending = 1;
    env->syscall_trace_entry_emitted = 0;
    env->syscall_trace_nr = nr;
    env->syscall_trace_bpc = bpc;
    env->syscall_trace_tpc = tpc;
    env->syscall_trace_pc_next = pc_next;
    env->syscall_trace_args[0] = env->gpr[LINX_REG_A0];
    env->syscall_trace_args[1] = env->gpr[LINX_REG_A1];
    env->syscall_trace_args[2] = env->gpr[LINX_REG_A2];
    env->syscall_trace_args[3] = env->gpr[LINX_REG_A3];
    env->syscall_trace_args[4] = env->gpr[LINX_REG_A4];
    env->syscall_trace_args[5] = env->gpr[LINX_REG_A5];
    env->syscall_trace_sp = env->gpr[LINX_REG_SP];
    env->syscall_trace_ra = env->gpr[LINX_REG_RA];
    env->syscall_trace_cstate = env->ssr[0x20];

    if (!linx_syscall_trace_matches(nr, bpc)) {
        return;
    }

    linx_syscall_trace_emitted++;
    env->syscall_trace_entry_emitted = 1;
    fprintf(stderr,
            "LINX_SYSCALL_TRACE nr=%" PRIu64
            " src_acr=%u dst_acr=%u count=%" PRIu64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " pc_next=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64
            " a2=0x%" PRIx64
            " a3=0x%" PRIx64
            " a4=0x%" PRIx64
            " a5=0x%" PRIx64
            " sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " cstate=0x%" PRIx64
            "\n",
            nr, src_acr, dst_acr, env->insn_count, bpc, tpc, pc_next,
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1],
            env->gpr[LINX_REG_A2], env->gpr[LINX_REG_A3],
            env->gpr[LINX_REG_A4], env->gpr[LINX_REG_A5],
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->ssr[0x20]);
    linx_syscall_trace_emit_regs(env, "entry", nr, bpc, tpc);
    linx_syscall_trace_emit_strings(env, nr, bpc, tpc);
    fflush(stderr);
}

static void linx_syscall_trace_return_maybe_emit(CPULinxState *env,
                                                 uint32_t mgr,
                                                 uint32_t target,
                                                 uint64_t bpc,
                                                 uint64_t tpc,
                                                 uint64_t resume_pc)
{
    const bool entry_emitted = env->syscall_trace_pending &&
        env->syscall_trace_entry_emitted;
    const uint64_t nr = env->syscall_trace_pending ?
        env->syscall_trace_nr : env->gpr[LINX_REG_A7];
    const uint64_t entry_bpc = env->syscall_trace_pending ?
        env->syscall_trace_bpc : bpc;
    const uint64_t entry_tpc = env->syscall_trace_pending ?
        env->syscall_trace_tpc : tpc;

    linx_syscall_trace_init();
    if (!linx_syscall_trace_enabled || !entry_emitted) {
        env->syscall_trace_pending = 0;
        env->syscall_trace_entry_emitted = 0;
        return;
    }

    if (!linx_syscall_trace_matches(nr, entry_bpc)) {
        env->syscall_trace_pending = 0;
        env->syscall_trace_entry_emitted = 0;
        return;
    }

    linx_syscall_trace_emitted++;
    fprintf(stderr,
            "LINX_SYSCALL_RETURN nr=%" PRIu64
            " mgr=%u target=%u count=%" PRIu64
            " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64
            " resume=0x%" PRIx64
            " entry_bpc=0x%" PRIx64
            " entry_tpc=0x%" PRIx64
            " ret=%" PRId64
            " ret_hex=0x%" PRIx64
            " entry_a0=0x%" PRIx64
            " entry_a1=0x%" PRIx64
            " entry_a2=0x%" PRIx64
            " entry_a3=0x%" PRIx64
            " entry_a4=0x%" PRIx64
            " entry_a5=0x%" PRIx64
            " a1=0x%" PRIx64
            " a2=0x%" PRIx64
            " sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " cstate=0x%" PRIx64
            "\n",
            nr, mgr, target, env->insn_count, bpc, tpc, resume_pc,
            entry_bpc, entry_tpc,
            (int64_t)env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A0],
            env->syscall_trace_args[0], env->syscall_trace_args[1],
            env->syscall_trace_args[2], env->syscall_trace_args[3],
            env->syscall_trace_args[4], env->syscall_trace_args[5],
            env->gpr[LINX_REG_A1], env->gpr[LINX_REG_A2],
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->ssr[0x20]);
    linx_syscall_trace_emit_argdump(env, nr, bpc, tpc,
                                    env->gpr[LINX_REG_A0]);
    linx_syscall_trace_emit_regs(env, "return", nr, bpc, tpc);
    env->syscall_trace_pending = 0;
    env->syscall_trace_entry_emitted = 0;
    fflush(stderr);
}

static inline QEMU_ALWAYS_INLINE bool linx_call_trace_disabled_fast(void)
{
    return linx_call_trace_inited &&
           !linx_call_trace_enabled &&
           !linx_call_trace_ring_enabled;
}

static void linx_call_trace_emit_slow(CPULinxState *env, uint32_t event,
                                      uint64_t pc, uint64_t extra0,
                                      uint64_t extra1)
{
    linx_call_trace_init();
    if (!linx_call_trace_enabled && !linx_call_trace_ring_enabled) {
        return;
    }
    if (linx_call_trace_ring_enabled) {
        linx_call_trace_ring_record(env, event, pc, extra0, extra1);
    }

    if (!linx_call_trace_matches(env, pc, extra0, extra1)) {
        return;
    }

    fprintf(stderr,
            "LINX_CALL_TRACE event=%s pc=0x%" PRIx64
            " extra0=0x%" PRIx64 " extra1=0x%" PRIx64
            " count=%" PRIu64
            " envpc=0x%" PRIx64 " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64 " acr=%u cstate=0x%" PRIx64
            " brtype=%u tgt=0x%" PRIx64
            " ra=0x%" PRIx64 " sp=0x%" PRIx64
            " a0=0x%" PRIx64 " a1=0x%" PRIx64
            " a2=0x%" PRIx64
            " call_ra_set=%u call_setret_pending=%u"
            " in_body=%u body_tpc=0x%" PRIx64
            " return_pc=0x%" PRIx64
            " tmpl_kind=%u tmpl_pc=0x%" PRIx64
            " tmpl_step=%u\n",
            linx_call_trace_event_name(event), pc, extra0, extra1,
            env->insn_count, env->pc, env->bpc, env->body_tpc,
            env->acr & 0xFu, env->ssr[0x20],
            env->brtype, env->tgt, env->gpr[LINX_REG_RA],
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_A0],
            env->gpr[LINX_REG_A1], env->gpr[LINX_REG_A2],
            env->call_ra_set, env->call_setret_pending, env->in_body,
            env->body_tpc, env->return_pc, env->tmpl_kind,
            env->tmpl_pc, env->tmpl_step);
    fflush(stderr);
}

static inline QEMU_ALWAYS_INLINE void linx_call_trace_emit(CPULinxState *env,
                                                          uint32_t event,
                                                          uint64_t pc,
                                                          uint64_t extra0,
                                                          uint64_t extra1)
{
    if (linx_call_trace_disabled_fast()) {
        return;
    }
    linx_call_trace_emit_slow(env, event, pc, extra0, extra1);
}

static void linx_fret_stk_trace_init(void)
{
    uint64_t lo = 0;
    uint64_t hi = UINT64_MAX;
    const char *lo_s;
    const char *hi_s;
    const char *value_s;

    if (linx_fret_stk_trace_inited) {
        return;
    }
    linx_fret_stk_trace_inited = true;

    linx_fret_stk_trace_enabled =
        linx_env_enabled("LINX_FRET_STK_TRACE") ||
        linx_env_enabled("LINX_QEMU_FRET_STK_TRACE");

    lo_s = linx_env_value2("LINX_FRET_STK_TRACE_PC_LO",
                           "LINX_QEMU_FRET_STK_TRACE_PC_LO");
    hi_s = linx_env_value2("LINX_FRET_STK_TRACE_PC_HI",
                           "LINX_QEMU_FRET_STK_TRACE_PC_HI");
    const bool have_pc_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_pc_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_pc_lo || have_pc_hi) {
        linx_fret_stk_trace_pc_lo = MIN(lo, hi);
        linx_fret_stk_trace_pc_hi = MAX(lo, hi);
        linx_fret_stk_trace_pc_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_FRET_STK_TRACE_PC",
                              "LINX_QEMU_FRET_STK_TRACE_PC");
    if (value_s && linx_parse_u64(value_s, &lo)) {
        linx_fret_stk_trace_pc_lo = lo;
        linx_fret_stk_trace_pc_hi = lo;
        linx_fret_stk_trace_pc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_env_value2("LINX_FRET_STK_TRACE_COUNT_LO",
                           "LINX_QEMU_FRET_STK_TRACE_COUNT_LO");
    hi_s = linx_env_value2("LINX_FRET_STK_TRACE_COUNT_HI",
                           "LINX_QEMU_FRET_STK_TRACE_COUNT_HI");
    const bool have_count_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_count_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_count_lo || have_count_hi) {
        linx_fret_stk_trace_count_lo = MIN(lo, hi);
        linx_fret_stk_trace_count_hi = MAX(lo, hi);
        linx_fret_stk_trace_count_filter_enabled = true;
    }

    value_s = linx_env_value2("LINX_FRET_STK_TRACE_RA",
                              "LINX_QEMU_FRET_STK_TRACE_RA");
    if (value_s && linx_parse_u64(value_s, &linx_fret_stk_trace_ra)) {
        linx_fret_stk_trace_ra_filter_enabled = true;
    }

    value_s = linx_env_nonzero2("LINX_FRET_STK_TRACE_LIMIT",
                                "LINX_QEMU_FRET_STK_TRACE_LIMIT");
    if (value_s) {
        (void)linx_parse_u64(value_s, &linx_fret_stk_trace_limit);
    } else {
        linx_fret_stk_trace_limit = 64;
    }

    value_s = linx_env_nonzero2("LINX_FRET_STK_TRACE_DUMP_WORDS",
                                "LINX_QEMU_FRET_STK_TRACE_DUMP_WORDS");
    if (value_s) {
        uint64_t words = 0;
        if (linx_parse_u64(value_s, &words) && words != 0) {
            linx_fret_stk_trace_dump_words = MIN(words, (uint64_t)32);
        }
    }

    linx_fret_stk_trace_regs_enabled =
        linx_env_enabled("LINX_FRET_STK_TRACE_REGS") ||
        linx_env_enabled("LINX_QEMU_FRET_STK_TRACE_REGS") ||
        linx_env_enabled("LINX_TRACE_REGS");
}

static inline QEMU_ALWAYS_INLINE bool linx_fret_stk_trace_enabled_fast(void)
{
    if (linx_fret_stk_trace_inited && !linx_fret_stk_trace_enabled) {
        return false;
    }
    linx_fret_stk_trace_init();
    return linx_fret_stk_trace_enabled;
}

static bool linx_fret_stk_trace_matches(CPULinxState *env, uint64_t pc,
                                        uint64_t restored_ra)
{
    if (linx_fret_stk_trace_inited && !linx_fret_stk_trace_enabled) {
        return false;
    }
    linx_fret_stk_trace_init();
    if (!linx_fret_stk_trace_enabled) {
        return false;
    }
    if (linx_fret_stk_trace_limit &&
        linx_fret_stk_trace_emitted >= linx_fret_stk_trace_limit) {
        return false;
    }
    if (linx_fret_stk_trace_pc_filter_enabled &&
        (pc < linx_fret_stk_trace_pc_lo ||
         pc > linx_fret_stk_trace_pc_hi)) {
        return false;
    }
    if (linx_fret_stk_trace_count_filter_enabled &&
        (env->insn_count < linx_fret_stk_trace_count_lo ||
         env->insn_count > linx_fret_stk_trace_count_hi)) {
        return false;
    }
    if (linx_fret_stk_trace_ra_filter_enabled &&
        restored_ra != linx_fret_stk_trace_ra) {
        return false;
    }
    linx_fret_stk_trace_emitted++;
    return true;
}

static void linx_fret_stk_trace_emit(CPULinxState *env, uint64_t cur_pc,
                                     uint64_t next_pc, uint64_t old_sp,
                                     uint64_t new_sp, uint64_t stacksize,
                                     uint64_t restore_base, int begin,
                                     int end,
                                     const uint32_t regs[LINX_GPR_COUNT],
                                     const uint64_t addrs[LINX_GPR_COUNT],
                                     const uint64_t values[LINX_GPR_COUNT],
                                     int count)
{
    uint64_t restored_ra = env->gpr[LINX_REG_RA];

    if (!linx_fret_stk_trace_enabled_fast()) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (regs[i] == LINX_REG_RA) {
            restored_ra = values[i];
            break;
        }
    }
    if (!linx_fret_stk_trace_matches(env, cur_pc, restored_ra)) {
        return;
    }

    fprintf(stderr,
            "LINX_FRET_STK_TRACE count=%" PRIu64
            " pc=0x%" PRIx64 " next_pc=0x%" PRIx64
            " old_sp=0x%" PRIx64 " new_sp=0x%" PRIx64
            " stacksize=%" PRIu64 " callframe=%" PRIu64
            " restore_base=%" PRIu64
            " begin=%s end=%s restore_count=%d"
            " incoming_ra=0x%" PRIx64 " restored_ra=0x%" PRIx64
            " envpc=0x%" PRIx64 " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64 " cstate=0x%" PRIx64
            " brtype=%u tgt=0x%" PRIx64 "\n",
            env->insn_count, cur_pc, next_pc, old_sp, new_sp, stacksize,
            linx_callframe_size, restore_base,
            (begin >= 0 && begin < LINX_GPR_COUNT) ? linx_gpr_names[begin] : "?",
            (end >= 0 && end < LINX_GPR_COUNT) ? linx_gpr_names[end] : "?",
            count, env->gpr[LINX_REG_RA], restored_ra, env->pc, env->bpc,
            env->body_tpc, env->ssr[0x20], env->brtype, env->tgt);

    for (int i = 0; i < count; i++) {
        const uint32_t reg = regs[i];
        fprintf(stderr,
                "LINX_FRET_STK_SLOT count=%" PRIu64
                " pc=0x%" PRIx64 " reg=%s addr=0x%" PRIx64
                " value=0x%" PRIx64 "\n",
                env->insn_count, cur_pc,
                reg < LINX_GPR_COUNT ? linx_gpr_names[reg] : "?",
                addrs[i], values[i]);
    }
    if (linx_fret_stk_trace_regs_enabled) {
        fprintf(stderr,
                "LINX_FRET_STK_REGS count=%" PRIu64
                " pc=0x%" PRIx64,
                env->insn_count, cur_pc);
        linx_fprint_gprs(stderr, env);
        fputc('\n', stderr);
    }
    if (linx_fret_stk_trace_dump_words) {
        linx_debug_dump_guest_units(env, old_sp,
                                    linx_fret_stk_trace_dump_words,
                                    "  fret_sp", 8);
    }
    fflush(stderr);
}

static void linx_fentry_trace_init(void)
{
    uint64_t lo = 0;
    uint64_t hi = UINT64_MAX;
    const char *lo_s;
    const char *hi_s;
    const char *value_s;

    if (linx_fentry_trace_inited) {
        return;
    }
    linx_fentry_trace_inited = true;

    linx_fentry_trace_enabled =
        linx_env_enabled("LINX_FENTRY_TRACE") ||
        linx_env_enabled("LINX_QEMU_FENTRY_TRACE");

    lo_s = linx_env_value2("LINX_FENTRY_TRACE_PC_LO",
                           "LINX_QEMU_FENTRY_TRACE_PC_LO");
    hi_s = linx_env_value2("LINX_FENTRY_TRACE_PC_HI",
                           "LINX_QEMU_FENTRY_TRACE_PC_HI");
    const bool have_pc_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_pc_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_pc_lo || have_pc_hi) {
        linx_fentry_trace_pc_lo = MIN(lo, hi);
        linx_fentry_trace_pc_hi = MAX(lo, hi);
        linx_fentry_trace_pc_filter_enabled = true;
    }
    value_s = linx_env_value2("LINX_FENTRY_TRACE_PC",
                              "LINX_QEMU_FENTRY_TRACE_PC");
    if (value_s && linx_parse_u64(value_s, &lo)) {
        linx_fentry_trace_pc_lo = lo;
        linx_fentry_trace_pc_hi = lo;
        linx_fentry_trace_pc_filter_enabled = true;
    }

    lo = 0;
    hi = UINT64_MAX;
    lo_s = linx_env_value2("LINX_FENTRY_TRACE_COUNT_LO",
                           "LINX_QEMU_FENTRY_TRACE_COUNT_LO");
    hi_s = linx_env_value2("LINX_FENTRY_TRACE_COUNT_HI",
                           "LINX_QEMU_FENTRY_TRACE_COUNT_HI");
    const bool have_count_lo = lo_s && linx_parse_u64(lo_s, &lo);
    const bool have_count_hi = hi_s && linx_parse_u64(hi_s, &hi);
    if (have_count_lo || have_count_hi) {
        linx_fentry_trace_count_lo = MIN(lo, hi);
        linx_fentry_trace_count_hi = MAX(lo, hi);
        linx_fentry_trace_count_filter_enabled = true;
    }

    value_s = linx_env_value2("LINX_FENTRY_TRACE_RA",
                              "LINX_QEMU_FENTRY_TRACE_RA");
    if (value_s && linx_parse_u64(value_s, &linx_fentry_trace_ra)) {
        linx_fentry_trace_ra_filter_enabled = true;
    }

    value_s = linx_env_value2("LINX_FENTRY_TRACE_SP",
                              "LINX_QEMU_FENTRY_TRACE_SP");
    if (value_s && linx_parse_u64(value_s, &linx_fentry_trace_sp)) {
        linx_fentry_trace_sp_filter_enabled = true;
    }

    value_s = linx_env_value2("LINX_FENTRY_TRACE_NEW_SP",
                              "LINX_QEMU_FENTRY_TRACE_NEW_SP");
    if (value_s && linx_parse_u64(value_s, &linx_fentry_trace_new_sp)) {
        linx_fentry_trace_new_sp_filter_enabled = true;
    }

    value_s = linx_env_nonzero2("LINX_FENTRY_TRACE_LIMIT",
                                "LINX_QEMU_FENTRY_TRACE_LIMIT");
    if (value_s) {
        (void)linx_parse_u64(value_s, &linx_fentry_trace_limit);
    } else {
        linx_fentry_trace_limit = 64;
    }

    value_s = linx_env_nonzero2("LINX_FENTRY_TRACE_DUMP_WORDS",
                                "LINX_QEMU_FENTRY_TRACE_DUMP_WORDS");
    if (value_s) {
        uint64_t words = 0;
        if (linx_parse_u64(value_s, &words) && words != 0) {
            linx_fentry_trace_dump_words = MIN(words, (uint64_t)32);
        }
    }

    linx_fentry_trace_regs_enabled =
        linx_env_enabled("LINX_FENTRY_TRACE_REGS") ||
        linx_env_enabled("LINX_QEMU_FENTRY_TRACE_REGS") ||
        linx_env_enabled("LINX_TRACE_REGS");
}

static inline QEMU_ALWAYS_INLINE bool linx_fentry_trace_enabled_fast(void)
{
    if (linx_fentry_trace_inited && !linx_fentry_trace_enabled) {
        return false;
    }
    linx_fentry_trace_init();
    return linx_fentry_trace_enabled;
}

static bool linx_fentry_trace_matches(CPULinxState *env, uint64_t pc,
                                      uint64_t old_sp, uint64_t new_sp,
                                      uint64_t save_ra)
{
    if (linx_fentry_trace_inited && !linx_fentry_trace_enabled) {
        return false;
    }
    linx_fentry_trace_init();
    if (!linx_fentry_trace_enabled) {
        return false;
    }
    if (linx_fentry_trace_limit &&
        linx_fentry_trace_emitted >= linx_fentry_trace_limit) {
        return false;
    }
    if (linx_fentry_trace_pc_filter_enabled &&
        (pc < linx_fentry_trace_pc_lo ||
         pc > linx_fentry_trace_pc_hi)) {
        return false;
    }
    if (linx_fentry_trace_count_filter_enabled &&
        (env->insn_count < linx_fentry_trace_count_lo ||
         env->insn_count > linx_fentry_trace_count_hi)) {
        return false;
    }
    if (linx_fentry_trace_ra_filter_enabled &&
        save_ra != linx_fentry_trace_ra) {
        return false;
    }
    if (linx_fentry_trace_sp_filter_enabled &&
        old_sp != linx_fentry_trace_sp) {
        return false;
    }
    if (linx_fentry_trace_new_sp_filter_enabled &&
        new_sp != linx_fentry_trace_new_sp) {
        return false;
    }
    linx_fentry_trace_emitted++;
    return true;
}

static void linx_fentry_trace_begin(CPULinxState *env, uint64_t cur_pc,
                                    uint64_t next_pc, uint64_t old_sp,
                                    uint64_t new_sp, uint64_t stacksize,
                                    int begin, int end, int count,
                                    int mmu_idx)
{
    fprintf(stderr,
            "LINX_FENTRY_TRACE count=%" PRIu64
            " pc=0x%" PRIx64 " next_pc=0x%" PRIx64
            " old_sp=0x%" PRIx64 " new_sp=0x%" PRIx64
            " stacksize=%" PRIu64 " callframe=%" PRIu64
            " begin=%s end=%s save_count=%d"
            " incoming_ra=0x%" PRIx64
            " envpc=0x%" PRIx64 " bpc=0x%" PRIx64
            " tpc=0x%" PRIx64 " cstate=0x%" PRIx64
            " acr=%u mmu=%d brtype=%u tgt=0x%" PRIx64 "\n",
            env->insn_count, cur_pc, next_pc, old_sp, new_sp, stacksize,
            linx_callframe_size,
            (begin >= 0 && begin < LINX_GPR_COUNT) ? linx_gpr_names[begin] : "?",
            (end >= 0 && end < LINX_GPR_COUNT) ? linx_gpr_names[end] : "?",
            count, env->gpr[LINX_REG_RA], env->pc, env->bpc,
            env->body_tpc, env->ssr[0x20], (unsigned)(env->acr & 0xfu),
            mmu_idx, env->brtype, env->tgt);
}

static void linx_fentry_trace_slot(CPULinxState *env, uint64_t cur_pc,
                                   uint32_t reg, uint64_t addr,
                                   uint64_t value, int mmu_idx, void *host)
{
    uint64_t debug_readback = 0;
    uint64_t host_readback = 0;
    const bool debug_read_ok =
        linx_debug_read_guest_u64(env, addr, &debug_readback);
    const uint64_t mmu_readback =
        cpu_ldq_mmu((CPUArchState *)env, addr,
                    make_memop_idx(MO_LEUQ, mmu_idx), GETPC());
    if (host) {
        host_readback = ldq_le_p(host);
    }

    fprintf(stderr,
            "LINX_FENTRY_SLOT count=%" PRIu64
            " pc=0x%" PRIx64 " reg=%s addr=0x%" PRIx64
            " value=0x%" PRIx64 " mmu=%d mmu_readback=0x%" PRIx64
            " host=%p host_readback=0x%" PRIx64
            " debug_read_ok=%u debug_readback=0x%" PRIx64 "\n",
            env->insn_count, cur_pc,
            reg < LINX_GPR_COUNT ? linx_gpr_names[reg] : "?",
            addr, value, mmu_idx, mmu_readback, host, host_readback,
            debug_read_ok ? 1u : 0u, debug_readback);
}

static void linx_fentry_trace_end(CPULinxState *env, uint64_t cur_pc,
                                  uint64_t new_sp)
{
    if (linx_fentry_trace_regs_enabled) {
        fprintf(stderr,
                "LINX_FENTRY_REGS count=%" PRIu64
                " pc=0x%" PRIx64,
                env->insn_count, cur_pc);
        linx_fprint_gprs(stderr, env);
        fputc('\n', stderr);
    }
    if (linx_fentry_trace_dump_words) {
        linx_debug_dump_guest_units(env, new_sp,
                                    linx_fentry_trace_dump_words,
                                    "  fentry_sp", 8);
    }
    fflush(stderr);
}

void HELPER(linx_call_trace_event)(CPULinxState *env, uint64_t pc,
                                   uint32_t event, uint64_t extra0,
                                   uint64_t extra1)
{
    linx_call_trace_emit(env, event, pc, extra0, extra1);
}

static inline bool linx_semihost_enabled_p(void)
{
    if (!linx_semihost_inited) {
        const char *v = getenv("LINX_SEMIHOST");
        linx_semihost_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_semihost_inited = true;
    }
    return linx_semihost_enabled;
}

static bool linx_reconstruct_ebreak_pc(CPULinxState *env, uint32_t imm,
                                       uint64_t *trap_pc_out)
{
    CPUState *cs = env_cpu(env);
    uint8_t buf[4];

    if (env->pc >= 4 &&
        cpu_memory_rw_debug(cs, env->pc - 4, buf, sizeof(buf), 0) == 0) {
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        if ((insn & ~UINT32_C(0x0f000000)) == UINT32_C(0x0010102b) &&
            ((insn >> 24) & 0xfu) == (imm & 0xfu)) {
            *trap_pc_out = env->pc - 4;
            return true;
        }
    }

    if (env->pc >= 2 &&
        cpu_memory_rw_debug(cs, env->pc - 2, buf, 2, 0) == 0) {
        const uint16_t insn = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        if ((insn & ~UINT16_C(0x07c0)) == UINT16_C(0xc02c) &&
            ((insn >> 6) & 0x1fu) == (imm & 0x1fu)) {
            *trap_pc_out = env->pc - 2;
            return true;
        }
    }

    return false;
}

static inline bool linx_debug_local_enabled_p(void)
{
    if (!linx_debug_local_inited) {
        const char *v = getenv("LINX_DEBUG_LOCAL");
        linx_debug_local_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_debug_local_inited = true;
    }
    return linx_debug_local_enabled;
}

static inline bool linx_debug_body_replay_enabled_p(void)
{
    if (!linx_debug_body_replay_inited) {
        const char *v = getenv("LINX_DEBUG_BODY_REPLAY");
        linx_debug_body_replay_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_debug_body_replay_inited = true;
    }
    return linx_debug_body_replay_enabled;
}

static inline bool linx_debug_acre_stderr_enabled_p(void)
{
    if (!linx_debug_acre_stderr_inited) {
        const char *v = getenv("LINX_DEBUG_ACRE_STDERR");
        linx_debug_acre_stderr_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_debug_acre_stderr_inited = true;
    }
    return linx_debug_acre_stderr_enabled;
}

static inline bool linx_debug_work_grab_enabled_p(void)
{
    if (!linx_debug_work_grab_inited) {
        const char *v = getenv("LINX_DEBUG_WORK_GRAB");
        linx_debug_work_grab_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_debug_work_grab_inited = true;
    }
    return linx_debug_work_grab_enabled;
}

static void linx_debug_pc_watch_init(void)
{
    const char *v;
    char *copy;
    char *saveptr = NULL;
    char *tok;

    if (linx_debug_pc_watch_inited) {
        return;
    }
    linx_debug_pc_watch_inited = true;

    const char *watch = getenv("LINX_DEBUG_PC_WATCH");
    if (!watch || !watch[0] || strcmp(watch, "0") == 0) {
        return;
    }

    v = getenv("LINX_DEBUG_PC_WATCH_COUNT_LO");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t count;
        if (linx_parse_u64(v, &count)) {
            linx_debug_pc_watch_count_lo = count;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_COUNT_HI");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t count;
        if (linx_parse_u64(v, &count)) {
            linx_debug_pc_watch_count_hi = count;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_HIT_LIMIT");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t limit;
        if (linx_parse_u64(v, &limit)) {
            linx_debug_pc_watch_hit_limit = limit;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_HIT_LO");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t hit;
        if (linx_parse_u64(v, &hit)) {
            linx_debug_pc_watch_hit_lo = hit;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_HIT_HI");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t hit;
        if (linx_parse_u64(v, &hit)) {
            linx_debug_pc_watch_hit_hi = hit;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_MATCH_MASK");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t mask;
        if (linx_parse_u64(v, &mask)) {
            linx_debug_pc_watch_match_mask = mask;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_MATCH_GPR");
    if (v && v[0] && strcmp(v, "0") != 0) {
        unsigned gpr;
        const char *value_s = getenv("LINX_DEBUG_PC_WATCH_MATCH_VALUE");
        uint64_t value;
        if (linx_parse_gpr_name(v, &gpr) &&
            value_s && value_s[0] &&
            linx_parse_u64(value_s, &value)) {
            linx_debug_pc_watch_match_source_enabled = true;
            linx_debug_pc_watch_match_kind = LINX_DEBUG_PC_WATCH_DUMP_GPR;
            linx_debug_pc_watch_match_index = gpr;
            linx_debug_pc_watch_match_name = linx_gpr_names[gpr];
            linx_debug_pc_watch_match_value = value;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_MATCH_REG");
    if (v && v[0] && strcmp(v, "0") != 0) {
        unsigned kind;
        unsigned index;
        const char *name;
        const char *value_s = getenv("LINX_DEBUG_PC_WATCH_MATCH_VALUE");
        uint64_t value;

        if (linx_debug_pc_watch_parse_source_copy(v, &kind, &index, &name) &&
            value_s && value_s[0] &&
            linx_parse_u64(value_s, &value)) {
            linx_debug_pc_watch_match_source_enabled = true;
            linx_debug_pc_watch_match_kind = kind;
            linx_debug_pc_watch_match_index = index;
            linx_debug_pc_watch_match_name = name;
            linx_debug_pc_watch_match_value = value;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_A0_WORDS");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t words;
        if (linx_parse_u64(v, &words) && words != 0) {
            linx_debug_pc_watch_dump_words = MIN((uint64_t)16, words);
            linx_debug_pc_watch_dump_kind = LINX_DEBUG_PC_WATCH_DUMP_GPR;
            linx_debug_pc_watch_dump_index = LINX_REG_A0;
            linx_debug_pc_watch_dump_name = "a0";
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_A0_OFFSET");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t offset;
        if (linx_parse_u64(v, &offset)) {
            linx_debug_pc_watch_dump_offset = offset;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_WORDS");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t words;
        if (linx_parse_u64(v, &words) && words != 0) {
            linx_debug_pc_watch_dump_words = MIN((uint64_t)16, words);
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_WIDTH");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t width;
        if (linx_parse_u64(v, &width) &&
            (width == 1 || width == 2 || width == 4 || width == 8)) {
            linx_debug_pc_watch_dump_width = width;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_OFFSET");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t offset;
        if (linx_parse_u64(v, &offset)) {
            linx_debug_pc_watch_dump_offset = offset;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_OFFSETS");
    if (v && v[0] && strcmp(v, "0") != 0) {
        linx_debug_pc_watch_parse_dump_offsets(v);
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_PTR_OFFSETS");
    if (v && v[0]) {
        linx_debug_pc_watch_parse_dump_ptr_offsets(v);
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_REG");
    if (v && v[0] && strcmp(v, "0") != 0) {
        linx_debug_pc_watch_parse_dump_source(v);
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_REGS");
    if (v && v[0] && strcmp(v, "0") != 0) {
        linx_debug_pc_watch_parse_dump_sources(v);
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_CODE_BYTES");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t bytes;
        if (linx_parse_u64(v, &bytes) && bytes != 0) {
            linx_debug_pc_watch_dump_code_bytes = MIN((uint64_t)32, bytes);
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_PHYS");
    if (v && v[0] && strcmp(v, "0") != 0) {
        linx_debug_pc_watch_dump_phys_bytes =
            linx_debug_pc_watch_dump_code_bytes ?
            linx_debug_pc_watch_dump_code_bytes : 16;
    }

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_PHYS_BYTES");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t bytes;
        if (linx_parse_u64(v, &bytes) && bytes != 0) {
            linx_debug_pc_watch_dump_phys_bytes = MIN((uint64_t)32, bytes);
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_EXIT");
    linx_debug_pc_watch_exit = v && v[0] && strcmp(v, "0") != 0;

    v = getenv("LINX_DEBUG_PC_WATCH_DUMP_CALL_RING");
    linx_debug_pc_watch_dump_call_ring =
        v && v[0] && strcmp(v, "0") != 0;

    v = getenv("LINX_DEBUG_PC_WATCH_PRINT");
    if (v && v[0] && strcmp(v, "0") == 0) {
        linx_debug_pc_watch_print_enabled = false;
    }

    v = getenv("LINX_DEBUG_PC_WATCH_RING");
    linx_debug_pc_watch_ring_enabled =
        v && v[0] && strcmp(v, "0") != 0;
    if (linx_debug_pc_watch_ring_enabled) {
        uint64_t size = 0;
        const char *size_s = getenv("LINX_DEBUG_PC_WATCH_RING_SIZE");

        linx_debug_pc_watch_ring_size = 64;
        if (size_s && size_s[0] && strcmp(size_s, "0") != 0 &&
            linx_parse_u64(size_s, &size)) {
            linx_debug_pc_watch_ring_size =
                MIN(size, (uint64_t)LINX_DEBUG_PC_WATCH_RING_MAX);
            linx_debug_pc_watch_ring_size =
                MAX(linx_debug_pc_watch_ring_size, 1);
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_RING_MEM_REG");
    if (v && v[0] && strcmp(v, "0") != 0) {
        unsigned kind;
        unsigned index;
        const char *name;

        if (linx_debug_pc_watch_parse_source_copy(v, &kind, &index, &name)) {
            linx_debug_pc_watch_ring_mem_enabled = true;
            linx_debug_pc_watch_ring_mem_kind = kind;
            linx_debug_pc_watch_ring_mem_index = index;
            linx_debug_pc_watch_ring_mem_name = name;
        }
    }

    v = getenv("LINX_DEBUG_PC_WATCH_RING_MEM_OFFSET");
    if (v && v[0] && strcmp(v, "0") != 0) {
        uint64_t offset;
        if (linx_parse_u64(v, &offset)) {
            linx_debug_pc_watch_ring_mem_offset = offset;
        }
    }

    linx_debug_pc_watch_regs_enabled =
        linx_env_enabled("LINX_DEBUG_PC_WATCH_REGS") ||
        linx_env_enabled("LINX_TRACE_REGS");

    copy = g_strdup(watch);
    for (tok = strtok_r(copy, ",", &saveptr);
         tok && linx_debug_pc_watch_count < ARRAY_SIZE(linx_debug_pc_watch);
         tok = strtok_r(NULL, ",", &saveptr)) {
        uint64_t pc;
        if (linx_parse_u64(tok, &pc)) {
            linx_debug_pc_watch[linx_debug_pc_watch_count++] = pc;
        }
    }
    g_free(copy);
}

static void linx_debug_pc_watch_ring_record(CPULinxState *env,
                                            unsigned watch_index,
                                            uint64_t pc, uint64_t hit)
{
    LinxDebugPcWatchRingEntry *entry;

    if (!linx_debug_pc_watch_ring_enabled) {
        return;
    }

    entry = &linx_debug_pc_watch_ring[linx_debug_pc_watch_ring_next];
    *entry = (LinxDebugPcWatchRingEntry) {
        .watch_index = watch_index,
        .acr = env->acr & 0xFu,
        .cond = env->cond,
        .carg = env->carg,
        .brtype = env->brtype,
        .in_body = env->in_body,
        .blocktype = env->blocktype,
        .call_ra_set = env->call_ra_set,
        .call_setret_pending = env->call_setret_pending,
        .pc = pc,
        .hit = hit,
        .printed = linx_debug_pc_watch_printed[watch_index],
        .count = env->insn_count,
        .envpc = env->pc,
        .bpc = env->bpc,
        .tpc = env->body_tpc,
        .cstate = env->ssr[0x20],
        .tgt = env->tgt,
        .body_tpc = env->body_tpc,
        .return_pc = env->return_pc,
        .tp = env->ssr[0],
    };
    memcpy(entry->gpr, env->gpr, sizeof(entry->gpr));
    memcpy(entry->tq, env->tq, sizeof(entry->tq));
    memcpy(entry->uq, env->uq, sizeof(entry->uq));
    if (linx_debug_pc_watch_ring_mem_enabled) {
        entry->mem_kind = linx_debug_pc_watch_ring_mem_kind;
        entry->mem_index = linx_debug_pc_watch_ring_mem_index;
        entry->mem_base = linx_debug_pc_watch_dump_addr_for(
            env, linx_debug_pc_watch_ring_mem_kind,
            linx_debug_pc_watch_ring_mem_index);
        entry->mem_addr =
            entry->mem_base + linx_debug_pc_watch_ring_mem_offset;
        entry->mem_ok =
            entry->mem_base &&
            linx_debug_read_guest_u64(env, entry->mem_addr,
                                      &entry->mem_value);
    }

    linx_debug_pc_watch_ring_next =
        (linx_debug_pc_watch_ring_next + 1) %
        linx_debug_pc_watch_ring_size;
    if (linx_debug_pc_watch_ring_count < linx_debug_pc_watch_ring_size) {
        linx_debug_pc_watch_ring_count++;
    }
}

void linx_debug_pc_watch_dump_recent(CPULinxState *env, const char *reason,
                                     uint64_t fault_pc)
{
    uint64_t entries;
    uint64_t start;

    linx_debug_pc_watch_init();
    if (!linx_debug_pc_watch_ring_enabled ||
        linx_debug_pc_watch_ring_count == 0) {
        return;
    }

    entries = linx_debug_pc_watch_ring_count;
    start = (linx_debug_pc_watch_ring_next +
             linx_debug_pc_watch_ring_size - entries) %
            linx_debug_pc_watch_ring_size;
    fprintf(stderr,
            "LINX_PC_WATCH_RING reason=%s fault_pc=0x%" PRIx64
            " fault_count=%" PRIu64 " entries=%" PRIu64 "\n",
            reason ? reason : "unknown", fault_pc, env->insn_count,
            entries);

    for (uint64_t i = 0; i < entries; i++) {
        const LinxDebugPcWatchRingEntry *entry =
            &linx_debug_pc_watch_ring[(start + i) %
                                      linx_debug_pc_watch_ring_size];

        fprintf(stderr,
                "LINX_PC_WATCH_RING_ENTRY idx=%" PRIu64
                " age=%" PRIu64 " watch=%u pc=0x%" PRIx64
                " hit=%" PRIu64 " printed=%" PRIu64
                " count=%" PRIu64 " envpc=0x%" PRIx64
                " bpc=0x%" PRIx64 " tpc=0x%" PRIx64
                " acr=%u cstate=0x%" PRIx64
                " cond=%u carg=%u brtype=%u tgt=0x%" PRIx64
                " tp=0x%" PRIx64
                " sp=0x%" PRIx64 " ra=0x%" PRIx64
                " a0=0x%" PRIx64 " a1=0x%" PRIx64
                " a2=0x%" PRIx64 " a3=0x%" PRIx64
                " a4=0x%" PRIx64 " a5=0x%" PRIx64
                " a6=0x%" PRIx64 " a7=0x%" PRIx64
                " x0=0x%" PRIx64 " x1=0x%" PRIx64
                " x2=0x%" PRIx64 " x3=0x%" PRIx64
                " s0=0x%" PRIx64 " s1=0x%" PRIx64
                " s2=0x%" PRIx64 " s3=0x%" PRIx64
                " s4=0x%" PRIx64 " s5=0x%" PRIx64
                " s6=0x%" PRIx64 " s7=0x%" PRIx64
                " s8=0x%" PRIx64
                " tq0=0x%" PRIx64 " tq1=0x%" PRIx64
                " uq0=0x%" PRIx64 " uq1=0x%" PRIx64
                " in_body=%u blocktype=%u body_tpc=0x%" PRIx64
                " return_pc=0x%" PRIx64
                " call_ra_set=%u call_setret_pending=%u",
                i, entries - i - 1, entry->watch_index, entry->pc,
                entry->hit, entry->printed, entry->count, entry->envpc,
                entry->bpc, entry->tpc, entry->acr, entry->cstate,
                entry->cond, entry->carg, entry->brtype, entry->tgt,
                entry->tp, entry->gpr[LINX_REG_SP],
                entry->gpr[LINX_REG_RA], entry->gpr[LINX_REG_A0],
                entry->gpr[LINX_REG_A1], entry->gpr[LINX_REG_A2],
                entry->gpr[LINX_REG_A3], entry->gpr[LINX_REG_A4],
                entry->gpr[LINX_REG_A5], entry->gpr[LINX_REG_A6],
                entry->gpr[LINX_REG_A7], entry->gpr[LINX_REG_X0],
                entry->gpr[LINX_REG_X1], entry->gpr[LINX_REG_X2],
                entry->gpr[LINX_REG_X3], entry->gpr[LINX_REG_S0],
                entry->gpr[LINX_REG_S1], entry->gpr[LINX_REG_S2],
                entry->gpr[LINX_REG_S3], entry->gpr[LINX_REG_S4],
                entry->gpr[LINX_REG_S5], entry->gpr[LINX_REG_S6],
                entry->gpr[LINX_REG_S7], entry->gpr[LINX_REG_S8],
                entry->tq[0], entry->tq[1], entry->uq[0], entry->uq[1],
                entry->in_body, entry->blocktype, entry->body_tpc,
                entry->return_pc, entry->call_ra_set,
                entry->call_setret_pending);
        if (linx_debug_pc_watch_ring_mem_enabled) {
            fprintf(stderr,
                    " mem_src=%s mem_kind=%u mem_index=%u"
                    " mem_offset=0x%" PRIx64
                    " mem_base=0x%" PRIx64 " mem_addr=0x%" PRIx64
                    " mem_ok=%u mem_value=0x%" PRIx64,
                    linx_debug_pc_watch_ring_mem_name ?
                        linx_debug_pc_watch_ring_mem_name : "unknown",
                    entry->mem_kind, entry->mem_index,
                    linx_debug_pc_watch_ring_mem_offset,
                    entry->mem_base, entry->mem_addr, entry->mem_ok,
                    entry->mem_value);
        }
        fputc('\n', stderr);
    }
    fflush(stderr);
}

static void linx_debug_pc_watch_probe(CPULinxState *env, uint64_t pc)
{
    unsigned i;
    linx_debug_pc_watch_init();
    if (!linx_debug_pc_watch_count ||
        env->insn_count < linx_debug_pc_watch_count_lo ||
        env->insn_count > linx_debug_pc_watch_count_hi) {
        return;
    }

    const uint64_t tp = env->ssr[0];
    const uint64_t sp = env->gpr[LINX_REG_SP];
    for (i = 0; i < linx_debug_pc_watch_count; i++) {
        if (linx_debug_pc_watch[i] != pc) {
            continue;
        }
        linx_debug_pc_watch_hits[i]++;
        const uint64_t hit = linx_debug_pc_watch_hits[i];
        if (hit < linx_debug_pc_watch_hit_lo ||
            hit > linx_debug_pc_watch_hit_hi) {
            continue;
        }
        if (linx_debug_pc_watch_match_source_enabled) {
            const uint64_t actual = linx_debug_pc_watch_dump_addr_for(
                env, linx_debug_pc_watch_match_kind,
                linx_debug_pc_watch_match_index);
            if ((actual & linx_debug_pc_watch_match_mask) !=
                (linx_debug_pc_watch_match_value &
                 linx_debug_pc_watch_match_mask)) {
                continue;
            }
        }
        linx_debug_pc_watch_ring_record(env, i, pc, hit);
        if (linx_debug_pc_watch_hit_limit &&
            linx_debug_pc_watch_printed[i] >= linx_debug_pc_watch_hit_limit) {
            continue;
        }
        if (!linx_debug_pc_watch_print_enabled) {
            continue;
        }
        linx_debug_pc_watch_printed[i]++;
        fprintf(stderr,
                "linx_pc_watch: pc=0x%" PRIx64
                " hit=%" PRIu64 " printed=%" PRIu64
                " count=%" PRIu64 " sp=0x%" PRIx64
                " a0=0x%" PRIx64 " a1=0x%" PRIx64 " a2=0x%" PRIx64
                " ra=0x%" PRIx64 " tp=0x%" PRIx64 " cstate=0x%" PRIx64
                " cond=%u carg=%u brtype=%u tgt=0x%" PRIx64
                " bpc=0x%" PRIx64 " tq0=0x%" PRIx64 " tq1=0x%" PRIx64
                " uq0=0x%" PRIx64 " uq1=0x%" PRIx64
                " in_body=%u blocktype=%u body_tpc=0x%" PRIx64
                " return_pc=0x%" PRIx64 " call_ra_set=%u call_setret_pending=%u",
                pc, linx_debug_pc_watch_hits[i],
                linx_debug_pc_watch_printed[i], env->insn_count,
                env->gpr[LINX_REG_SP],
                env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1],
                env->gpr[LINX_REG_A2], env->gpr[LINX_REG_RA], tp, env->ssr[0x20],
                env->cond, env->carg, env->brtype, env->tgt, env->bpc,
                env->tq[0], env->tq[1], env->uq[0], env->uq[1],
                env->in_body, env->blocktype, env->body_tpc,
                env->return_pc, env->call_ra_set, env->call_setret_pending);
        if (linx_debug_pc_watch_match_source_enabled) {
            const char *name = linx_debug_pc_watch_match_name ?
                linx_debug_pc_watch_match_name : "unknown";
            fprintf(stderr,
                    " match_src=%s match_kind=%u match_index=%u"
                    " match_value=0x%" PRIx64 " match_mask=0x%" PRIx64,
                    name, linx_debug_pc_watch_match_kind,
                    linx_debug_pc_watch_match_index,
                    linx_debug_pc_watch_match_value,
                    linx_debug_pc_watch_match_mask);
        }
        fputc('\n', stderr);
        if (linx_debug_pc_watch_regs_enabled) {
            fprintf(stderr,
                    "LINX_PC_WATCH_REGS pc=0x%" PRIx64
                    " hit=%" PRIu64
                    " count=%" PRIu64 " bpc=0x%" PRIx64
                    " tpc=0x%" PRIx64,
                    pc, linx_debug_pc_watch_hits[i], env->insn_count,
                    env->bpc, env->body_tpc);
            linx_fprint_gprs(stderr, env);
            fputc('\n', stderr);
        }
        if (linx_debug_pc_watch_dump_code_bytes) {
            fprintf(stderr,
                    "LINX_PC_WATCH_CODE hit=%" PRIu64,
                    linx_debug_pc_watch_hits[i]);
            linx_fprint_guest_code_bytes(stderr, env, "pc", pc,
                                         linx_debug_pc_watch_dump_code_bytes);
            fputc('\n', stderr);
        }
        if (linx_debug_pc_watch_dump_phys_bytes) {
            fprintf(stderr,
                    "LINX_PC_WATCH_PHYS hit=%" PRIu64,
                    linx_debug_pc_watch_hits[i]);
            linx_fprint_guest_phys_bytes(stderr, env, "pc", pc,
                                         linx_debug_pc_watch_dump_phys_bytes);
            fputc('\n', stderr);
        }
        if (tp) {
            linx_debug_dump_guest_words(env, tp, 4, "  tp");
        }
        if (linx_debug_pc_watch_dump_words) {
            if (linx_debug_pc_watch_dump_source_count) {
                for (unsigned j = 0;
                     j < linx_debug_pc_watch_dump_source_count;
                     j++) {
                    linx_debug_pc_watch_dump_words_for_source_offsets(
                        env,
                        linx_debug_pc_watch_dump_source_kinds[j],
                        linx_debug_pc_watch_dump_source_indexes[j],
                        linx_debug_pc_watch_dump_source_names[j]);
                }
            } else {
                linx_debug_pc_watch_dump_words_for_source_offsets(
                    env, linx_debug_pc_watch_dump_kind,
                    linx_debug_pc_watch_dump_index,
                    linx_debug_pc_watch_dump_name);
            }
        }
        if (linx_debug_pc_watch_dump_call_ring) {
            linx_call_trace_dump_recent(env, "pc_watch", pc);
        }
        if (pc == UINT64_C(0xffffffff80007bf8) ||
            pc == UINT64_C(0xffffffff80007bac)) {
            linx_debug_dump_guest_words(env, sp + 280, 5, "  pt_tail");
        }
        fflush(stderr);
        if (linx_debug_pc_watch_exit) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            cpu_loop_exit_noexc(env_cpu(env));
        }
    }
}

static void linx_debug_dump_guest_units(CPULinxState *env, uint64_t addr,
                                        unsigned count, const char *label,
                                        unsigned width)
{
    CPUState *cs = env_cpu(env);
    unsigned i;

    if (width != 1 && width != 2 && width != 4 && width != 8) {
        width = 8;
    }

    fprintf(stderr, "%s @0x%" PRIx64, label, addr);
    if (width != 8) {
        fprintf(stderr, " width=%u", width);
    }
    for (i = 0; i < count; i++) {
        uint64_t value = 0;
        uint64_t cur = addr + (uint64_t)i * width;
        if (cpu_memory_rw_debug(cs, cur, (uint8_t *)&value, width, 0) != 0) {
            if ((cur >> 48) == 0xff60u || (cur >> 48) == 0xff80u ||
                (cur >> 48) == 0xffffu) {
                const uint64_t low_alias = cur & UINT64_C(0x7fffffff);
                if (cpu_memory_rw_debug(cs, low_alias, (uint8_t *)&value,
                                        width, 0) == 0) {
                    switch (width) {
                    case 1:
                        fprintf(stderr, " [%" PRIu32 "]=0x%02" PRIx64 "*",
                                i, value);
                        break;
                    case 2:
                        fprintf(stderr, " [%" PRIu32 "]=0x%04" PRIx64 "*",
                                i, value);
                        break;
                    case 4:
                        fprintf(stderr, " [%" PRIu32 "]=0x%08" PRIx64 "*",
                                i, value);
                        break;
                    default:
                        fprintf(stderr, " [%" PRIu32 "]=0x%016" PRIx64 "*",
                                i, value);
                        break;
                    }
                    continue;
                }
            }
            fprintf(stderr, " [%" PRIu32 "]=<fault>", i);
            break;
        }
        switch (width) {
        case 1:
            fprintf(stderr, " [%" PRIu32 "]=0x%02" PRIx64, i, value);
            break;
        case 2:
            fprintf(stderr, " [%" PRIu32 "]=0x%04" PRIx64, i, value);
            break;
        case 4:
            fprintf(stderr, " [%" PRIu32 "]=0x%08" PRIx64, i, value);
            break;
        default:
            fprintf(stderr, " [%" PRIu32 "]=0x%016" PRIx64, i, value);
            break;
        }
    }
    fprintf(stderr, "\n");
}

static void linx_debug_dump_guest_words(CPULinxState *env, uint64_t addr,
                                        unsigned count, const char *label)
{
    linx_debug_dump_guest_units(env, addr, count, label, 8);
}

static void linx_debug_work_grab_probe(CPULinxState *env, uint64_t pc)
{
    /*
     * work_grab_pending()/timer_delete loop observed in Linux bring-up:
     * - work_grab_pending: 0xffffffff80031552, 0xffffffff80031592,
     *   0xffffffff8003159c, 0xffffffff800315c6
     * - __timer_delete: 0xffffffff8008cc18
     */
    if (!linx_debug_work_grab_enabled_p() || linx_debug_work_grab_emits >= 64) {
        return;
    }

    switch (pc) {
    case UINT64_C(0xffffffff80031552):
    case UINT64_C(0xffffffff80031592):
    case UINT64_C(0xffffffff8003159c):
    case UINT64_C(0xffffffff800315c6): {
        const uint64_t work = env->gpr[11];
        const uint64_t irq_flags = env->gpr[12];
        const uint64_t timer = env->gpr[13];
        fprintf(stderr,
                "linx_work_grab: pc=0x%" PRIx64
                " work=0x%" PRIx64 " irq_flags=0x%" PRIx64
                " timer=0x%" PRIx64 " a0=0x%" PRIx64
                " a1=0x%" PRIx64 " a2=0x%" PRIx64
                " ra=0x%" PRIx64 " cstate=0x%" PRIx64 "\n",
                pc, work, irq_flags, timer,
                env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1],
                env->gpr[LINX_REG_A2], env->gpr[LINX_REG_RA],
                env->ssr[0x20]);
        linx_debug_dump_guest_words(env, work, 6, "  work");
        linx_debug_dump_guest_words(env, irq_flags, 1, "  irq_flags");
        linx_debug_dump_guest_words(env, timer, 6, "  timer");
        linx_debug_work_grab_emits++;
        break;
    }
    case UINT64_C(0xffffffff8008cc18): {
        const uint64_t timer = env->gpr[LINX_REG_A0];
        fprintf(stderr,
                "linx_timer_delete: pc=0x%" PRIx64
                " timer=0x%" PRIx64 " shutdown=0x%" PRIx64
                " ra=0x%" PRIx64 " cstate=0x%" PRIx64 "\n",
                pc, timer, env->gpr[LINX_REG_A1],
                env->gpr[LINX_REG_RA], env->ssr[0x20]);
        linx_debug_dump_guest_words(env, timer, 6, "  timer");
        linx_debug_work_grab_emits++;
        break;
    }
    default:
        break;
    }
}

const LinxOpcodeMeta *linx_opcode_meta_lookup(uint64_t insn_word, unsigned insn_len)
{
    const LinxOpcodeMeta *best = NULL;
    int best_bits = -1;
    unsigned i;

    for (i = 0; i < linx_opcode_meta_table_count; i++) {
        const LinxOpcodeMeta *m = &linx_opcode_meta_table[i];
        int bits;

        if (m->insn_len != 0 && insn_len != 0 && m->insn_len != insn_len) {
            continue;
        }
        if ((insn_word & m->mask) != m->match) {
            continue;
        }
        bits = __builtin_popcountll(m->mask);
        if (bits > best_bits) {
            best = m;
            best_bits = bits;
        }
    }
    return best;
}

/* Semihosting operations via EBREAK immediate */
#define LINX_SEMIHOST_EXIT      0  /* Exit program */
#define LINX_SEMIHOST_PUTCHAR   1  /* a0 = character to output */
#define LINX_SEMIHOST_WRITE     2  /* a0 = fd, a1 = buf, a2 = len -> a0 = bytes written */
#define LINX_SEMIHOST_READ      3  /* a0 = fd, a1 = buf, a2 = len -> a0 = bytes read */

/* ------------------------------------------------------------------------- */
/* System Status Register (SSR) helpers                                      */
/* ------------------------------------------------------------------------- */

/* SSR IDs (bring-up subset; see `isa.txt`). */
enum {
    LINX_SSR_CW    = 0x0820,
    LINX_SSR_TP    = 0x0000,
    LINX_SSR_GP    = 0x0001,
    LINX_SSR_TIME  = 0x0010,
    LINX_SSR_CYCLE = 0x0c00,
    LINX_SSR_CSTATE = 0x0020,
};

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
    LINX_SSR_TTBR0    = 0xF10,
    LINX_SSR_TTBR1    = 0xF11,
    LINX_SSR_TCR      = 0xF12,
    LINX_SSR_MAIR     = 0xF13,
    LINX_SSR_IOTTBR   = 0xF14,
    LINX_SSR_IOTCR    = 0xF15,
    LINX_SSR_IOMAIR   = 0xF16,
    LINX_SSR_TIMER_TIME   = 0xF20,
    LINX_SSR_TIMER_TIMECMP = 0xF21,

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

enum {
    LINX_IRQ_TIMER0 = 4,
};

static void linx_tp_trace_init(void)
{
    if (linx_tp_trace_inited) {
        return;
    }

    linx_tp_trace_enabled = linx_env_enabled("LINX_TP_TRACE");
    linx_tp_trace_ssr_enabled = linx_env_enabled("LINX_TP_TRACE_SSR");
    linx_tp_trace_reads_enabled = linx_env_enabled("LINX_TP_TRACE_READS");

    const char *limit_s = getenv("LINX_TP_TRACE_LIMIT");
    if (limit_s) {
        (void)linx_parse_u64(limit_s, &linx_tp_trace_limit);
    }

    linx_tp_trace_inited = true;
}

static bool linx_tp_trace_enabled_p(void)
{
    linx_tp_trace_init();
    if (!linx_tp_trace_enabled) {
        return false;
    }
    if (linx_tp_trace_limit != 0 &&
        linx_tp_trace_emitted >= linx_tp_trace_limit) {
        return false;
    }
    return true;
}

static bool linx_tp_trace_interesting_idx(uint32_t idx)
{
    return idx == LINX_SSR_TP ||
           idx == LINX_SSR_ETEMP ||
           idx == LINX_SSR_ETEMP0;
}

static const char *linx_tp_trace_ssr_name(uint32_t idx)
{
    switch (idx) {
    case LINX_SSR_TP:
        return "TP";
    case LINX_SSR_ETEMP:
        return "ETEMP";
    case LINX_SSR_ETEMP0:
        return "ETEMP0";
    default:
        return "unknown";
    }
}

static void linx_tp_trace_emit(CPULinxState *env, const char *event,
                               uint32_t ssrid, uint32_t bank,
                               uint64_t old_value, uint64_t new_value)
{
    const uint32_t idx = linx_ssr_low12(ssrid);

    if (!linx_tp_trace_interesting_idx(idx) || !linx_tp_trace_enabled_p()) {
        return;
    }
    if (!linx_tp_trace_ssr_enabled && strcmp(event, "ssr_read") != 0) {
        return;
    }

    linx_tp_trace_emitted++;
    fprintf(stderr,
            "LINX_TP_TRACE event=%s seq=%" PRIu64
            " count=%" PRIu64
            " pc=0x%" PRIx64 " bpc=0x%" PRIx64 " tpc=0x%" PRIx64
            " envpc=0x%" PRIx64 " acr=%u cstate=0x%" PRIx64
            " ssrid=0x%x idx=0x%x name=%s bank=%u"
            " old=0x%" PRIx64 " new=0x%" PRIx64
            " tp=0x%" PRIx64 " etemp1=0x%" PRIx64
            " etemp0_1=0x%" PRIx64
            " sp=0x%" PRIx64 " ra=0x%" PRIx64
            " a0=0x%" PRIx64 " a1=0x%" PRIx64 "\n",
            event, linx_tp_trace_emitted,
            env->insn_count, env->pc, env->bpc, env->body_tpc,
            env->pc, env->acr & 0xFu, env->ssr[LINX_SSR_CSTATE],
            ssrid, idx, linx_tp_trace_ssr_name(idx), bank,
            old_value, new_value, env->ssr[LINX_SSR_TP],
            env->ssr_acr[1][LINX_SSR_ETEMP],
            env->ssr_acr[1][LINX_SSR_ETEMP0],
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1]);
    fflush(stderr);
}

static void linx_tp_trace_emit_handoff(CPULinxState *env, const char *event,
                                       uint32_t src_acr, uint32_t dst_acr,
                                       uint64_t user_tp,
                                       uint64_t thread_info)
{
    if (!linx_tp_trace_enabled_p()) {
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

#define LINX_LEGACY_MMCONFIG_MODE_MASK  UINT64_C(0x3)
#define LINX_LEGACY_MMCONFIG_Q_BIT      (UINT64_C(1) << 7)
#define LINX_LEGACY_MMCONFIG_ENABLE_BIT (UINT64_C(1) << 63)

/* ECSTATE bits (v0.2 bring-up profile; mirrors key CSTATE fields). */
#define LINX_ECSTATE_BI_BIT        (1ULL << 62)
#define LINX_TRAPNUM_BREAKPOINT_EXP 17u

/* TRAPNO encoding (v0.2 bring-up profile; keep in sync with target/linx/cpu.c). */
#define LINX_TRAPNO_E_BIT          (1ULL << 63) /* 1=exception, 0=interrupt */
#define LINX_TRAPNO_ARGV_BIT       (1ULL << 62)
#define LINX_TRAPNO_CAUSE_SHIFT    24u
#define LINX_TRAPNO_CAUSE_MASK     0xFFFFFFu
#define LINX_TRAPNO_TRAPNUM_MASK   0x3Fu

static inline uint64_t linx_trapno_make(bool exception, bool argv,
                                        uint32_t cause, uint8_t trapnum)
{
    const uint64_t e = exception ? LINX_TRAPNO_E_BIT : 0;
    const uint64_t a = argv ? LINX_TRAPNO_ARGV_BIT : 0;
    const uint64_t c = ((uint64_t)(cause & LINX_TRAPNO_CAUSE_MASK)) << LINX_TRAPNO_CAUSE_SHIFT;
    const uint64_t t = (uint64_t)(trapnum & LINX_TRAPNO_TRAPNUM_MASK);
    return e | a | c | t;
}

typedef struct LinxCosimSnapshotHeader {
    char magic[8];
    uint32_t version;
    uint32_t range_count;
} LinxCosimSnapshotHeader;

typedef struct LinxCosimSnapshotRange {
    uint64_t base;
    uint64_t size;
    uint64_t file_offset;
} LinxCosimSnapshotRange;

static inline bool linx_env_enabled(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

static inline bool linx_cfi_trace_enabled_p(void)
{
    if (!linx_cfi_trace_inited) {
        linx_cfi_trace_enabled = linx_env_enabled("LINX_CFI_TRACE");
        linx_cfi_trace_inited = true;
    }
    return linx_cfi_trace_enabled;
}

static inline bool linx_bstart_cache_revalidate_enabled_p(void)
{
    if (!linx_bstart_cache_revalidate_inited) {
        linx_bstart_cache_revalidate_enabled =
            linx_env_enabled("LINX_BSTART_CACHE_REVALIDATE");
        linx_bstart_cache_revalidate_inited = true;
    }
    return linx_bstart_cache_revalidate_enabled;
}

static inline bool linx_bstart_cache_stats_enabled_p(void)
{
    if (!linx_bstart_cache_stats_inited) {
        uint64_t interval = 0;

        linx_bstart_cache_stats_enabled =
            linx_env_enabled("LINX_BSTART_CACHE_STATS");
        if (linx_bstart_cache_stats_enabled) {
            const char *interval_s = getenv("LINX_BSTART_CACHE_STATS_INTERVAL");
            if (interval_s && linx_parse_u64(interval_s, &interval)) {
                linx_bstart_cache_stats_interval = interval;
            } else {
                linx_bstart_cache_stats_interval = 1000000u;
            }
        }
        linx_bstart_cache_stats_inited = true;
    }
    return linx_bstart_cache_stats_enabled;
}

static inline QEMU_ALWAYS_INLINE bool
linx_bstart_cache_fast_hit_available(void)
{
    if (unlikely(!linx_cfi_trace_inited || linx_cfi_trace_enabled ||
                 !linx_bstart_cache_revalidate_inited ||
                 linx_bstart_cache_revalidate_enabled ||
                 !linx_bstart_cache_stats_inited ||
                 linx_bstart_cache_stats_enabled)) {
        return false;
    }
    return true;
}

static inline QEMU_ALWAYS_INLINE bool
linx_bstart_cache_fast_hit(CPULinxState *env, uint64_t target)
{
    const size_t slot = linx_bstart_cache_slot(target);
    const uint8_t mmu_idx = (uint8_t)linx_env_mmu_index(env);

    return linx_bstart_cache_fast_hit_available() &&
           env->bstart_cache_valid[slot] &&
           env->bstart_cache_tag[slot] == target &&
           env->bstart_cache_mmu_idx[slot] == mmu_idx;
}

static inline void linx_bstart_cache_stats_emit_maybe(CPULinxState *env)
{
    if (!linx_bstart_cache_stats_interval ||
        (linx_bstart_cache_stat_checks % linx_bstart_cache_stats_interval) != 0) {
        return;
    }

    fprintf(stderr,
            "LINX_BSTART_CACHE_STATS count=%" PRIu64
            " pc=0x%" PRIx64 " bpc=0x%" PRIx64 " tpc=0x%" PRIx64
            " checks=%" PRIu64 " hits=%" PRIu64
            " revalidations=%" PRIu64 " continuations=%" PRIu64
            " fallthroughs=%" PRIu64 " bstarts=%" PRIu64
            " defers=%" PRIu64 " bad=%" PRIu64
            " inserts=%" PRIu64
            " resets=%" PRIu64 " page_resets=%" PRIu64
            " page_reset_entries=%" PRIu64 " size=%u\n",
            env->insn_count, env->pc, env->bpc, env->body_tpc,
            linx_bstart_cache_stat_checks, linx_bstart_cache_stat_hits,
            linx_bstart_cache_stat_revalidations,
            linx_bstart_cache_stat_continuations,
            linx_bstart_cache_stat_fallthroughs,
            linx_bstart_cache_stat_bstarts,
            linx_bstart_cache_stat_defers,
            linx_bstart_cache_stat_bad,
            linx_bstart_cache_stat_inserts,
            linx_bstart_cache_stat_resets,
            linx_bstart_cache_stat_page_resets,
            linx_bstart_cache_stat_page_reset_entries,
            (unsigned)LINX_BSTART_CACHE_SIZE);
    fflush(stderr);
}

static inline bool linx_ssr_idx_is_debug(uint32_t idx)
{
    return ((idx >= 0xF90u && idx <= 0xF97u) || /* DBCR/DBVR[0..3] */
            (idx >= 0xFA0u && idx <= 0xFA1u) || /* DCCR/DCVR[0] */
            (idx >= 0xFB0u && idx <= 0xFB7u));  /* DWCR/DWVR[0..3] */
}

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

static inline void linx_refresh_tb_cosim_precheck(CPULinxState *env)
{
    env->tb_cosim_precheck =
        (env->cosim.enabled && !env->cosim.active && !env->cosim.ended) ? 1 : 0;
}

static bool linx_parse_u64(const char *s, uint64_t *out)
{
    char *endp = NULL;

    if (!s || !s[0]) {
        return false;
    }
    errno = 0;
    *out = strtoull(s, &endp, 0);
    return errno == 0 && endp && endp != s && *endp == '\0';
}

static void linx_cosim_close_socket(CPULinxState *env)
{
    if (env->cosim.sock_fd >= 0) {
        close(env->cosim.sock_fd);
        env->cosim.sock_fd = -1;
    }
}

static void linx_cosim_finish(CPULinxState *env)
{
    env->cosim.active = 0;
    env->cosim.ended = 1;
    linx_cosim_close_socket(env);
    linx_refresh_tb_cosim_precheck(env);
}

static bool linx_cosim_parse_ranges(CPULinxState *env, const char *ranges_s)
{
    char *cursor;
    char *saveptr = NULL;
    char *copy;

    env->cosim.range_count = 0;
    if (!ranges_s || !ranges_s[0]) {
        return false;
    }

    copy = g_strdup(ranges_s);
    if (!copy) {
        return false;
    }

    for (cursor = strtok_r(copy, ",", &saveptr);
         cursor;
         cursor = strtok_r(NULL, ",", &saveptr)) {
        char *sep = strchr(cursor, ':');
        uint64_t base;
        uint64_t size;

        if (!sep) {
            g_free(copy);
            return false;
        }
        *sep = '\0';
        if (!linx_parse_u64(cursor, &base) || !linx_parse_u64(sep + 1, &size) || size == 0) {
            g_free(copy);
            return false;
        }
        if (env->cosim.range_count >= LINX_COSIM_MAX_RANGES) {
            g_free(copy);
            return false;
        }
        env->cosim.ranges[env->cosim.range_count].base = base;
        env->cosim.ranges[env->cosim.range_count].size = size;
        env->cosim.range_count++;
    }

    g_free(copy);
    return env->cosim.range_count > 0;
}

static bool linx_cosim_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += (size_t)n;
    }

    return true;
}

static bool linx_cosim_send_line(CPULinxState *env, const char *line)
{
    const size_t len = strlen(line);
    return linx_cosim_write_all(env->cosim.sock_fd, line, len) &&
           linx_cosim_write_all(env->cosim.sock_fd, "\n", 1);
}

static bool linx_cosim_recv_line(CPULinxState *env, char *out, size_t out_sz)
{
    size_t off = 0;

    if (out_sz == 0) {
        return false;
    }
    while (off + 1 < out_sz) {
        char ch = '\0';
        ssize_t n = recv(env->cosim.sock_fd, &ch, 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        if (ch == '\n') {
            break;
        }
        out[off++] = ch;
    }
    out[off] = '\0';
    return true;
}

static bool linx_cosim_connect(CPULinxState *env)
{
    struct sockaddr_un addr = { 0 };
    int fd;

    if (env->cosim.sock_fd >= 0) {
        return true;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "Linx cosim: socket() failed: %s\n", strerror(errno));
        return false;
    }
    addr.sun_family = AF_UNIX;
    if (strlen(env->cosim.socket_path) >= sizeof(addr.sun_path)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx cosim: socket path too long: %s\n",
                      env->cosim.socket_path);
        close(fd);
        return false;
    }
    strcpy(addr.sun_path, env->cosim.socket_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx cosim: connect(%s) failed: %s\n",
                      env->cosim.socket_path, strerror(errno));
        close(fd);
        return false;
    }
    env->cosim.sock_fd = fd;
    return true;
}

static bool linx_cosim_dump_snapshot(CPULinxState *env)
{
    const LinxCosimSnapshotHeader hdr = {
        .magic = { 'L', 'X', 'C', 'O', 'S', 'I', 'M', '1' },
        .version = 1u,
        .range_count = env->cosim.range_count,
    };
    LinxCosimSnapshotRange *table = NULL;
    CPUState *cs = env_cpu(env);
    FILE *fp = NULL;
    uint64_t payload_off;
    uint32_t i;
    bool ok = false;

    fp = fopen(env->cosim.snapshot_path, "wb");
    if (!fp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx cosim: failed to open snapshot '%s': %s\n",
                      env->cosim.snapshot_path, strerror(errno));
        return false;
    }

    table = g_new0(LinxCosimSnapshotRange, env->cosim.range_count);
    if (!table) {
        goto out;
    }

    payload_off = sizeof(hdr) + ((uint64_t)env->cosim.range_count * sizeof(*table));
    for (i = 0; i < env->cosim.range_count; i++) {
        table[i].base = env->cosim.ranges[i].base;
        table[i].size = env->cosim.ranges[i].size;
        table[i].file_offset = payload_off;
        payload_off += env->cosim.ranges[i].size;
    }

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        goto out;
    }
    if (env->cosim.range_count > 0 &&
        fwrite(table, sizeof(*table), env->cosim.range_count, fp) != env->cosim.range_count) {
        goto out;
    }

    for (i = 0; i < env->cosim.range_count; i++) {
        uint64_t remain = env->cosim.ranges[i].size;
        uint64_t addr = env->cosim.ranges[i].base;
        uint8_t chunk[4096];

        while (remain > 0) {
            const size_t n = (size_t)MIN((uint64_t)sizeof(chunk), remain);
            if (cpu_memory_rw_debug(cs, addr, chunk, n, 0) != 0) {
                memset(chunk, 0, n);
            }
            if (fwrite(chunk, 1, n, fp) != n) {
                goto out;
            }
            addr += n;
            remain -= n;
        }
    }

    ok = true;
out:
    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx cosim: snapshot write failed for '%s'\n",
                      env->cosim.snapshot_path);
    }
    g_free(table);
    fclose(fp);
    return ok;
}

static bool linx_cosim_parse_seq(const char *line, uint64_t *seq_out)
{
    const char *p = strstr(line, "\"seq\"");
    char *endp = NULL;

    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    errno = 0;
    *seq_out = strtoull(p, &endp, 10);
    return errno == 0 && endp && endp != p;
}

static void linx_cosim_fail_fast(CPULinxState *env, const char *why, const char *line)
{
    if (line && line[0]) {
        qemu_log_mask(LOG_GUEST_ERROR, "Linx cosim: %s: %s\n", why, line);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "Linx cosim: %s\n", why);
    }
    linx_cosim_finish(env);
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_PANIC);
    cpu_loop_exit_noexc(env_cpu(env));
}

static bool linx_cosim_send_end(CPULinxState *env, const char *reason)
{
    char line[256];

    if (env->cosim.sock_fd < 0) {
        return false;
    }
    snprintf(line, sizeof(line), "{\"type\":\"end\",\"reason\":\"%s\"}", reason);
    return linx_cosim_send_line(env, line);
}

static void linx_cosim_init(CPULinxState *env)
{
    const char *trigger_s;
    const char *terminate_s;
    const char *socket_s;
    const char *snapshot_s;
    const char *ranges_s;
    const char *max_commits_s;
    uint64_t max_commits = 0;

    if (env->cosim.inited) {
        return;
    }
    memset(&env->cosim, 0, sizeof(env->cosim));
    env->cosim.sock_fd = -1;
    env->cosim.inited = 1;

    if (!linx_env_enabled("LINX_COSIM_ENABLE")) {
        env->cosim.enabled = 0;
        linx_refresh_tb_cosim_precheck(env);
        return;
    }

    trigger_s = getenv("LINX_COSIM_TRIGGER_PC");
    terminate_s = getenv("LINX_COSIM_TERMINATE_PC");
    socket_s = getenv("LINX_COSIM_SOCKET");
    snapshot_s = getenv("LINX_COSIM_SNAPSHOT_PATH");
    ranges_s = getenv("LINX_COSIM_MEM_RANGES");
    max_commits_s = getenv("LINX_COSIM_MAX_COMMITS");

    if (!linx_parse_u64(trigger_s, &env->cosim.trigger_pc) ||
        !linx_parse_u64(terminate_s, &env->cosim.terminate_pc) ||
        !socket_s || !socket_s[0] || !snapshot_s || !snapshot_s[0] ||
        !linx_cosim_parse_ranges(env, ranges_s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx cosim: invalid configuration; disable co-sim mode\n");
        env->cosim.enabled = 0;
        linx_refresh_tb_cosim_precheck(env);
        return;
    }

    if (strlen(socket_s) >= sizeof(env->cosim.socket_path) ||
        strlen(snapshot_s) >= sizeof(env->cosim.snapshot_path)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx cosim: socket or snapshot path too long\n");
        env->cosim.enabled = 0;
        linx_refresh_tb_cosim_precheck(env);
        return;
    }
    strcpy(env->cosim.socket_path, socket_s);
    strcpy(env->cosim.snapshot_path, snapshot_s);

    if (max_commits_s && max_commits_s[0]) {
        if (!linx_parse_u64(max_commits_s, &max_commits)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx cosim: invalid LINX_COSIM_MAX_COMMITS='%s'\n",
                          max_commits_s);
            env->cosim.enabled = 0;
            linx_refresh_tb_cosim_precheck(env);
            return;
        }
    }
    env->cosim.max_commits = max_commits;
    env->cosim.enabled = 1;
    linx_refresh_tb_cosim_precheck(env);
}

void HELPER(linx_cosim_before_insn)(CPULinxState *env, uint64_t pc)
{
    char start_line[1024];

    linx_queue_trace_probe(env, pc);
    linx_debug_pc_watch_probe(env, pc);
    linx_debug_work_grab_probe(env, pc);

    linx_cosim_init(env);
    linx_refresh_tb_cosim_precheck(env);
    if (!env->cosim.enabled || env->cosim.active || env->cosim.ended) {
        return;
    }
    if (pc != env->cosim.trigger_pc) {
        return;
    }
    if (!linx_cosim_dump_snapshot(env) || !linx_cosim_connect(env)) {
        env->cosim.enabled = 0;
        linx_cosim_finish(env);
        return;
    }

    snprintf(start_line, sizeof(start_line),
             "{\"type\":\"start\",\"boot_pc\":%" PRIu64
             ",\"boot_sp\":%" PRIu64
             ",\"boot_ra\":%" PRIu64
             ",\"trigger_pc\":%" PRIu64
             ",\"terminate_pc\":%" PRIu64
             ",\"snapshot_path\":\"%s\",\"seq_base\":0}",
             pc, env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA], env->cosim.trigger_pc, env->cosim.terminate_pc,
             env->cosim.snapshot_path);
    if (!linx_cosim_send_line(env, start_line)) {
        env->cosim.enabled = 0;
        linx_cosim_finish(env);
        return;
    }
    env->cosim.seq = 0;
    env->cosim.active = 1;
    linx_refresh_tb_cosim_precheck(env);
}

static void linx_commit_trace_init(CPULinxState *env)
{
    if (env->commit_trace.inited) {
        return;
    }
    env->commit_trace.inited = 1;
    env->commit_trace.stop_after_commit = 0;

    const char *path = getenv("LINX_COMMIT_TRACE");
    if (!path || !path[0] || strcmp(path, "0") == 0) {
        env->commit_trace.enabled = 0;
        return;
    }

    env->commit_trace.fp = fopen(path, "w");
    if (!env->commit_trace.fp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: failed to open LINX_COMMIT_TRACE='%s'\n",
                      path);
        env->commit_trace.enabled = 0;
        return;
    }

    env->commit_trace.enabled = 1;
    env->commit_trace.cycle = 0;

    const char *lo_s = getenv("LINX_COMMIT_TRACE_FILTER_PC_LO");
    if (lo_s && lo_s[0] && strcmp(lo_s, "0") != 0) {
        char *endp = NULL;
        errno = 0;
        uint64_t lo = strtoull(lo_s, &endp, 0);
        if (errno == 0 && endp && endp != lo_s && *endp == '\0') {
            uint64_t hi = lo;
            const char *hi_s = getenv("LINX_COMMIT_TRACE_FILTER_PC_HI");
            if (hi_s && hi_s[0] && strcmp(hi_s, "0") != 0) {
                char *endp2 = NULL;
                errno = 0;
                uint64_t parsed = strtoull(hi_s, &endp2, 0);
                if (errno == 0 && endp2 && endp2 != hi_s && *endp2 == '\0') {
                    hi = parsed;
                }
            }
            env->commit_trace.pc_lo = MIN(lo, hi);
            env->commit_trace.pc_hi = MAX(lo, hi);
            env->commit_trace.pc_filter_enabled = 1;
        }
    }
}

static void linx_minst_trace_init(CPULinxState *env)
{
    if (env->minst_trace.inited) {
        return;
    }
    env->minst_trace.inited = 1;

    const char *path = getenv("LINX_MINST_TRACE");
    if (!path || !path[0] || strcmp(path, "0") == 0) {
        env->minst_trace.enabled = 0;
        return;
    }

    env->minst_trace.fp = fopen(path, "w");
    if (!env->minst_trace.fp) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: failed to open LINX_MINST_TRACE='%s'\n",
                      path);
        env->minst_trace.enabled = 0;
        return;
    }

    env->minst_trace.enabled = 1;
    env->minst_trace.stop_after_commit = 0;
    env->minst_trace.cycle = 0;
    env->minst_trace.pc_bias_valid = 0;
    env->minst_trace.pending_block_kind = 0;
    env->minst_trace.active_block_kind = 0;
    env->minst_trace.pc_bias = 0;

    const char *lo_s = getenv("LINX_COMMIT_TRACE_FILTER_PC_LO");
    if (lo_s && lo_s[0] && strcmp(lo_s, "0") != 0) {
        char *endp = NULL;
        errno = 0;
        uint64_t lo = strtoull(lo_s, &endp, 0);
        if (errno == 0 && endp && endp != lo_s && *endp == '\0') {
            uint64_t hi = lo;
            const char *hi_s = getenv("LINX_COMMIT_TRACE_FILTER_PC_HI");
            if (hi_s && hi_s[0] && strcmp(hi_s, "0") != 0) {
                char *endp2 = NULL;
                errno = 0;
                uint64_t parsed = strtoull(hi_s, &endp2, 0);
                if (errno == 0 && endp2 && endp2 != hi_s && *endp2 == '\0') {
                    hi = parsed;
                }
            }
            env->minst_trace.pc_lo = MIN(lo, hi);
            env->minst_trace.pc_hi = MAX(lo, hi);
            env->minst_trace.pc_filter_enabled = 1;
        }
    }
}

static inline bool linx_minst_trace_active(CPULinxState *env)
{
    linx_minst_trace_init(env);
    return env->minst_trace.enabled && env->minst_trace.fp;
}

static inline QEMU_ALWAYS_INLINE bool linx_trace_capture_active(CPULinxState *env)
{
    if (likely(env->trace_capture_disabled_fast)) {
        return false;
    }
    if (!env->cosim.inited) {
        linx_cosim_init(env);
    }
    if (!env->commit_trace.inited) {
        linx_commit_trace_init(env);
    }
    if (!env->minst_trace.inited) {
        linx_minst_trace_init(env);
    }
    const bool mem_log_active = qemu_loglevel_mask(LOG_LINX_MEM);
    const bool active =
        (env->commit_trace.enabled && env->commit_trace.fp) ||
        (env->minst_trace.enabled && env->minst_trace.fp) ||
        env->cosim.active || mem_log_active;

    if (!active && !env->cosim.enabled) {
        env->trace_capture_disabled_fast = 1;
    }
    return active;
}

static inline uint32_t linx_trace_len_to_meta_len(uint32_t len)
{
    switch (len) {
    case 2:
        return 16;
    case 4:
        return 32;
    case 6:
        return 64;
    case 8:
        return 64;
    default:
        return 0;
    }
}

static inline uint32_t linx_trace_len_to_bits(uint32_t len)
{
    switch (len) {
    case 2:
        return 16;
    case 4:
        return 32;
    case 6:
        return 48;
    case 8:
        return 64;
    default:
        return 0;
    }
}

static inline bool linx_trace_kind_is_reg(const char *kind)
{
    return kind && strcmp(kind, "REG") == 0;
}

static inline uint32_t linx_trace_extract_rd(uint64_t insn_raw, uint32_t len)
{
    if (len == 2) {
        const uint16_t hw = (uint16_t)(insn_raw & 0xffffu);
        return (uint32_t)((hw >> 11) & 0x1fu);
    }
    if (len == 6) {
        const uint32_t main32 = (uint32_t)((insn_raw >> 16) & 0xffffffffu);
        return (main32 >> 7) & 0x1fu;
    }
    return (uint32_t)((insn_raw >> 7) & 0x1fu);
}

static inline uint32_t linx_trace_extract_rs1(uint64_t insn_raw, uint32_t len)
{
    if (len == 2) {
        const uint16_t hw = (uint16_t)(insn_raw & 0xffffu);
        /* 16-bit %SrcL field is bits[10:6]. */
        return (uint32_t)((hw >> 6) & 0x1fu);
    }
    if (len == 6) {
        const uint32_t main32 = (uint32_t)((insn_raw >> 16) & 0xffffffffu);
        return (main32 >> 15) & 0x1fu;
    }
    return (uint32_t)((insn_raw >> 15) & 0x1fu);
}

static inline uint32_t linx_trace_extract_rs2(uint64_t insn_raw, uint32_t len)
{
    if (len == 2) {
        const uint16_t hw = (uint16_t)(insn_raw & 0xffffu);
        return (uint32_t)((hw >> 11) & 0x1fu);
    }
    if (len == 6) {
        const uint32_t main32 = (uint32_t)((insn_raw >> 16) & 0xffffffffu);
        return (main32 >> 20) & 0x1fu;
    }
    return (uint32_t)((insn_raw >> 20) & 0x1fu);
}

void HELPER(linx_trace_operands_begin)(CPULinxState *env, uint64_t insn_raw, uint32_t len)
{
    const LinxOpcodeMeta *meta;
    const uint32_t len_meta = linx_trace_len_to_meta_len(len);
    const uint32_t rd = linx_trace_extract_rd(insn_raw, len);
    const uint32_t rs1 = linx_trace_extract_rs1(insn_raw, len);
    const uint32_t rs2 = linx_trace_extract_rs2(insn_raw, len);

    if (!linx_trace_capture_active(env)) {
        return;
    }

    env->trace_src0_valid = 0;
    env->trace_src0_reg = 0;
    env->trace_src0_data = 0;
    env->trace_src1_valid = 0;
    env->trace_src1_reg = 0;
    env->trace_src1_data = 0;
    env->trace_dst_valid = 0;
    env->trace_dst_reg = 0;
    env->trace_dst_data = 0;

    meta = linx_opcode_meta_lookup(insn_raw, len_meta);
    if (!meta) {
        meta = linx_opcode_meta_lookup(insn_raw, 0);
    }
    if (!meta) {
        return;
    }

    if (linx_trace_kind_is_reg(meta->rs1_kind) && rs1 < LINX_GPR_COUNT) {
        env->trace_src0_valid = 1;
        env->trace_src0_reg = rs1;
        env->trace_src0_data = env->gpr[rs1];
    }
    if (linx_trace_kind_is_reg(meta->rs2_kind) && rs2 < LINX_GPR_COUNT) {
        env->trace_src1_valid = 1;
        env->trace_src1_reg = rs2;
        env->trace_src1_data = env->gpr[rs2];
    }
    if (linx_trace_kind_is_reg(meta->rd_kind)) {
        env->trace_dst_valid = 1;
        env->trace_dst_reg = rd;
    }
}

static inline QEMU_ALWAYS_INLINE void linx_trace_wb(CPULinxState *env,
                                                    uint32_t rd, uint64_t data)
{
    if (!linx_trace_capture_active(env)) {
        return;
    }
    env->trace_wb_valid = 1;
    env->trace_wb_rd = rd;
    env->trace_wb_data = data;
    env->trace_dst_valid = 1;
    env->trace_dst_reg = rd;
    env->trace_dst_data = data;
    trace_linx_reg_trace(env->pc, 1, rd, env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
                         env->brtype & 0x7u, env->cond, env->tgt,
                         data, 0);
}

static inline QEMU_ALWAYS_INLINE void linx_trace_mem(CPULinxState *env,
                                                     bool is_store,
                                                     uint64_t addr,
                                                     uint64_t wdata,
                                                     uint64_t rdata,
                                                     uint32_t size)
{
    if (!linx_trace_capture_active(env)) {
        return;
    }
    env->trace_mem_valid = 1;
    env->trace_mem_is_store = is_store ? 1 : 0;
    env->trace_mem_addr = addr;
    env->trace_mem_size = size;
    env->trace_mem_wdata = is_store ? wdata : 0;
    env->trace_mem_rdata = is_store ? 0 : rdata;
}

static inline void linx_trace_mem_clear(CPULinxState *env)
{
    env->trace_mem_valid = 0;
    env->trace_mem_is_store = 0;
    env->trace_mem_addr = 0;
    env->trace_mem_size = 0;
    env->trace_mem_wdata = 0;
    env->trace_mem_rdata = 0;
}

static inline void linx_template_commit_trace_if_needed(CPULinxState *env,
                                                        uint64_t next_pc)
{
    if (linx_trace_capture_active(env)) {
        HELPER(linx_commit_trace)(env, next_pc);
    }
}

static inline G_NORETURN void linx_template_commit_and_exit(CPULinxState *env,
                                                            CPUState *cs,
                                                            uint64_t next_pc)
{
    linx_template_commit_trace_if_needed(env, next_pc);
    cpu_loop_exit_noexc(cs);
}

static inline void linx_template_commit_or_chain(CPULinxState *env,
                                                 CPUState *cs,
                                                 uint64_t next_pc,
                                                 bool chain)
{
    linx_template_commit_trace_if_needed(env, next_pc);
    if (!chain) {
        cpu_loop_exit_noexc(cs);
    }
}

static inline G_NORETURN void linx_template_exit_without_commit(CPULinxState *env,
                                                                CPUState *cs)
{
    (void)env;
    cpu_loop_exit_noexc(cs);
}

static void linx_cosim_send_commit_and_wait_ack(CPULinxState *env, uint64_t next_pc)
{
    char line[4096];
    char ack[2048];
    uint64_t ack_seq = UINT64_MAX;
    const uint64_t seq = env->cosim.seq;
    const uint32_t dst_valid = env->trace_wb_valid ? 1u : env->trace_dst_valid;
    const uint32_t dst_reg = env->trace_wb_valid ? env->trace_wb_rd : env->trace_dst_reg;
    const uint64_t dst_data = env->trace_wb_valid ? env->trace_wb_data : env->trace_dst_data;

    if (!env->cosim.active) {
        return;
    }

    snprintf(line, sizeof(line),
             "{\"type\":\"commit\",\"seq\":%" PRIu64
             ",\"pc\":%" PRIu64
             ",\"insn\":%" PRIu64
             ",\"len\":%u"
             ",\"wb_valid\":%u,\"wb_rd\":%u,\"wb_data\":%" PRIu64
             ",\"src0_valid\":%u,\"src0_reg\":%u,\"src0_data\":%" PRIu64
             ",\"src1_valid\":%u,\"src1_reg\":%u,\"src1_data\":%" PRIu64
             ",\"dst_valid\":%u,\"dst_reg\":%u,\"dst_data\":%" PRIu64
             ",\"mem_valid\":%u,\"mem_is_store\":%u"
             ",\"mem_addr\":%" PRIu64 ",\"mem_wdata\":%" PRIu64
             ",\"mem_rdata\":%" PRIu64 ",\"mem_size\":%u"
             ",\"trap_valid\":%u,\"trap_cause\":%u,\"traparg0\":%" PRIu64
             ",\"next_pc\":%" PRIu64 "}",
             seq,
             env->trace_pc, env->trace_insn, env->trace_len,
             env->trace_wb_valid, env->trace_wb_rd, env->trace_wb_data,
             env->trace_src0_valid, env->trace_src0_reg, env->trace_src0_data,
             env->trace_src1_valid, env->trace_src1_reg, env->trace_src1_data,
             dst_valid, dst_reg, dst_data,
             env->trace_mem_valid, env->trace_mem_is_store,
             env->trace_mem_addr, env->trace_mem_wdata, env->trace_mem_rdata, env->trace_mem_size,
             env->trace_trap_valid, env->trace_trap_cause, env->trace_traparg0,
             next_pc);
    if (!linx_cosim_send_line(env, line)) {
        linx_cosim_fail_fast(env, "failed to send commit", NULL);
    }
    if (!linx_cosim_recv_line(env, ack, sizeof(ack))) {
        linx_cosim_fail_fast(env, "failed to receive ack", NULL);
    }
    if (!linx_cosim_parse_seq(ack, &ack_seq) || ack_seq != seq) {
        linx_cosim_fail_fast(env, "ack sequence mismatch", ack);
    }
    if (strstr(ack, "\"status\":\"mismatch\"")) {
        linx_cosim_fail_fast(env, "mismatch reported by DUT", ack);
    }
    if (!strstr(ack, "\"status\":\"ok\"")) {
        linx_cosim_fail_fast(env, "invalid ack status", ack);
    }

    env->cosim.seq = seq + 1;

    if (env->trace_pc == env->cosim.terminate_pc) {
        (void)linx_cosim_send_end(env, "terminate_pc");
        linx_cosim_finish(env);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        cpu_loop_exit_noexc(env_cpu(env));
    }
    if (env->cosim.max_commits && env->cosim.seq >= env->cosim.max_commits) {
        (void)linx_cosim_send_end(env, "max_commits");
        linx_cosim_finish(env);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        cpu_loop_exit_noexc(env_cpu(env));
    }
}

static uint64_t linx_trace_canonical_insn(uint64_t insn_raw, uint32_t len,
                                          const LinxOpcodeMeta *meta)
{
    uint64_t v = insn_raw;
    (void)meta;

    if (len == 2) {
        return v & 0xffffu;
    }
    if (len == 4) {
        v &= 0xffffffffu;
        return v;
    }
    if (len == 6) {
        return v & UINT64_C(0xffffffffffff);
    }
    return v;
}

static const char *linx_trace_block_kind_name(const LinxOpcodeMeta *meta,
                                              uint64_t insn_raw, uint32_t len)
{
    const char *mnemonic;

    if (len == 2) {
        const uint16_t hw = (uint16_t)(insn_raw & 0xffffu);
        if (hw == 0x88c0u) {
            return "vpar";
        }
        if (hw == 0xc8c0u) {
            return "vseq";
        }
    }

    if (!meta) {
        return "scalar";
    }

    if (meta->minor_cat && strcmp(meta->minor_cat, "sys") == 0) {
        return "sys";
    }

    mnemonic = meta->mnemonic;
    if (!mnemonic) {
        return "scalar";
    }
    if (strstr(mnemonic, "bstart_vpar")) {
        return "vpar";
    }
    if (strstr(mnemonic, "bstart_vseq")) {
        return "vseq";
    }
    if (strcmp(mnemonic, "bstart_tload") == 0 ||
        strcmp(mnemonic, "bstart_tstore") == 0 ||
        strcmp(mnemonic, "bstart_tmov") == 0) {
        return "tma";
    }
    if (strcmp(mnemonic, "bstart_cube") == 0 ||
        strcmp(mnemonic, "bstart_tmatmul") == 0 ||
        strcmp(mnemonic, "bstart_tmatmul_acc") == 0) {
        return "cube";
    }
    if (strstr(mnemonic, "tepl")) {
        return "tepl";
    }

    return "scalar";
}

static int32_t linx_trace_lane_id_for_kind(const char *block_kind)
{
    if (!block_kind) {
        return -1;
    }
    if (strcmp(block_kind, "vpar") == 0 || strcmp(block_kind, "vseq") == 0) {
        return 0;
    }
    return -1;
}

static const char *linx_minst_opcode_class_name(const LinxOpcodeMeta *meta)
{
    if (!meta) {
        return "invalid";
    }
    switch (meta->major_cat) {
    case LINX_CAT_LOAD:
        return "load";
    case LINX_CAT_STORE:
        return "store";
    case LINX_CAT_BRU_SETC_CMP:
        return "branch";
    case LINX_CAT_CMD_PIPE:
    case LINX_CAT_MACRO_TEMPLATE:
    case LINX_CAT_FP_SYS:
        return "system";
    case LINX_CAT_VECTOR:
        return "int";
    case LINX_CAT_ALU_INT:
    case LINX_CAT_COMPRESSED:
    case LINX_CAT_BLOCK_BOUNDARY:
    case LINX_CAT_BLOCK_ARGS_DESC:
    case LINX_CAT_MISC:
    case LINX_CAT_HL_PCR:
    default:
        return "int";
    }
}

typedef struct LinxMinstCanonicalInfo {
    const char *mnemonic;
    const char *form_id;
    const char *opcode_class;
    const char *block_kind_override;
} LinxMinstCanonicalInfo;

static LinxMinstCanonicalInfo linx_minst_canonical_info(const LinxOpcodeMeta *meta)
{
    const char *mnemonic = meta && meta->mnemonic ? meta->mnemonic : "";

    if (strcmp(mnemonic, "addi") == 0) {
        return (LinxMinstCanonicalInfo){ "ADDI", "2decd0a93a0a", "int", NULL };
    }
    if (strcmp(mnemonic, "b_text") == 0) {
        return (LinxMinstCanonicalInfo){ "B.TEXT", "1ce09f50e5dd", "system", "tma" };
    }
    if (strcmp(mnemonic, "bstart_split_direct") == 0) {
        return (LinxMinstCanonicalInfo){ "BSTART", "7eb93b649748", "system", NULL };
    }
    if (strcmp(mnemonic, "bstart_split_cond") == 0) {
        return (LinxMinstCanonicalInfo){ "BSTART", "e11e678a32ac", "system", NULL };
    }
    if (strcmp(mnemonic, "bstart_tload") == 0) {
        return (LinxMinstCanonicalInfo){ "BSTART.TLOAD", "d0c18bb0ab15", "system", "tma" };
    }
    if (strcmp(mnemonic, "bstart_tstore") == 0) {
        return (LinxMinstCanonicalInfo){ "BSTART.TSTORE", "4048b6e8b0f4", "system", "tma" };
    }
    if (strcmp(mnemonic, "bstart_tmov") == 0) {
        return (LinxMinstCanonicalInfo){ "BSTART.TMOV", "211446509efb", "system", "tma" };
    }
    if (strcmp(mnemonic, "c_bstart_cond") == 0) {
        return (LinxMinstCanonicalInfo){ "C.BSTART", "c4e238a9227a", "system", NULL };
    }
    if (strcmp(mnemonic, "c_bstart_direct") == 0) {
        return (LinxMinstCanonicalInfo){ "C.BSTART", "f833d2a4753c", "system", NULL };
    }
    if (strcmp(mnemonic, "c_bstart_std_fall") == 0) {
        return (LinxMinstCanonicalInfo){ "C.BSTART.STD", "8b40f078c14a", "invalid", NULL };
    }
    if (strcmp(mnemonic, "c_bstart_vpar") == 0) {
        return (LinxMinstCanonicalInfo){ "C.BSTART.VPAR", "c4d89efc71ea", "invalid", NULL };
    }
    if (strcmp(mnemonic, "c_bstart_vseq") == 0) {
        return (LinxMinstCanonicalInfo){ "C.BSTART.VSEQ", "50d70de3f84f", "invalid", NULL };
    }
    if (strcmp(mnemonic, "c_bstop") == 0) {
        return (LinxMinstCanonicalInfo){ "C.BSTOP", "ca4743d8a95e", "system", NULL };
    }
    if (strcmp(mnemonic, "c_movi") == 0) {
        return (LinxMinstCanonicalInfo){ "C.MOVI", "2c84faf1bc72", "int", NULL };
    }
    if (strcmp(mnemonic, "c_movr") == 0) {
        return (LinxMinstCanonicalInfo){ "C.MOVR", "80d2b5f3580b", "int", NULL };
    }
    if (strcmp(mnemonic, "hl_lui") == 0) {
        return (LinxMinstCanonicalInfo){ "HL.LUI", "255991889818", "int", NULL };
    }
    if (strcmp(mnemonic, "lui") == 0) {
        return (LinxMinstCanonicalInfo){ "LUI", "982113b541d6", "int", NULL };
    }
    if (strcmp(mnemonic, "lwi") == 0) {
        return (LinxMinstCanonicalInfo){ "LWI", "7085c98058fa", "load", NULL };
    }
    if (strcmp(mnemonic, "mcopy") == 0) {
        return (LinxMinstCanonicalInfo){ "MCOPY", "4fc4a803e995", "system", NULL };
    }
    if (strcmp(mnemonic, "mset") == 0) {
        return (LinxMinstCanonicalInfo){ "MSET", "0b932f291932", "system", NULL };
    }
    if (strcmp(mnemonic, "setc_ltu") == 0) {
        return (LinxMinstCanonicalInfo){ "SETC.LTU", "4a1ff65ecafb", "branch", NULL };
    }
    if (strcmp(mnemonic, "setc_ne") == 0) {
        return (LinxMinstCanonicalInfo){ "SETC.NE", "77576a5c690c", "branch", NULL };
    }
    if (strcmp(mnemonic, "ssrset") == 0) {
        return (LinxMinstCanonicalInfo){ "SSRSET", "4dd3b71802c6", "system", NULL };
    }
    if (strcmp(mnemonic, "swi") == 0) {
        return (LinxMinstCanonicalInfo){ "SWI", "147e55489c41", "store", NULL };
    }

    return (LinxMinstCanonicalInfo){ mnemonic, "", linx_minst_opcode_class_name(meta), NULL };
}

static inline uint64_t linx_minst_canonical_pc(CPULinxState *env, uint64_t pc)
{
    if (!env->minst_trace.pc_bias_valid) {
        env->minst_trace.pc_bias = pc;
        env->minst_trace.pc_bias_valid = 1;
    }
    return pc - env->minst_trace.pc_bias;
}

static const char *linx_trace_context_block_kind(const CPULinxState *env)
{
    switch (env->blocktype) {
    case 1:
        return "sys";
    case 2:
        return "tma";
    case 4:
        return "vpar";
    case 5:
        return "vseq";
    case 6:
        return "cube";
    case 7:
        return "tepl";
    default:
        return NULL;
    }
}

static inline uint8_t linx_minst_block_kind_code(const char *block_kind)
{
    if (!block_kind || strcmp(block_kind, "scalar") == 0) {
        return 0;
    }
    if (strcmp(block_kind, "sys") == 0) {
        return 1;
    }
    if (strcmp(block_kind, "tma") == 0) {
        return 2;
    }
    if (strcmp(block_kind, "vpar") == 0) {
        return 3;
    }
    if (strcmp(block_kind, "vseq") == 0) {
        return 4;
    }
    if (strcmp(block_kind, "cube") == 0) {
        return 5;
    }
    if (strcmp(block_kind, "tepl") == 0) {
        return 6;
    }
    return 0;
}

static inline const char *linx_minst_block_kind_name_from_code(uint8_t code)
{
    switch (code) {
    case 1:
        return "sys";
    case 2:
        return "tma";
    case 3:
        return "vpar";
    case 4:
        return "vseq";
    case 5:
        return "cube";
    case 6:
        return "tepl";
    default:
        return "scalar";
    }
}

static void linx_emit_minst_trace(CPULinxState *env, uint64_t next_pc)
{
    bool emit_file = false;
    uint64_t pc;
    uint64_t pc_out;
    uint64_t next_pc_out;
    uint64_t cycle;
    uint32_t trap_valid;
    uint32_t trap_cause;
    uint32_t dst_valid;
    uint32_t dst_reg;
    uint64_t dst_data_raw;
    uint64_t dst_data;
    uint32_t len_meta;
    const LinxOpcodeMeta *meta;
    uint64_t canonical_insn;
    const char *block_kind;
    int32_t lane_id;
    uint32_t len_bits;
    LinxMinstCanonicalInfo info;
    bool is_macro_template;
    uint32_t src0_valid;
    uint32_t src1_valid;
    uint32_t mem_valid;
    uint32_t mem_is_load;
    uint32_t mem_is_store;
    uint64_t mem_addr;
    uint32_t mem_size;
    uint64_t mem_rdata;
    const char *context_block_kind;
    const uint32_t trace_rs2 = linx_trace_extract_rs2(env->trace_insn, env->trace_len);
    uint8_t emitted_block_kind_code;
    bool consume_pending_non_scalar = false;
    bool activate_sys_context = false;
    bool terminal_store = false;

    linx_minst_trace_init(env);
    emit_file = env->minst_trace.enabled && env->minst_trace.fp;
    if (!emit_file) {
        return;
    }

    pc = env->trace_pc;
    if (env->minst_trace.pc_filter_enabled &&
        (pc < env->minst_trace.pc_lo || pc > env->minst_trace.pc_hi)) {
        return;
    }

    trap_valid = env->trace_trap_valid;
    trap_cause = env->trace_trap_cause;
    dst_valid = env->trace_wb_valid ? 1u : env->trace_dst_valid;
    dst_reg = env->trace_wb_valid ? env->trace_wb_rd : env->trace_dst_reg;
    dst_data_raw = env->trace_wb_valid ? env->trace_wb_data : env->trace_dst_data;
    len_meta = linx_trace_len_to_meta_len(env->trace_len);
    meta = linx_opcode_meta_lookup(env->trace_insn, len_meta);
    if (!meta) {
        meta = linx_opcode_meta_lookup(env->trace_insn, 0);
    }
    canonical_insn = linx_trace_canonical_insn(env->trace_insn, env->trace_len, meta);
    block_kind = linx_trace_block_kind_name(meta, canonical_insn, env->trace_len);
    info = linx_minst_canonical_info(meta);
    if (info.block_kind_override) {
        block_kind = info.block_kind_override;
    }
    context_block_kind = linx_trace_context_block_kind(env);
    if (context_block_kind &&
        strcmp(block_kind, "scalar") == 0 &&
        strcmp(info.mnemonic, "C.BSTART.STD") != 0) {
        block_kind = context_block_kind;
    }
    if (strcmp(info.mnemonic, "C.BSTART.STD") == 0 &&
        strcmp(block_kind, "scalar") == 0 &&
        env->minst_trace.pending_block_kind != 0) {
        block_kind = linx_minst_block_kind_name_from_code(env->minst_trace.pending_block_kind);
        consume_pending_non_scalar = true;
    } else if (strcmp(block_kind, "scalar") == 0 &&
               env->minst_trace.active_block_kind == 1) {
        block_kind = "sys";
    }
    lane_id = linx_trace_lane_id_for_kind(block_kind);
    len_bits = linx_trace_len_to_bits(env->trace_len);
    pc_out = linx_minst_canonical_pc(env, pc);
    next_pc_out = linx_minst_canonical_pc(env, next_pc);
    is_macro_template = meta && meta->major_cat == LINX_CAT_MACRO_TEMPLATE;
    src0_valid = env->trace_src0_valid;
    src1_valid = env->trace_src1_valid;
    if (strcmp(info.mnemonic, "SWI") == 0) {
        src1_valid = 1;
    }
    mem_valid = is_macro_template ? 0u : env->trace_mem_valid;
    mem_is_load = mem_valid && !env->trace_mem_is_store;
    mem_is_store = mem_valid && env->trace_mem_is_store;
    mem_addr = mem_valid ? env->trace_mem_addr : 0;
    mem_size = mem_valid ? env->trace_mem_size : 0;
    mem_rdata = mem_is_load ? env->trace_mem_rdata : 0;
    terminal_store = mem_is_store &&
                     env->trace_mem_addr == LINX_VIRT_FINISHER_ADDR;
    dst_data = dst_valid ? dst_data_raw : 0;
    if (trap_valid && strcmp(info.mnemonic, "C.BSTOP") == 0) {
        return;
    }
    if (next_pc_out == pc_out &&
        (strcmp(info.mnemonic, "BSTART") == 0 ||
         strncmp(info.mnemonic, "BSTART.", 7) == 0 ||
         strncmp(info.mnemonic, "C.BSTART", 8) == 0)) {
        return;
    }
    cycle = env->minst_trace.cycle++;
    emitted_block_kind_code = linx_minst_block_kind_code(block_kind);
    activate_sys_context = emitted_block_kind_code == 1;

    fprintf(env->minst_trace.fp,
            "{\"schema_version\":\"1.0\""
            ",\"cycle\":%" PRIu64
            ",\"pc\":%" PRIu64
            ",\"next_pc\":%" PRIu64
            ",\"insn\":%" PRIu64
            ",\"len\":%u"
            ",\"lane_id\":%d"
            ",\"mnemonic\":\"%s\""
            ",\"form_id\":\"%s\""
            ",\"opcode_class\":\"%s\""
            ",\"lifecycle\":\"retired\""
            ",\"block_kind\":\"%s\""
            ",\"src0_valid\":%u,\"src0_kind\":%u,\"src0_value\":%u,\"src0_data\":%" PRIu64
            ",\"src1_valid\":%u,\"src1_kind\":%u,\"src1_value\":%u,\"src1_data\":%" PRIu64
            ",\"dst0_valid\":%u,\"dst0_kind\":%u,\"dst0_value\":%u,\"dst0_data\":%" PRIu64
            ",\"mem_valid\":%u,\"mem_is_load\":%u,\"mem_is_store\":%u,\"mem_addr\":%" PRIu64
            ",\"mem_size\":%u,\"mem_wdata\":%" PRIu64 ",\"mem_rdata\":%" PRIu64
            ",\"trap_valid\":%u,\"trap_cause\":%u,\"traparg0\":%" PRIu64 "}\n",
            cycle,
            pc_out,
            next_pc_out,
            canonical_insn,
            len_bits,
            lane_id,
            info.mnemonic,
            info.form_id,
            info.opcode_class,
            block_kind,
            src0_valid, src0_valid ? LINX_MINST_OPERAND_REGISTER : LINX_MINST_OPERAND_INVALID,
            src0_valid ? env->trace_src0_reg : 0u, 0ull,
            src1_valid, src1_valid ? LINX_MINST_OPERAND_REGISTER : LINX_MINST_OPERAND_INVALID,
            src1_valid ? (strcmp(info.mnemonic, "SWI") == 0 ? trace_rs2 : env->trace_src1_reg) : 0u, 0ull,
            dst_valid, dst_valid ? LINX_MINST_OPERAND_REGISTER : LINX_MINST_OPERAND_INVALID,
            dst_valid ? dst_reg : 0u, dst_data,
            mem_valid, mem_is_load, mem_is_store, mem_addr,
            mem_size, mem_is_store ? env->trace_mem_wdata : 0ull, mem_rdata,
            trap_valid, trap_cause, env->trace_traparg0);
    fflush(env->minst_trace.fp);

    if (consume_pending_non_scalar) {
        env->minst_trace.pending_block_kind = 0;
    }
    if (activate_sys_context) {
        env->minst_trace.active_block_kind = 1;
    } else if (strcmp(info.mnemonic, "C.BSTOP") == 0 ||
               strcmp(info.mnemonic, "BSTART") == 0 ||
               strncmp(info.mnemonic, "BSTART.", 7) == 0 ||
               strncmp(info.mnemonic, "C.BSTART.", 9) == 0 ||
               strcmp(info.mnemonic, "C.BSTART") == 0) {
        env->minst_trace.active_block_kind = 0;
    }
    if (emitted_block_kind_code >= 2 && !consume_pending_non_scalar) {
        env->minst_trace.pending_block_kind = emitted_block_kind_code;
    }
    if (env->minst_trace.stop_after_commit || terminal_store) {
        fclose(env->minst_trace.fp);
        env->minst_trace.fp = NULL;
        env->minst_trace.enabled = 0;
        env->minst_trace.stop_after_commit = 0;
    }
}

void HELPER(linx_commit_trace)(CPULinxState *env, uint64_t next_pc)
{
    bool emit_file = false;

    linx_commit_trace_init(env);
    emit_file = env->commit_trace.enabled && env->commit_trace.fp;
    if (emit_file) {
        const uint64_t pc = env->trace_pc;
        if (env->commit_trace.pc_filter_enabled &&
            (pc < env->commit_trace.pc_lo || pc > env->commit_trace.pc_hi)) {
            emit_file = false;
        }
    }

    if (emit_file) {
        const uint64_t pc = env->trace_pc;
        const uint64_t cycle = env->commit_trace.cycle++;
        const uint32_t trap_valid = env->trace_trap_valid;
        const uint32_t trap_cause = env->trace_trap_cause;
        const uint32_t dst_valid = env->trace_wb_valid ? 1u : env->trace_dst_valid;
        const uint32_t dst_reg = env->trace_wb_valid ? env->trace_wb_rd : env->trace_dst_reg;
        const uint64_t dst_data = env->trace_wb_valid ? env->trace_wb_data : env->trace_dst_data;
        const uint8_t trapnum = (uint8_t)(trap_cause & 0xffu);
        const uint32_t cause = (uint32_t)((trap_cause >> 8) & 0xffu);
        const bool argv = trap_valid != 0; /* commit-trace: treat TRAPARG0 as present when trap_valid */
        const uint64_t trapno_full = trap_valid ? linx_trapno_make(true, argv, cause, trapnum) : 0;
        const uint32_t len_meta = linx_trace_len_to_meta_len(env->trace_len);
        const LinxOpcodeMeta *meta = linx_opcode_meta_lookup(env->trace_insn, len_meta);
        uint64_t canonical_insn;
        const char *block_kind;
        int32_t lane_id;

        if (!meta) {
            meta = linx_opcode_meta_lookup(env->trace_insn, 0);
        }
        canonical_insn = linx_trace_canonical_insn(env->trace_insn, env->trace_len, meta);
        block_kind = linx_trace_block_kind_name(meta, canonical_insn, env->trace_len);
        lane_id = linx_trace_lane_id_for_kind(block_kind);

        /* Mandatory schema fields (see linxisa/docs/bringup/contracts/trace_schema.md). */
        fprintf(env->commit_trace.fp,
                "{\"cycle\":%" PRIu64
                ",\"pc\":%" PRIu64
                ",\"insn\":%" PRIu64
                ",\"len\":%u"
                ",\"wb_valid\":%u,\"wb_rd\":%u,\"wb_data\":%" PRIu64
                ",\"src0_valid\":%u,\"src0_reg\":%u,\"src0_data\":%" PRIu64
                ",\"src1_valid\":%u,\"src1_reg\":%u,\"src1_data\":%" PRIu64
                ",\"dst_valid\":%u,\"dst_reg\":%u,\"dst_data\":%" PRIu64
                ",\"mem_valid\":%u,\"mem_is_store\":%u,\"mem_addr\":%" PRIu64
                ",\"mem_wdata\":%" PRIu64 ",\"mem_rdata\":%" PRIu64 ",\"mem_size\":%u"
                ",\"trap_valid\":%u,\"trap_cause\":%u"
                ",\"block_kind\":\"%s\",\"lane_id\":%d"
                ",\"tile_meta\":\"\",\"tile_ref_src\":0,\"tile_ref_dst\":0"
                ",\"trapno_full\":%" PRIu64 ",\"traparg0\":%" PRIu64
                ",\"next_pc\":%" PRIu64 "}\n",
                cycle,
                pc,
                canonical_insn,
                env->trace_len,
                env->trace_wb_valid, env->trace_wb_rd, env->trace_wb_data,
                env->trace_src0_valid, env->trace_src0_reg, env->trace_src0_data,
                env->trace_src1_valid, env->trace_src1_reg, env->trace_src1_data,
                dst_valid, dst_reg, dst_data,
                env->trace_mem_valid, env->trace_mem_is_store, env->trace_mem_addr,
                env->trace_mem_wdata, env->trace_mem_rdata, env->trace_mem_size,
                trap_valid, trap_cause,
                block_kind, lane_id,
                trapno_full, env->trace_traparg0,
                next_pc);
        fflush(env->commit_trace.fp);

        if (env->commit_trace.stop_after_commit) {
            fclose(env->commit_trace.fp);
            env->commit_trace.fp = NULL;
            env->commit_trace.enabled = 0;
            env->commit_trace.stop_after_commit = 0;
        }
    }

    if (env->trace_mem_valid) {
        const uint64_t pc = env->trace_pc;

        qemu_log_mask_and_addr(LOG_LINX_MEM, pc,
                               "LinxMem: pc=0x%016" PRIx64 " %s"
                               " addr=0x%016" PRIx64 " size=%u"
                               " data=0x%016" PRIx64 "\n",
                               pc,
                               env->trace_mem_is_store ? "store" : "load ",
                               env->trace_mem_addr, env->trace_mem_size,
                               env->trace_mem_is_store ?
                               env->trace_mem_wdata : env->trace_mem_rdata);
    }

    if (env->cosim.active) {
        linx_cosim_send_commit_and_wait_ack(env, next_pc);
    }
    if (linx_minst_trace_active(env)) {
        linx_emit_minst_trace(env, next_pc);
    }
}

/*
 * CSTATE (bring-up encoding).
 *
 * The privileged architecture describes CSTATE as a packed state register
 * (ACR, interrupt enable, flags, ...). For QEMU bring-up, model only:
 *   - CSTATE.ACR: bits[3:0]  (current Access Control Ring)
 *   - CSTATE.I:   bit[4]     (interrupt enable for same-ring interrupts)
 *
 * All other bits are preserved on writes but are otherwise ignored.
 */
#define LINX_CSTATE_ACR_MASK 0xFULL
#define LINX_CSTATE_I_BIT    (1ULL << 4)

static inline uint64_t linx_cstate_set_acr(uint64_t cstate, uint32_t acr)
{
    return (cstate & ~LINX_CSTATE_ACR_MASK) | ((uint64_t)acr & LINX_CSTATE_ACR_MASK);
}

static inline uint32_t linx_cstate_get_acr(uint64_t cstate)
{
    return (uint32_t)(cstate & LINX_CSTATE_ACR_MASK);
}

static inline bool linx_irq_allowed(const CPULinxState *env, uint32_t dst_acr)
{
    const uint32_t cur_acr = env->acr & 0xF;
    const uint64_t cstate = env->ssr[LINX_SSR_CSTATE];
    const bool ie = (cstate & LINX_CSTATE_I_BIT) != 0;

    /*
     * v0.2 bring-up profile: if an interrupt routes to a more privileged ACR, it may
     * preempt regardless of the current ring's I bit. If it routes to the
     * current ACR, it is gated by CSTATE.I.
     */
    if (dst_acr < cur_acr) {
        return true;
    }
    if (dst_acr == cur_acr) {
        return ie;
    }
    /* Less-privileged target interrupts are not modeled (bring-up). */
    return ie;
}

static inline void linx_irq_kick_if_allowed(CPULinxState *env, uint32_t dst_acr)
{
    CPUState *cs = env_cpu(env);
    if (env->ssr_acr[dst_acr][LINX_SSR_IPENDING] == 0) {
        return;
    }
    /*
     * Latch CPU_INTERRUPT_HARD whenever a source is pending.
     *
     * Delivery permission (CSTATE.I / ring checks) is enforced later in
     * cpu_exec_interrupt(). Keeping the request latched avoids losing pending
     * IRQs across ACR transitions where permission flips after trap return.
     */
    generic_handle_interrupt(cs, CPU_INTERRUPT_HARD);
}

/* ACRC request_type values (v0.2 bring-up profile). */
enum {
    LINX_SCT_MAC = 0,
    LINX_SCT_SYS = 1,
    LINX_SCT_SEC = 2,
};

static inline uint32_t linx_ssr_low12(uint32_t ssrid)
{
    return ssrid & 0xfffu;
}

static inline bool linx_ssr_is_manager_idx(uint32_t idx)
{
    return (idx & 0xf00u) == 0xf00u;
}

static inline uint32_t linx_ssr_manager_bank(CPULinxState *env, uint32_t ssrid)
{
    const uint32_t encoded_bank = (ssrid >> 12) & 0xFu;
    /*
     * Historical Linx Linux bring-up still relies on base manager-SSR forms
     * (0x0fxx) defaulting to the current managing ACR when the high nibble is
     * omitted. Keep explicit HL bank selectors (0x1fxx, 0x2fxx, ...) intact.
     */
    return encoded_bank != 0 ? encoded_bank : (env->acr & 0xFu);
}

static inline void linx_raise_illegal_inst(CPULinxState *env)
{
    env->pending_trap_arg0 = 0;
    env->pending_trap_cause = 0;
    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
}

static inline bool linx_legacy_trapsave_alias_read(CPULinxState *env, uint32_t idx,
                                                   uint32_t bank, uint64_t *value)
{
    switch (idx) {
    case 0xF0B:
        *value = env->ssr_acr[bank][LINX_SSR_EBARG_BPC_CUR];
        return true;
    case 0xF0C:
        *value = env->ssr_acr[bank][LINX_SSR_EBARG0];
        return true;
    case 0xF0D:
        *value = env->ssr_acr[bank][LINX_SSR_EBARG_TPC];
        return true;
    case 0xF0E:
        *value = env->ssr_acr[bank][LINX_SSR_EBARG_BPC_TGT];
        return true;
    default:
        return false;
    }
}

static inline bool linx_legacy_trapsave_alias_write(CPULinxState *env, uint32_t idx,
                                                    uint32_t bank, uint64_t value)
{
    switch (idx) {
    case 0xF0B:
        env->ssr_acr[bank][LINX_SSR_EBARG_BPC_CUR] = value;
        return true;
    case 0xF0C:
        env->ssr_acr[bank][LINX_SSR_EBARG0] = value;
        return true;
    case 0xF0D:
        env->ssr_acr[bank][LINX_SSR_EBARG_TPC] = value;
        return true;
    case 0xF0E:
        env->ssr_acr[bank][LINX_SSR_EBARG_BPC_TGT] = value;
        return true;
    default:
        return false;
    }
}

uint64_t HELPER(linx_ssr_read)(CPULinxState *env, uint32_t ssrid)
{
    uint32_t idx = linx_ssr_low12(ssrid);
    const bool is_manager = linx_ssr_is_manager_idx(idx);
    const uint32_t bank = is_manager ? linx_ssr_manager_bank(env, ssrid) : 0u;
    uint64_t value;

    switch (idx) {
    case LINX_SSR_CYCLE:
        /* Bring-up: model CYCLE as the dynamic instruction counter. */
        value = env->insn_count;
        break;
    case LINX_SSR_TIME:
        /* Virtual time in nanoseconds. */
        value = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;
    default:
        if (is_manager) {
            if (linx_legacy_trapsave_alias_read(env, idx, bank, &value)) {
                break;
            }
            if (idx == LINX_SSR_TIMER_TIME) {
                value = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                break;
            }
            if (idx == LINX_SSR_DBGID) {
                const uint64_t cps_minus1 = 0; /* CPs=1 */
                const uint64_t bps_minus1 = 3; /* BPs=4 */
                const uint64_t wps_minus1 = 3; /* WPs=4 */
                value = (cps_minus1 << 0) | (bps_minus1 << 4) | (wps_minus1 << 8);
                break;
            }
            if (bank < LINX_ACR_COUNT) {
                value = env->ssr_acr[bank][idx];
                break;
            }
            value = 0;
            break;
        }
        value = env->ssr[idx];
        break;
    }

    if (linx_debug_local_enabled_p() &&
        (idx == LINX_SSR_TIME || idx == LINX_SSR_CYCLE || idx == LINX_SSR_TIMER_TIME)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx ssr read pc=0x%" PRIx64 " ssrid=0x%x bank=%u val=0x%" PRIx64 "\n",
                      env->pc, ssrid, bank, value);
    }
    linx_tp_trace_init();
    if (linx_tp_trace_reads_enabled) {
        linx_tp_trace_emit(env, "ssr_read", ssrid, bank, 0, value);
    }
    if ((env->pc >= 0x10bc0 && env->pc < 0x10c40) ||
        (env->pc >= 0xffffffff80010bc0ULL && env->pc < 0xffffffff80010c40ULL)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: ssr_read pc=0x%" PRIx64 " ssrid=0x%x bank=%u value=0x%" PRIx64 "\n",
                      env->pc, ssrid, bank, value);
    }
    return value;
}

uint64_t HELPER(linx_scalar_read_reg)(CPULinxState *env, uint32_t code)
{
    if (code == LINX_REG_ZERO) {
        return 0;
    }
    if (code < LINX_GPR_COUNT) {
        return env->gpr[code];
    }
    if (code < 28u) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx scalar read pc=0x%" PRIx64 " code=%u tq[%u]=0x%" PRIx64 "\n",
                          env->pc, code, code - 24u, env->tq[code - 24u]);
        }
        return env->tq[code - 24u];
    }
    if (code < 32u) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx scalar read pc=0x%" PRIx64 " code=%u uq[%u]=0x%" PRIx64 "\n",
                          env->pc, code, code - 28u, env->uq[code - 28u]);
        }
        return env->uq[code - 28u];
    }
    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
    return 0;
}

void HELPER(linx_scalar_write_reg)(CPULinxState *env, uint32_t code, uint64_t value)
{
    if (code == LINX_REG_ZERO) {
        return;
    }
    if (code < LINX_GPR_COUNT) {
        env->gpr[code] = value;
        return;
    }
    if (code < 28u) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx scalar write pc=0x%" PRIx64 " code=%u tq[%u]=0x%" PRIx64 "\n",
                          env->pc, code, code - 24u, value);
        }
        env->tq[code - 24u] = value;
        return;
    }
    if (code < 32u) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx scalar write pc=0x%" PRIx64 " code=%u uq[%u]=0x%" PRIx64 "\n",
                          env->pc, code, code - 28u, value);
        }
        env->uq[code - 28u] = value;
        return;
    }
    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
}

uint64_t HELPER(linx_scalar_addi)(CPULinxState *env, uint32_t code, uint64_t imm)
{
    return HELPER(linx_scalar_read_reg)(env, code) + imm;
}

void HELPER(linx_tq_push)(CPULinxState *env, uint64_t value)
{
    if (linx_debug_local_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx tq push pc=0x%" PRIx64 " val=0x%" PRIx64
                      " before=[0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 "]\n",
                      env->pc, value, env->tq[0], env->tq[1], env->tq[2], env->tq[3]);
    }
    env->tq[3] = env->tq[2];
    env->tq[2] = env->tq[1];
    env->tq[1] = env->tq[0];
    env->tq[0] = value;
    if (linx_debug_local_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx tq push after  pc=0x%" PRIx64
                      " tq=[0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 "]\n",
                      env->pc, env->tq[0], env->tq[1], env->tq[2], env->tq[3]);
    }
}

void HELPER(linx_uq_push)(CPULinxState *env, uint64_t value)
{
    if (linx_debug_local_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx uq push pc=0x%" PRIx64 " val=0x%" PRIx64
                      " before=[0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 "]\n",
                      env->pc, value, env->uq[0], env->uq[1], env->uq[2], env->uq[3]);
    }
    env->uq[3] = env->uq[2];
    env->uq[2] = env->uq[1];
    env->uq[1] = env->uq[0];
    env->uq[0] = value;
    if (linx_debug_local_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx uq push after  pc=0x%" PRIx64
                      " uq=[0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 "]\n",
                      env->pc, env->uq[0], env->uq[1], env->uq[2], env->uq[3]);
    }
}

void HELPER(linx_ssr_write)(CPULinxState *env, uint32_t ssrid, uint64_t value)
{
    uint32_t idx = linx_ssr_low12(ssrid);
    const bool is_manager = linx_ssr_is_manager_idx(idx);
    const uint32_t bank = is_manager ? linx_ssr_manager_bank(env, ssrid) : 0u;
    if ((env->pc >= 0x10bc0 && env->pc < 0x10c40) ||
        (env->pc >= 0xffffffff80010bc0ULL && env->pc < 0xffffffff80010c40ULL)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: ssr_write pc=0x%" PRIx64 " ssrid=0x%x bank=%u value=0x%" PRIx64 "\n",
                      env->pc, ssrid, bank, value);
    }

    switch (idx) {
    case LINX_SSR_CYCLE:
    case LINX_SSR_TIME:
        /* Read-only for now. Ignore writes. */
        return;
    case LINX_SSR_CSTATE:
        /*
         * Track ACR in both env->acr and CSTATE.ACR. If software enables
         * interrupts and there is a pending interrupt for the external
         * interrupt routing ring (ACR1),
         * kick the CPU so it can be taken.
         */
        env->ssr[idx] = value;
        env->acr = linx_cstate_get_acr(value);
        linx_refresh_tb_dbg_active(env);
        linx_irq_kick_if_allowed(env, 1);
        return;
    default:
        if (idx == LINX_SSR_CW && env->pc >= 0xffffffff80000000ULL) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: scratch ssr_write pc=0x%" PRIx64
                          " ssrid=0x%x value=0x%" PRIx64 "\n",
                          env->pc, ssrid, value);
        }
        if (is_manager) {
            if (bank >= LINX_ACR_COUNT) {
                return;
            }

            if (linx_legacy_trapsave_alias_write(env, idx, bank, value)) {
                return;
            }
            if (idx == LINX_SSR_DBGID) {
                env->pending_trap_arg0 = 0;
                env->pending_trap_cause = 0;
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }

            switch (idx) {
            case LINX_SSR_TTBR0:
            case LINX_SSR_TTBR1:
            case LINX_SSR_TCR:
            case LINX_SSR_MAIR:
            case LINX_SSR_IOTTBR:
            case LINX_SSR_IOTCR:
            case LINX_SSR_IOMAIR:
                trace_linx_mmu_ssr_write(ssrid, bank, idx, value);
                break;
            default:
                break;
            }

            if (bank == 1) {
                /*
                 * ACR1 privileged MMU/IOMMU programming registers: validate the
                 * v0.2 bring-up subset and flush translations on updates.
                 */
                if (idx == LINX_SSR_TCR) {
                    const uint64_t allowed =
                        (1ull << 0) | (0x3full << 1) | (0x3full << 7) |
                        (1ull << 13) | (1ull << 14) | (1ull << 15);
                    if ((value & ~allowed) != 0) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "Linx: illegal TCR write ssrid=0x%x bank=%u value=0x%" PRIx64 "\n",
                                      ssrid, bank, value);
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    tlb_flush(env_cpu(env));
                    linx_mmu_cache_flush(env);
                    linx_bstart_cache_reset(env);
                    return;
                }
                if (idx == LINX_SSR_IOTCR) {
                    const uint64_t allowed = (1ull << 0) | (0x3full << 1);
                    if ((value & ~allowed) != 0) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "Linx: illegal IOTCR write ssrid=0x%x bank=%u value=0x%" PRIx64 "\n",
                                      ssrid, bank, value);
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    return;
                }
                if (idx == LINX_SSR_TTBR0 || idx == LINX_SSR_TTBR1 || idx == LINX_SSR_IOTTBR) {
                    const bool raw_ttbr = (value & 0xfffu) == 0;
                    const bool legacy_ttbr0 = idx == LINX_SSR_TTBR0 && (value & 0x3u) == 0;
                    const bool legacy_mmconfig =
                        idx == LINX_SSR_TTBR1 &&
                        (value & ~(LINX_LEGACY_MMCONFIG_MODE_MASK |
                                   LINX_LEGACY_MMCONFIG_Q_BIT |
                                   LINX_LEGACY_MMCONFIG_ENABLE_BIT)) == 0;
                    if (!raw_ttbr && !legacy_ttbr0 && !legacy_mmconfig) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "Linx: illegal TTBR write ssrid=0x%x bank=%u value=0x%" PRIx64 "\n",
                                      ssrid, bank, value);
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    tlb_flush(env_cpu(env));
                    linx_mmu_cache_flush(env);
                    linx_bstart_cache_reset(env);
                    return;
                }
            }

            if (idx == LINX_SSR_EOIEI) {
                /*
                 * End of interrupt (v0.2 bring-up profile): clear the pending bit for the
                 * given interrupt ID.
                 *
                 * Keep line level and pending latch separate:
                 * - IPENDING is software-cleared via EOIEI.
                 * - irq_level_acr[] reflects current external line level.
                 *
                 * If a level source is still asserted when EOIEI executes,
                 * immediately re-pend it so completion interrupts cannot be
                 * lost due to short deassert/reassert windows.
                 */
                CPUState *cs = env_cpu(env);
                const uint32_t irq_id = (uint32_t)value & 63u;
                const uint64_t bit = (1ull << irq_id);
                const uint64_t before = env->ssr_acr[bank][LINX_SSR_IPENDING];
                uint64_t after;

                after = before & ~bit;
                if (env->irq_level_acr[bank] & bit) {
                    after |= bit;
                }
                trace_linx_eoiei_write(bank, irq_id, before, after, env->irq_level_acr[bank]);
                env->ssr_acr[bank][LINX_SSR_IPENDING] = after;

                if (env->ssr_acr[bank][LINX_SSR_IPENDING] == 0) {
                    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
                } else {
                    linx_irq_kick_if_allowed(env, bank);
                }
                return;
            }

            if (idx == LINX_SSR_TIMER_TIMECMP) {
                /*
                 * Virtual timer compare (bring-up).
                 *
                 * If TIMECMP is non-zero, schedule a virtual timer interrupt at
                 * that absolute virtual time (ns). If TIMECMP is zero, cancel.
                 */
                const uint64_t now = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                trace_linx_timer_timecmp_write(bank, value, now);
                env->ssr_acr[bank][idx] = value;

                if (bank == 1 && env->timer) {
                    CPUState *cs = env_cpu(env);
                    if (value == 0) {
                        timer_del(env->timer);
                        env->ssr_acr[1][LINX_SSR_IPENDING] &= ~(1ull << LINX_IRQ_TIMER0);
                        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
                        return;
                    }

                    if (value <= now) {
                        env->ssr_acr[1][LINX_SSR_IPENDING] |= (1ull << LINX_IRQ_TIMER0);
                        linx_irq_kick_if_allowed(env, 1);
                        return;
                    }
                    trace_linx_timer_schedule(now, value);
                    timer_mod_ns(env->timer, (int64_t)value);
                }
                return;
            }

            /* Debug SSR validation (v0.2 bring-up subset). */
            if ((idx >= 0xF90 && idx <= 0xF97) || /* DBCR/DBVR[0..3] */
                (idx >= 0xFA0 && idx <= 0xFA1) || /* DCCR/DCVR[0] */
                (idx >= 0xFB0 && idx <= 0xFB7)    /* DWCR/DWVR[0..3] */
                ) {
                const bool is_ctrl = ((idx & 1u) == 0);
                if (is_ctrl) {
                    const uint64_t E = (value >> 0) & 1u;
                    const uint64_t MT = (value >> 1) & 1u;
                    const uint64_t ML = (value >> 2) & 1u;
                    const uint64_t LE_or_LT = (value >> 3) & 1u;
                    const uint64_t ls = (value >> 4) & 3u;
                    const uint64_t mln = (value >> 51) & 0xFu;
                    const uint64_t mask = (value >> 55) & 0x1Fu;

                    (void)E;
                    (void)mask;

                    /* Only Address Match / Context Match is implemented: MT must be 0. */
                    if (MT != 0) {
                        linx_raise_illegal_inst(env);
                    }

                    if (idx >= 0xF90 && idx <= 0xF97) {
                        /* DBCR<n>: allow only defined bits; ML implies MLN in range (CP0 only). */
                        const uint64_t allowed =
                            (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) |
                            (0xFull << 51) | (0x1Full << 55);
                        if ((value & ~allowed) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                        if (ML && mln != 0) {
                            linx_raise_illegal_inst(env);
                        }
                    } else if (idx >= 0xFA0 && idx <= 0xFA1) {
                        /* DCCR0: only support LC match profile (MC=0, CT=0). */
                        const uint64_t allowed =
                            (0x3ull << 6) | (0x3ull << 4) | (1ull << 3) | (1ull << 1) | (1ull << 0);
                        if ((value & ~allowed) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                        if (((value >> 6) & 0x3u) != 0 || ((value >> 4) & 0x3u) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                    } else {
                        /* DWCR<n>: require context linking only when ML=1; validate reserved bits. */
                        const uint64_t allowed =
                            (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) |
                            (0x3ull << 4) |
                            (0xFull << 51) | (0x1Full << 55);
                        if ((value & ~allowed) != 0) {
                            linx_raise_illegal_inst(env);
                        }
                        if (ML) {
                            const uint64_t LT = LE_or_LT;
                            if (LT != 1 || mln != 0) {
                                linx_raise_illegal_inst(env);
                            }
                        }
                        /* LS is only advisory in bring-up; accept any encoding (including 0). */
                        (void)ls;
                    }
                }
            }

            linx_tp_trace_emit(env, "ssr_write", ssrid, bank,
                               env->ssr_acr[bank][idx], value);
            env->ssr_acr[bank][idx] = value;
            if (linx_ssr_idx_is_debug(idx)) {
                linx_refresh_tb_dbg_active(env);
            }
            return;
        }
        linx_tp_trace_emit(env, "ssr_write", ssrid, bank,
                           env->ssr[idx], value);
        env->ssr[idx] = value;
        return;
    }
}

uint64_t HELPER(linx_ssr_swap)(CPULinxState *env, uint32_t ssrid, uint64_t value)
{
    const uint32_t idx = linx_ssr_low12(ssrid);
    const bool is_manager = linx_ssr_is_manager_idx(idx);
    const uint32_t bank = is_manager ? linx_ssr_manager_bank(env, ssrid) : 0u;
    if ((env->pc >= 0x10bc0 && env->pc < 0x10be8) ||
        (env->pc >= 0xffffffff80010bc0ULL && env->pc < 0xffffffff80010be8ULL)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: ssrswap pc=0x%" PRIx64 " ssrid=0x%x value=0x%" PRIx64 "\n",
                      env->pc, ssrid, value);
    }
    uint64_t old = HELPER(linx_ssr_read)(env, ssrid);
    if (idx == LINX_SSR_ETEMP &&
        env->pc >= 0xffffffff800078e4ULL && env->pc < 0xffffffff80007940ULL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: etemp swap pc=0x%" PRIx64 " ssrid=0x%x bank=%u"
                      " old=0x%" PRIx64 " new=0x%" PRIx64
                      " tp=0x%" PRIx64 " acr=%u\n",
                      env->pc, ssrid, bank, old, value,
                      env->ssr[LINX_SSR_TP], env->acr & 0xFu);
    }
    linx_tp_trace_emit(env, "ssr_swap", ssrid, bank, old, value);
    HELPER(linx_ssr_write)(env, ssrid, value);
    return old;
}

void HELPER(linx_tlb_iall)(CPULinxState *env, uint64_t pc)
{
    linx_tlb_trace_emit(env, "iall", pc, 0, false);
    linx_tlb_stats_record(env, LINX_TLB_INV_IALL, pc, 0);
    tlb_flush(env_cpu(env));
    linx_mmu_cache_flush(env);
    linx_bstart_cache_reset(env);
}

void HELPER(linx_tlb_ia)(CPULinxState *env, uint64_t asid, uint64_t pc)
{
    linx_tlb_trace_emit(env, "ia", pc, asid, true);
    linx_tlb_stats_record(env, LINX_TLB_INV_IA, pc, asid);
    /*
     * QEMU's current Linx TLB model is not ASID-tagged independently from
     * TTBR/MMU-index state, so keep TLB.IA as a conservative local full flush.
     */
    tlb_flush(env_cpu(env));
    linx_mmu_cache_flush(env);
    linx_bstart_cache_reset(env);
}

void HELPER(linx_tlb_iv)(CPULinxState *env, uint64_t addr, uint64_t pc)
{
    linx_tlb_trace_emit(env, "iv", pc, addr, true);
    linx_tlb_stats_record(env, LINX_TLB_INV_IV, pc, addr);
    tlb_flush_page(env_cpu(env), (vaddr)addr);
    linx_mmu_cache_flush_page(env, addr);
    linx_bstart_cache_reset_page(env, addr);
}

void HELPER(linx_tlb_iav)(CPULinxState *env, uint64_t packed, uint64_t pc)
{
    const uint64_t addr = packed & ((UINT64_C(1) << 44) - 1);

    linx_tlb_trace_emit(env, "iav", pc, packed, true);
    linx_tlb_stats_record(env, LINX_TLB_INV_IAV, pc, packed);
    tlb_flush_page(env_cpu(env), (vaddr)addr);
    linx_mmu_cache_flush_page(env, addr);
    linx_bstart_cache_reset_page(env, addr);
}

/* ------------------------------------------------------------------------- */
/* Debug helpers (v0.2 bring-up subset)                                      */
/* ------------------------------------------------------------------------- */

static inline bool linx_dbg_addr_match(uint64_t a, uint64_t b, uint32_t mask_bits)
{
    if (mask_bits >= 63) {
        return true;
    }
    const uint64_t m = (mask_bits == 0) ? 0 : ((1ull << mask_bits) - 1ull);
    return (a & ~m) == (b & ~m);
}

static inline bool linx_dbg_ctx_match(CPULinxState *env, uint32_t acr, uint32_t cp_idx)
{
    if (cp_idx != 0) {
        return false;
    }
    const uint64_t dccr = env->ssr_acr[acr][LINX_SSR_DCCR0];
    const uint64_t dcvr = env->ssr_acr[acr][LINX_SSR_DCVR0];
    const uint64_t E = (dccr >> 0) & 1u;
    const uint64_t MT = (dccr >> 1) & 1u;
    if (!E || MT != 0) {
        return false;
    }
    const uint64_t lc0 = (dcvr >> 0) & 0xffffu;
    const uint64_t lc1 = (dcvr >> 16) & 0xffffu;
    const uint64_t lc2 = (dcvr >> 32) & 0xffffu;
    return ((env->lc[0] & 0xffffu) == lc0) &&
           ((env->lc[1] & 0xffffu) == lc1) &&
           ((env->lc[2] & 0xffffu) == lc2);
}

void HELPER(linx_dbg_check_pc)(CPULinxState *env, uint64_t pc)
{
    CPUState *cs = env_cpu(env);
    const uint32_t acr = env->acr & 0xFu;

    for (uint32_t n = 0; n < 4; n++) {
        const uint32_t cr_idx = LINX_SSR_DBCR0 + 2u * n;
        const uint32_t vr_idx = LINX_SSR_DBVR0 + 2u * n;
        const uint64_t cr = env->ssr_acr[acr][cr_idx];
        const uint64_t E = (cr >> 0) & 1u;
        if (!E) {
            continue;
        }
        const uint64_t MT = (cr >> 1) & 1u;
        if (MT != 0) {
            continue;
        }
        const uint64_t ML = (cr >> 2) & 1u;
        const uint64_t LE = (cr >> 3) & 1u;
        const uint32_t mln = (uint32_t)((cr >> 51) & 0xFu);
        const uint32_t mask = (uint32_t)((cr >> 55) & 0x1Fu);
        (void)LE;

        const uint64_t vr = env->ssr_acr[acr][vr_idx];
        if (!linx_dbg_addr_match(pc, vr, mask)) {
            continue;
        }

        if (ML) {
            if (!linx_dbg_ctx_match(env, acr, mln)) {
                continue;
            }
        }

        env->pending_trap_arg0 = pc;
        env->pending_trap_cause = n & 0xFu;
        cs->exception_index = LINX_EXCP_HW_BREAKPOINT;
        cpu_loop_exit_restore(cs, GETPC());
    }
}

static inline void linx_dbg_check_mem(CPULinxState *env, uint64_t pc,
                                      uint64_t addr, uint32_t size,
                                      bool is_store)
{
    CPUState *cs = env_cpu(env);
    const uint32_t acr = env->acr & 0xFu;
    (void)size;

    for (uint32_t n = 0; n < 4; n++) {
        const uint32_t cr_idx = LINX_SSR_DWCR0 + 2u * n;
        const uint32_t vr_idx = LINX_SSR_DWVR0 + 2u * n;
        const uint64_t cr = env->ssr_acr[acr][cr_idx];
        const uint64_t E = (cr >> 0) & 1u;
        if (!E) {
            continue;
        }
        const uint64_t MT = (cr >> 1) & 1u;
        if (MT != 0) {
            continue;
        }
        const uint64_t ML = (cr >> 2) & 1u;
        const uint64_t LT = (cr >> 3) & 1u;
        const uint32_t ls = (uint32_t)((cr >> 4) & 0x3u);
        const uint32_t mln = (uint32_t)((cr >> 51) & 0xFu);
        const uint32_t mask = (uint32_t)((cr >> 55) & 0x1Fu);

        const bool allow = (ls == 0) ? true :
                           (ls == 1) ? !is_store :
                           (ls == 2) ? is_store :
                           true;
        if (!allow) {
            continue;
        }

        const uint64_t vr = env->ssr_acr[acr][vr_idx];
        if (!linx_dbg_addr_match(addr, vr, mask)) {
            continue;
        }

        if (ML) {
            if (LT != 1 || !linx_dbg_ctx_match(env, acr, mln)) {
                continue;
            }
        }

        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = n & 0xFu;
        trace_linx_debug_watchpoint_hit(pc, addr,
                                        is_store ? BP_MEM_WRITE : BP_MEM_READ);
        cs->exception_index = LINX_EXCP_HW_WATCHPOINT;
        cpu_loop_exit_restore(cs, GETPC());
    }
}

void HELPER(linx_dbg_check_load)(CPULinxState *env, uint64_t pc, uint64_t addr, uint32_t size)
{
    linx_dbg_check_mem(env, pc, addr, size, false);
}

void HELPER(linx_dbg_check_store)(CPULinxState *env, uint64_t pc, uint64_t addr, uint32_t size)
{
    linx_dbg_check_mem(env, pc, addr, size, true);
}

/* ------------------------------------------------------------------------- */
/* Privilege transitions (bring-up)                                          */
/* ------------------------------------------------------------------------- */

void HELPER(linx_service_request)(CPULinxState *env, uint32_t request_type,
                                  uint64_t bpc, uint64_t tpc, uint64_t pc_next)
{
    CPUState *cs = env_cpu(env);
    const uint32_t src_acr = env->acr & 0xFu;
    uint32_t dst_acr = 0;
    uint64_t src_cstate = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], src_acr);
    /* v0.2: ACRC traps are always reported as block-body traps. */
    src_cstate |= LINX_ECSTATE_BI_BIT;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: SERVICE_REQUEST src_acr=%u req=%u bpc=0x%" PRIx64 " tpc=0x%" PRIx64
                  " pc_next=0x%" PRIx64 "\n",
                  src_acr, request_type, bpc, tpc, pc_next);
    trace_linx_service_request(src_acr, request_type, bpc, tpc, pc_next);

    /* ACRC request_type validity + routing (bring-up profile; see linxisa manual). */
    if (src_acr == 1) {
        if (request_type != LINX_SCT_MAC && request_type != LINX_SCT_SEC) {
            cs->exception_index = LINX_EXCP_ILLEGAL_INST;
            cpu_loop_exit(cs);
        }
        dst_acr = 0;
    } else if (src_acr == 2) {
        if (request_type != LINX_SCT_MAC && request_type != LINX_SCT_SYS && request_type != LINX_SCT_SEC) {
            cs->exception_index = LINX_EXCP_ILLEGAL_INST;
            cpu_loop_exit(cs);
        }
        /* v0.2 bring-up: ACR2 + SCT_SYS routes to ACR1; others route to ACR0. */
        dst_acr = (request_type == LINX_SCT_SYS) ? 1 : 0;
    } else {
        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
        cpu_loop_exit(cs);
    }

    if (request_type == LINX_SCT_SYS) {
        linx_syscall_trace_maybe_emit(env, src_acr, dst_acr, bpc, tpc, pc_next);
    }

    /*
     * Preserve block/queue state for the trapped ACR so we can resume the
     * interrupted block after returning via ACRE. Without this, the kernel's
     * own block headers clobber the user's commit metadata (brtype/tgt/cond)
     * and hand queues, breaking post-syscall control flow and any mid-block
     * trap return.
     */
    linx_acr_save_block_state(env, src_acr);
    const LinxAcrBlockState *src_state = &env->acr_block_state[src_acr];
    linx_acr_restore_block_state(env, dst_acr);

    /* Save trap state into the managing ACR bank (v0.2: EBARG + TRAPNO). */
    env->ssr_acr[dst_acr][LINX_SSR_ECSTATE] = src_cstate;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG0] = (uint64_t)(src_state->blocktype & 0x1fu);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_CUR] = bpc;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_BPC_TGT] = pc_next;
    /* v0.2: ACRC resume PC is the following instruction (bring-up: explicit BSTOP). */
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TPC] = pc_next;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LRA] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ0] = src_state->tq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ1] = src_state->tq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ2] = src_state->tq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_TQ3] = src_state->tq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ0] = src_state->uq[0];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ1] = src_state->uq[1];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ2] = src_state->uq[2];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_UQ3] = src_state->uq[3];
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LB] =
        ((src_state->lb[0] & 0xffffu) << 0) | ((src_state->lb[1] & 0xffffu) << 16) |
        ((src_state->lb[2] & 0xffffu) << 32);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_LC] =
        ((src_state->lc[0] & 0xffffu) << 0) | ((src_state->lc[1] & 0xffffu) << 16) |
        ((src_state->lc[2] & 0xffffu) << 32);
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_PTR] = 0;
    env->ssr_acr[dst_acr][LINX_SSR_EBARG_EXT_META] = 0;

    /* Trap reporting (v0.2 bring-up encoding). */
    env->ssr_acr[dst_acr][LINX_SSR_TRAPNO] =
        linx_trapno_make(true, true, (uint32_t)request_type, 16 /* SYSCALL */);
    env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = (uint64_t)request_type;

    /*
     * Linux user entry expects live SSR_TP and manager ETEMP to hold
     * thread_info during the first save blocks. Preserve the interrupted user
     * TLS pointer in ETEMP0 so the kernel can restore PT_TP on ACRE.
     */
    if (src_acr == 2 && dst_acr == 1) {
        const uint64_t user_tp = env->ssr[LINX_SSR_TP];
        const uint64_t thread_info = env->ssr_acr[dst_acr][LINX_SSR_ETEMP];

        env->ssr[LINX_SSR_TP] = thread_info;
        env->ssr_acr[dst_acr][LINX_SSR_ETEMP0] = user_tp;
        linx_tp_trace_emit_handoff(env, "service_user_to_kernel",
                                   src_acr, dst_acr, user_tp, thread_info);
    }

    /* Disable interrupts and switch to managing ring, then vector to EVBASE. */
    env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
    env->acr = dst_acr;
    linx_refresh_tb_dbg_active(env);
    env->ssr[LINX_SSR_CSTATE] = linx_cstate_set_acr(env->ssr[LINX_SSR_CSTATE], dst_acr);
    const uint64_t evbase = env->ssr_acr[dst_acr][LINX_SSR_EVBASE];
    env->pc = evbase ? evbase : tpc;

    cs->exception_index = -1;
    cpu_loop_exit(cs);
}

void HELPER(linx_acr_enter)(CPULinxState *env, uint32_t rra_type)
{
    CPUState *cs = env_cpu(env);
    const uint32_t mgr = env->acr & 0xFu;
    const uint64_t ecstate = env->ssr_acr[mgr][LINX_SSR_ECSTATE];
    const uint64_t trapno = env->ssr_acr[mgr][LINX_SSR_TRAPNO];
    const uint32_t target = linx_cstate_get_acr(ecstate);
    const bool bi = (ecstate & LINX_ECSTATE_BI_BIT) != 0;
    const uint64_t resume_bpc = env->ssr_acr[mgr][LINX_SSR_EBARG_BPC_CUR];
    const uint64_t resume_tpc = env->ssr_acr[mgr][LINX_SSR_EBARG_TPC];
    const uint64_t resume_pc =
        ((trapno & 0x3fu) == LINX_TRAPNUM_BREAKPOINT_EXP) ? resume_bpc :
        (bi ? resume_tpc : resume_bpc);
    linx_acre_trace_maybe_emit(env, "entry", mgr, target, rra_type, bi,
                               trapno, ecstate, resume_pc, resume_bpc,
                               resume_tpc);
    if (linx_debug_acre_stderr_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: acre return mgr=%u target=%u bi=%u trapno=0x%" PRIx64
                      " ecstate=0x%" PRIx64
                      " bpc=0x%" PRIx64 " tpc=0x%" PRIx64
                      " resume=0x%" PRIx64 " rra=%u pc_before=0x%" PRIx64 "\n",
                      mgr, target, bi ? 1u : 0u, trapno, ecstate,
                      resume_bpc, resume_tpc, resume_pc, rra_type, env->pc);
    }
    linx_call_trace_emit(env, LINX_CALL_TRACE_ACRE_ENTER, resume_pc,
                         resume_bpc, resume_tpc);
    trace_linx_acr_enter(mgr, target, rra_type, bi ? 1u : 0u,
                         resume_pc, resume_bpc, resume_tpc,
                         env->gpr[LINX_REG_A0], ecstate);
    if (linx_debug_acre_stderr_enabled_p()) {
        fprintf(stderr,
                "linx_acre_enter: mgr=%u target=%u bi=%u trapno=0x%" PRIx64
                " ecstate=0x%" PRIx64 " resume=0x%" PRIx64
                " bpc=0x%" PRIx64 " tpc=0x%" PRIx64
                " rra=%u pc_before=0x%" PRIx64 " ipending1=0x%" PRIx64 "\n",
                mgr, target, bi ? 1u : 0u, trapno, ecstate,
                resume_pc, resume_bpc, resume_tpc, rra_type, env->pc,
                env->ssr_acr[1][LINX_SSR_IPENDING]);
        fflush(stderr);
    }

    /*
     * v0.2 bring-up: ACR_ENTER may keep privilege or drop privilege.
     * Entering a more-privileged ring directly from software is invalid.
     */
    if (target >= LINX_ACR_COUNT || target < mgr) {
        env->pending_trap_arg0 = (uint64_t)target;
        env->pending_trap_cause = 0;
        helper_raise_exception(env, LINX_EXCP_EXEC_STATE_CHECK);
        return;
    }

    /*
     * Trap return / ACR handoff.
     *
     * For transitions across ACRs (mgr != target), save the current block state
     * in the manager bank and restore the target ACR's saved state.
     *
     * For same-ACR returns (mgr == target), do *not* overwrite the interrupted
     * context's saved state. The interrupt/trap entry path already saved the
     * pre-trap block/template state into acr_block_state[mgr]; restoring that
     * state is required to resume an interrupted restartable template without
     * clobbering progress when the handler itself executes template blocks.
     */
    if (target != mgr) {
        linx_acr_save_block_state(env, mgr);
    }
    linx_acr_restore_block_state(env, target);
    if (linx_debug_acre_stderr_enabled_p()) {
        fprintf(stderr,
                "linx_acre_enter: restored target=%u blocktype=%u in_body=%d ebarg_depth=%u\n",
                target, env->blocktype, env->in_body, env->ebarg_stack_depth);
        fflush(stderr);
    }

    /* v0.2 ACRE(RRA) behavior: DEFAULT resets BSTATE; RESTORE uses EBARG snapshot. */
    if (rra_type == 0) {
        int i;
        for (i = 0; i < 4; i++) {
            env->tq[i] = 0;
            env->uq[i] = 0;
        }
        for (i = 0; i < LINX_VEC_QUEUE_DEPTH; i++) {
            env->vtq[i] = 0;
            env->vuq[i] = 0;
            env->vmq[i] = 0;
            env->vnq[i] = 0;
        }
        env->tgt = 0;
        env->cond = 0;
        env->carg = 0;
        env->brtype = 0;
        env->blocktype = 0;
        env->call_ra_set = 0;
        env->call_setret_pending = 0;
        env->vec_p = 0;
        env->body_tpc = 0;
        env->body_end = 0;
        env->return_pc = 0;
        env->in_body = 0;
        env->tmpl_pc = 0;
        env->tmpl_kind = 0;
        env->tmpl_step = 0;
        env->tmpl_reg_cur = 0;
        env->tmpl_reg_begin = 0;
        env->tmpl_reg_end = 0;
        env->tmpl_stacksize = 0;
        env->tmpl_mem_dst = 0;
        env->tmpl_mem_src = 0;
        env->tmpl_mem_remaining = 0;
        env->tmpl_mem_value = 0;
        for (i = 0; i < 3; i++) {
            env->lb[i] = 0;
            env->lc[i] = 0;
        }
    } else if (rra_type == 1) {
        /*
         * RRA_RESTORE: restore the EBARG-carried second-level architectural
         * snapshot.  The target ACR snapshot provides block metadata that is
         * not represented in EBARG, while EBARG itself is the architectural
         * trap-return transport for queue and block argument state.
         *
         * This is required for recoverable BI=1 data faults in the middle of a
         * scalar block: the faulting instruction resumes at EBARG_TPC and must
         * see the T/U queue values captured at exception entry, not a stale or
         * zero target snapshot.
         */
        linx_restore_bstate_from_ebarg(env, mgr);
    } else {
        env->pending_trap_arg0 = (uint64_t)rra_type;
        env->pending_trap_cause = 0;
        helper_raise_exception(env, LINX_EXCP_EXEC_STATE_CHECK);
        return;
    }

    /* v0.2: always restore BPC from EBARG. */
    env->bpc = resume_bpc;

    env->acr = target;
    linx_refresh_tb_dbg_active(env);
    env->ssr[LINX_SSR_CSTATE] = ecstate & ~LINX_ECSTATE_BI_BIT;
    env->pc = resume_pc;
    linx_call_trace_emit(env, LINX_CALL_TRACE_ACRE_STAGED, env->pc,
                         resume_bpc, resume_tpc);
    linx_acre_trace_maybe_emit(env, "staged", mgr, target, rra_type, bi,
                               trapno, ecstate, resume_pc, resume_bpc,
                               resume_tpc);
    if (target == 2 && (trapno & 0x3fu) == 16) {
        linx_syscall_trace_return_maybe_emit(env, mgr, target,
                                             resume_bpc, resume_tpc,
                                             resume_pc);
    }
    linx_tp_trace_emit_handoff(env, "acre_staged", mgr, target,
                               env->ssr[LINX_SSR_TP],
                               env->ssr_acr[target][LINX_SSR_ETEMP]);
    if (linx_debug_acre_stderr_enabled_p()) {
        fprintf(stderr,
                "linx_acre_enter: staged target=%u cstate=0x%" PRIx64
                " pc=0x%" PRIx64 " bpc=0x%" PRIx64
                " sp=0x%" PRIx64 " etemp1=0x%" PRIx64 " tp=0x%" PRIx64 "\n",
                env->acr, env->ssr[LINX_SSR_CSTATE], env->pc, env->bpc,
                env->gpr[LINX_REG_SP],
                env->ssr_acr[1][LINX_SSR_ETEMP], env->ssr[LINX_SSR_TP]);
        if (env->acr == 2) {
            uint8_t buf[8] = {0};
            int rv = cpu_memory_rw_debug(cs, env->pc, buf, sizeof(buf), 0);
            fprintf(stderr,
                    "linx_acre_enter: userpc probe rv=%d bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    rv, buf[0], buf[1], buf[2], buf[3],
                    buf[4], buf[5], buf[6], buf[7]);
        }
        fflush(stderr);
    }
    if (target == mgr) {
        const uint32_t depth_before = env->ebarg_stack_depth;
        if (linx_ebarg_stack_pop_restore(env, mgr)) {
            trace_linx_ebarg_stack_pop(mgr, depth_before, env->ebarg_stack_depth);
        } else {
            trace_linx_ebarg_stack_underflow(mgr, depth_before);
        }
        if (linx_debug_acre_stderr_enabled_p()) {
            fprintf(stderr,
                    "linx_acre_enter: same-acr pop depth_before=%u depth_after=%u\n",
                    depth_before, env->ebarg_stack_depth);
            fflush(stderr);
        }
    }
    /*
     * External IRQs route to ACR1 in the bring-up profile.
     * Re-latch a pending request after privilege/state restore.
     */
    linx_irq_kick_if_allowed(env, 1);
    if (linx_debug_acre_stderr_enabled_p()) {
        fprintf(stderr,
                "linx_acre_enter: post-kick cpu_interrupts=0x%x ipending1=0x%" PRIx64
                " about_to_exit pc=0x%" PRIx64 "\n",
                cs->interrupt_request, env->ssr_acr[1][LINX_SSR_IPENDING], env->pc);
        fflush(stderr);
    }

    cs->exception_index = -1;
    cpu_loop_exit(cs);
}

/* ------------------------------------------------------------------------- */
/* Atomics (LR/SC + fetch-RMW)                                               */
/* ------------------------------------------------------------------------- */

static inline int linx_env_mmu_index(CPULinxState *env)
{
    return ((env->acr & 0xFu) == 2) ? 1 : 0;
}

static inline MemOpIdx linx_oi_le_env(CPULinxState *env, MemOp mop)
{
    return make_memop_idx(mop | MO_LE, linx_env_mmu_index(env));
}

#define linx_oi_le(mop) linx_oi_le_env(env, (mop))

static inline void linx_lr_set(CPULinxState *env, uint64_t addr, uint32_t size)
{
    env->lr_addr = addr;
    env->lr_size = size;
    env->lr_valid = 1;
}

static inline void linx_lr_clear(CPULinxState *env)
{
    env->lr_valid = 0;
}

uint64_t HELPER(linx_lr_w)(CPULinxState *env, uint64_t addr)
{
    uint32_t v = cpu_ldl_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UL), GETPC());
    linx_lr_set(env, addr, 4);
    return (uint64_t)v;
}

uint64_t HELPER(linx_lr_b)(CPULinxState *env, uint64_t addr)
{
    uint32_t v = cpu_ldb_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UB), GETPC());
    linx_lr_set(env, addr, 1);
    return (uint64_t)v;
}

uint64_t HELPER(linx_lr_h)(CPULinxState *env, uint64_t addr)
{
    uint32_t v = cpu_ldw_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UW), GETPC());
    linx_lr_set(env, addr, 2);
    return (uint64_t)v;
}

uint64_t HELPER(linx_lr_d)(CPULinxState *env, uint64_t addr)
{
    uint64_t v = cpu_ldq_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UQ), GETPC());
    linx_lr_set(env, addr, 8);
    return v;
}

uint64_t HELPER(linx_sc_w)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    /*
     * SC.W returns 0 on success, non-zero on failure (bring-up convention).
     * This is a simplified reservation model: any intervening store clears the
     * reservation (via the translator calling linx_lr_clear on stores/atomics).
     */
    uint64_t ok = (env->lr_valid && env->lr_addr == addr && env->lr_size == 4) ? 0 : 1;
    if (ok == 0) {
        cpu_stl_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UL), GETPC());
    }
    linx_lr_clear(env);
    return ok;
}

uint64_t HELPER(linx_sc_b)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    uint64_t ok = (env->lr_valid && env->lr_addr == addr && env->lr_size == 1) ? 0 : 1;
    if (ok == 0) {
        cpu_stb_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UB), GETPC());
    }
    linx_lr_clear(env);
    return ok;
}

uint64_t HELPER(linx_sc_h)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    uint64_t ok = (env->lr_valid && env->lr_addr == addr && env->lr_size == 2) ? 0 : 1;
    if (ok == 0) {
        cpu_stw_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UW), GETPC());
    }
    linx_lr_clear(env);
    return ok;
}

uint64_t HELPER(linx_sc_d)(CPULinxState *env, uint64_t addr, uint64_t value)
{
    uint64_t ok = (env->lr_valid && env->lr_addr == addr && env->lr_size == 8) ? 0 : 1;
    if (ok == 0) {
        cpu_stq_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UQ), GETPC());
    }
    linx_lr_clear(env);
    return ok;
}

uint64_t HELPER(linx_swapw)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    linx_lr_clear(env);
    return (uint64_t)cpu_atomic_xchgl_le_mmu((CPUArchState *)env, addr, value,
                                            linx_oi_le(MO_UL), GETPC());
}

uint64_t HELPER(linx_swapb)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    linx_lr_clear(env);
    return (uint64_t)cpu_atomic_xchgb_mmu((CPUArchState *)env, addr, value,
                                          linx_oi_le(MO_UB), GETPC());
}

uint64_t HELPER(linx_swaph)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    linx_lr_clear(env);
    return (uint64_t)cpu_atomic_xchgw_le_mmu((CPUArchState *)env, addr, value,
                                             linx_oi_le(MO_UW), GETPC());
}

uint64_t HELPER(linx_swapd)(CPULinxState *env, uint64_t addr, uint64_t value)
{
    linx_lr_clear(env);
    return cpu_atomic_xchgq_le_mmu((CPUArchState *)env, addr, value,
                                   linx_oi_le(MO_UQ), GETPC());
}

uint64_t HELPER(linx_lw_add)(CPULinxState *env, uint64_t addr, uint32_t value)
{
    linx_lr_clear(env);
    return (uint64_t)cpu_atomic_fetch_addl_le_mmu((CPUArchState *)env, addr, value,
                                                  linx_oi_le(MO_UL), GETPC());
}

#define LINX_DEFINE_FETCH32_HELPER(NAME, OP, OI) \
uint64_t HELPER(linx_##NAME)(CPULinxState *env, uint64_t addr, uint32_t value) \
{ \
    linx_lr_clear(env); \
    return (uint64_t)cpu_atomic_##OP((CPUArchState *)env, addr, value, OI, GETPC()); \
}

#define LINX_DEFINE_FETCH64_HELPER(NAME, OP, OI) \
uint64_t HELPER(linx_##NAME)(CPULinxState *env, uint64_t addr, uint64_t value) \
{ \
    linx_lr_clear(env); \
    return cpu_atomic_##OP((CPUArchState *)env, addr, value, OI, GETPC()); \
}

#define LINX_DEFINE_STORE32_HELPER(NAME, OP, OI) \
void HELPER(linx_##NAME)(CPULinxState *env, uint64_t addr, uint32_t value) \
{ \
    linx_lr_clear(env); \
    (void)cpu_atomic_##OP((CPUArchState *)env, addr, value, OI, GETPC()); \
}

#define LINX_DEFINE_STORE64_HELPER(NAME, OP, OI) \
void HELPER(linx_##NAME)(CPULinxState *env, uint64_t addr, uint64_t value) \
{ \
    linx_lr_clear(env); \
    (void)cpu_atomic_##OP((CPUArchState *)env, addr, value, OI, GETPC()); \
}

LINX_DEFINE_FETCH32_HELPER(lw_and, fetch_andl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_FETCH32_HELPER(lw_or, fetch_orl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_FETCH32_HELPER(lw_xor, fetch_xorl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_FETCH32_HELPER(lw_smax, fetch_smaxl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_FETCH32_HELPER(lw_smin, fetch_sminl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_FETCH32_HELPER(lw_umax, fetch_umaxl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_FETCH32_HELPER(lw_umin, fetch_uminl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_FETCH64_HELPER(ld_and, fetch_andq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_FETCH64_HELPER(ld_or, fetch_orq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_FETCH64_HELPER(ld_xor, fetch_xorq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_FETCH64_HELPER(ld_smax, fetch_smaxq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_FETCH64_HELPER(ld_smin, fetch_sminq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_FETCH64_HELPER(ld_umax, fetch_umaxq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_FETCH64_HELPER(ld_umin, fetch_uminq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE32_HELPER(sw_add, fetch_addl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE32_HELPER(sw_and, fetch_andl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE32_HELPER(sw_or, fetch_orl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE32_HELPER(sw_xor, fetch_xorl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE32_HELPER(sw_smax, fetch_smaxl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE32_HELPER(sw_smin, fetch_sminl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE32_HELPER(sw_umax, fetch_umaxl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE32_HELPER(sw_umin, fetch_uminl_le_mmu, linx_oi_le(MO_UL))
LINX_DEFINE_STORE64_HELPER(sd_add, fetch_addq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE64_HELPER(sd_and, fetch_andq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE64_HELPER(sd_or, fetch_orq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE64_HELPER(sd_xor, fetch_xorq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE64_HELPER(sd_smax, fetch_smaxq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE64_HELPER(sd_smin, fetch_sminq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE64_HELPER(sd_umax, fetch_umaxq_le_mmu, linx_oi_le(MO_UQ))
LINX_DEFINE_STORE64_HELPER(sd_umin, fetch_uminq_le_mmu, linx_oi_le(MO_UQ))

#undef LINX_DEFINE_FETCH32_HELPER
#undef LINX_DEFINE_FETCH64_HELPER
#undef LINX_DEFINE_STORE32_HELPER
#undef LINX_DEFINE_STORE64_HELPER

uint64_t HELPER(linx_ld_add)(CPULinxState *env, uint64_t addr, uint64_t value)
{
    linx_lr_clear(env);
    if (cpu_in_serial_context(env_cpu(env))) {
        const uint64_t old = cpu_ldq_le_data(env, (abi_ptr)addr);
        cpu_stq_le_data(env, (abi_ptr)addr, old + value);
        return old;
    }
    return cpu_atomic_fetch_addq_le_mmu((CPUArchState *)env, addr, value,
                                        linx_oi_le(MO_UQ), GETPC());
}

uint64_t HELPER(linx_casb)(CPULinxState *env, uint64_t addr, uint32_t cmpv, uint32_t newv)
{
    linx_lr_clear(env);
    if (cpu_in_serial_context(env_cpu(env))) {
        const uint8_t old = cpu_ldub_data(env, (abi_ptr)addr);
        if (old == (uint8_t)cmpv) {
            cpu_stb_data(env, (abi_ptr)addr, (uint8_t)newv);
        }
        return old;
    }
    return (uint64_t)cpu_atomic_cmpxchgb_mmu((CPUArchState *)env, addr,
                                             (uint8_t)cmpv, (uint8_t)newv,
                                             linx_oi_le(MO_UB), GETPC());
}

uint64_t HELPER(linx_cash)(CPULinxState *env, uint64_t addr, uint32_t cmpv, uint32_t newv)
{
    linx_lr_clear(env);
    if (cpu_in_serial_context(env_cpu(env))) {
        const uint16_t old = cpu_lduw_le_data(env, (abi_ptr)addr);
        if (old == (uint16_t)cmpv) {
            cpu_stw_le_data(env, (abi_ptr)addr, (uint16_t)newv);
        }
        return old;
    }
    return (uint64_t)cpu_atomic_cmpxchgw_le_mmu((CPUArchState *)env, addr,
                                                (uint16_t)cmpv, (uint16_t)newv,
                                                linx_oi_le(MO_UW), GETPC());
}

uint64_t HELPER(linx_casw)(CPULinxState *env, uint64_t addr, uint32_t cmpv, uint32_t newv)
{
    linx_lr_clear(env);
    if (cpu_in_serial_context(env_cpu(env))) {
        const uint32_t old = cpu_ldl_le_data(env, (abi_ptr)addr);
        if (old == cmpv) {
            cpu_stl_le_data(env, (abi_ptr)addr, newv);
        }
        return old;
    }
    return (uint64_t)cpu_atomic_cmpxchgl_le_mmu((CPUArchState *)env, addr,
                                                cmpv, newv,
                                                linx_oi_le(MO_UL), GETPC());
}

uint64_t HELPER(linx_casd)(CPULinxState *env, uint64_t addr, uint64_t cmpv, uint64_t newv)
{
    linx_lr_clear(env);
    if (cpu_in_serial_context(env_cpu(env))) {
        const uint64_t old = cpu_ldq_le_data(env, (abi_ptr)addr);
        if (old == cmpv) {
            cpu_stq_le_data(env, (abi_ptr)addr, newv);
        }
        return old;
    }
    return cpu_atomic_cmpxchgq_le_mmu((CPUArchState *)env, addr, cmpv, newv,
                                      linx_oi_le(MO_UQ), GETPC());
}

void HELPER(linx_dma)(CPULinxState *env, uint64_t src, uint64_t dst)
{
    uint8_t buf[64];

    linx_lr_clear(env);
    for (unsigned i = 0; i < sizeof(buf); i++) {
        buf[i] = cpu_ldub_data(env, (abi_ptr)(src + i));
    }
    for (unsigned i = 0; i < sizeof(buf); i++) {
        cpu_stb_data(env, (abi_ptr)(dst + i), buf[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* Floating-point helpers (hard-float bring-up)                              */
/* ------------------------------------------------------------------------- */

/* FCSR bits (as documented in docs/isa-manual): */
#define LINX_FCSR_FFLAGS_MASK 0x1fu
#define LINX_FCSR_FRM_SHIFT   8u
#define LINX_FCSR_FRM_MASK    (0x7u << LINX_FCSR_FRM_SHIFT)

static FloatRoundMode linx_fcsr_rounding_mode(uint32_t fcsr)
{
    switch ((fcsr & LINX_FCSR_FRM_MASK) >> LINX_FCSR_FRM_SHIFT) {
    case 0: /* RNE */
        return float_round_nearest_even;
    case 1: /* RDN */
        return float_round_down;
    case 2: /* RUP */
        return float_round_up;
    case 3: /* RTZ */
        return float_round_to_zero;
    case 4: /* RMM */
        return float_round_ties_away;
    default:
        return float_round_nearest_even;
    }
}

static int linx_fcsr_to_softfloat_flags(uint32_t fcsr)
{
    int flags = 0;
    if (fcsr & (1u << 0)) {
        flags |= float_flag_invalid;
    }
    if (fcsr & (1u << 1)) {
        flags |= float_flag_divbyzero;
    }
    if (fcsr & (1u << 2)) {
        flags |= float_flag_overflow;
    }
    if (fcsr & (1u << 3)) {
        flags |= float_flag_underflow;
    }
    if (fcsr & (1u << 4)) {
        flags |= float_flag_inexact;
    }
    return flags;
}

static uint32_t linx_softfloat_flags_to_fcsr(int flags)
{
    uint32_t fcsr = 0;
    if (flags & float_flag_invalid) {
        fcsr |= (1u << 0);
    }
    if (flags & float_flag_divbyzero) {
        fcsr |= (1u << 1);
    }
    if (flags & float_flag_overflow) {
        fcsr |= (1u << 2);
    }
    if (flags & float_flag_underflow) {
        fcsr |= (1u << 3);
    }
    if (flags & float_flag_inexact) {
        fcsr |= (1u << 4);
    }
    return fcsr;
}

static void linx_fp_sync_from_fcsr(CPULinxState *env)
{
    set_float_rounding_mode(linx_fcsr_rounding_mode(env->fcsr), &env->fp_status);
    set_float_exception_flags(linx_fcsr_to_softfloat_flags(env->fcsr), &env->fp_status);
}

static void linx_fp_sync_to_fcsr(CPULinxState *env)
{
    uint32_t fcsr = env->fcsr & ~LINX_FCSR_FFLAGS_MASK;
    fcsr |= linx_softfloat_flags_to_fcsr(get_float_exception_flags(&env->fp_status));
    env->fcsr = fcsr;
}

static uint64_t linx_fp_unop_fabs(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_abs((float64)a);
        break;
    case 1: { /* fs */
        float32 ra = float32_abs((float32)(uint32_t)a);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_unop_sqrt(CPULinxState *env, uint64_t a, uint32_t srctype);
static uint64_t linx_fp_unop_recip(CPULinxState *env, uint64_t a, uint32_t srctype);
static uint64_t linx_fp_unop_exp(CPULinxState *env, uint64_t a, uint32_t srctype);
static uint64_t linx_fp_binop_max(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype);
static uint64_t linx_fp_binop_min(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype);
static uint64_t linx_fp_ternop_muladd(CPULinxState *env, uint64_t a, uint64_t b,
                                      uint64_t c, uint32_t srctype, int flags);

static uint64_t linx_fp_binop_add(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_add((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_add((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_binop_sub(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_sub((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_sub((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_binop_mul(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_mul((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_mul((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_binop_div(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        res = (uint64_t)float64_div((float64)a, (float64)b, &env->fp_status);
        break;
    case 1: { /* fs */
        float32 ra = float32_div((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        res = (uint64_t)(uint32_t)ra;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_cmp_eq(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    bool ok = false;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        ok = float64_eq((float64)a, (float64)b, &env->fp_status);
        break;
    case 1:
        ok = float32_eq((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    linx_fcmp_trace_emit(env, "feq", LINX_FCMP_TRACE_OP_FEQ,
                         a, b, srctype, ok);
    return ok ? 1 : 0;
}

static uint64_t linx_fp_cmp_lt(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    bool ok = false;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        ok = float64_lt((float64)a, (float64)b, &env->fp_status);
        break;
    case 1:
        ok = float32_lt((float32)(uint32_t)a, (float32)(uint32_t)b, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    linx_fcmp_trace_emit(env, "flt", LINX_FCMP_TRACE_OP_FLT,
                         a, b, srctype, ok);
    return ok ? 1 : 0;
}

static uint64_t linx_fp_cmp_ge(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    bool ok = false;
    switch (srctype & 0x3u) {
    case 0: /* fd */
        ok = float64_le((float64)b, (float64)a, &env->fp_status);
        break;
    case 1:
        ok = float32_le((float32)(uint32_t)b, (float32)(uint32_t)a, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    linx_fcmp_trace_emit(env, "fge", LINX_FCMP_TRACE_OP_FGE,
                         a, b, srctype, ok);
    return ok ? 1 : 0;
}

static uint64_t linx_fp_fcvt(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    switch (srctype & 0x3u) {
    case 0: { /* src fd */
        if ((dsttype & 0x1fu) == 0) {
            res = a;
        } else if ((dsttype & 0x1fu) == 1) {
            float32 v = float64_to_float32((float64)a, &env->fp_status);
            res = (uint64_t)(uint32_t)v;
        } else {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    case 1: { /* src fs */
        uint32_t a32 = (uint32_t)a;
        if ((dsttype & 0x1fu) == 1) {
            res = a32;
        } else if ((dsttype & 0x1fu) == 0) {
            float64 v = float32_to_float64((float32)a32, &env->fp_status);
            res = (uint64_t)v;
        } else {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static unsigned linx_int_type_width(uint32_t type)
{
    switch (type & 0x1fu) {
    case 0:
    case 8:
        return 64;
    case 1:
    case 9:
        return 32;
    case 2:
    case 10:
        return 16;
    case 3:
    case 11:
        return 8;
    case 4:
    case 12:
        return 4;
    case 5:
    case 13:
        return 2;
    case 6:
    case 7:
    case 14:
        return 1;
    default:
        return 0;
    }
}

static bool linx_int_type_is_signed(uint32_t type)
{
    const unsigned t = type & 0x1fu;
    return t >= 8u && t <= 14u;
}

static uint64_t linx_int_mask(unsigned width)
{
    return width >= 64u ? UINT64_MAX : ((1ULL << width) - 1u);
}

static uint64_t linx_int_canonicalize(uint64_t value, uint32_t type)
{
    const unsigned width = linx_int_type_width(type);

    if (width == 0u) {
        return UINT64_MAX;
    }

    value &= linx_int_mask(width);
    if (linx_int_type_is_signed(type) && width < 64u) {
        value = (uint64_t)(((int64_t)(value << (64u - width))) >> (64u - width));
    }
    return value;
}

static uint64_t linx_fp_fcvti(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    const unsigned width = linx_int_type_width(dsttype);
    uint64_t res = 0;

    if (width == 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_from_fcsr(env);
    switch (srctype & 0x1fu) {
    case 0: {
        const float64 v = (float64)a;
        if (linx_int_type_is_signed(dsttype)) {
            res = width > 32u ? (uint64_t)float64_to_int64(v, &env->fp_status)
                              : (uint64_t)(int64_t)float64_to_int32(v, &env->fp_status);
        } else {
            res = width > 32u ? float64_to_uint64(v, &env->fp_status)
                              : (uint64_t)float64_to_uint32(v, &env->fp_status);
        }
        break;
    }
    case 1: {
        const float32 v = (float32)(uint32_t)a;
        if (linx_int_type_is_signed(dsttype)) {
            res = width > 32u ? (uint64_t)float32_to_int64(v, &env->fp_status)
                              : (uint64_t)(int64_t)float32_to_int32(v, &env->fp_status);
        } else {
            res = width > 32u ? float32_to_uint64(v, &env->fp_status)
                              : (uint64_t)float32_to_uint32(v, &env->fp_status);
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return linx_int_canonicalize(res, dsttype);
}

static uint64_t linx_fp_fcvti_round(CPULinxState *env, uint64_t a, uint32_t dsttype,
                                    uint32_t srctype, FloatRoundMode round_mode)
{
    const unsigned width = linx_int_type_width(dsttype);
    const FloatRoundMode prev_mode = linx_fcsr_rounding_mode(env->fcsr);
    uint64_t res = 0;

    if (width == 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_from_fcsr(env);
    set_float_rounding_mode(round_mode, &env->fp_status);
    switch (srctype & 0x1fu) {
    case 0: {
        const float64 v = (float64)a;
        if (linx_int_type_is_signed(dsttype)) {
            res = width > 32u ? (uint64_t)float64_to_int64(v, &env->fp_status)
                              : (uint64_t)(int64_t)float64_to_int32(v, &env->fp_status);
        } else {
            res = width > 32u ? float64_to_uint64(v, &env->fp_status)
                              : (uint64_t)float64_to_uint32(v, &env->fp_status);
        }
        break;
    }
    case 1: {
        const float32 v = (float32)(uint32_t)a;
        if (linx_int_type_is_signed(dsttype)) {
            res = width > 32u ? (uint64_t)float32_to_int64(v, &env->fp_status)
                              : (uint64_t)(int64_t)float32_to_int32(v, &env->fp_status);
        } else {
            res = width > 32u ? float32_to_uint64(v, &env->fp_status)
                              : (uint64_t)float32_to_uint32(v, &env->fp_status);
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    set_float_rounding_mode(prev_mode, &env->fp_status);
    linx_fp_sync_to_fcsr(env);
    return linx_int_canonicalize(res, dsttype);
}

static uint64_t linx_fp_fcvtz(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    const unsigned dt = dsttype & 0x1fu;

    switch (srctype & 0x3u) {
    case 0: { /* src fd */
        float64 v = (float64)a;
        switch (dt) {
        case 8: /* s64 */
            res = (uint64_t)float64_to_int64_round_to_zero(v, &env->fp_status);
            break;
        case 9: /* s32 */
            res = (uint64_t)(int64_t)float64_to_int32_round_to_zero(v, &env->fp_status);
            break;
        case 0: /* u64 */
            res = float64_to_uint64_round_to_zero(v, &env->fp_status);
            break;
        case 1: /* u32 */
            res = (uint64_t)float64_to_uint32_round_to_zero(v, &env->fp_status);
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    case 1: { /* src fs */
        float32 v = (float32)(uint32_t)a;
        switch (dt) {
        case 8: /* s64 */
            res = (uint64_t)float32_to_int64_round_to_zero(v, &env->fp_status);
            break;
        case 9: /* s32 */
            res = (uint64_t)(int64_t)float32_to_int32_round_to_zero(v, &env->fp_status);
            break;
        case 0: /* u64 */
            res = float32_to_uint64_round_to_zero(v, &env->fp_status);
            break;
        case 1: /* u32 */
            res = (uint64_t)float32_to_uint32_round_to_zero(v, &env->fp_status);
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_int_icvt(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    if (linx_int_type_width(srctype) == 0u || linx_int_type_width(dsttype) == 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }
    return linx_int_canonicalize(linx_int_canonicalize(a, srctype), dsttype);
}

static uint64_t linx_fp_scvtf(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    const unsigned dt = dsttype & 0x1fu;

    if (dt != 0 && dt != 1) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    switch (srctype & 0x3u) {
    case 0: { /* sd */
        int64_t v = (int64_t)a;
        if (dt == 0) {
            res = (uint64_t)int64_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)int64_to_float32(v, &env->fp_status);
        }
        break;
    }
    case 1: { /* sw */
        int32_t v = (int32_t)a;
        if (dt == 0) {
            res = (uint64_t)int32_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)int32_to_float32(v, &env->fp_status);
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_int_icvtf(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    const unsigned width = linx_int_type_width(srctype);
    const uint64_t canon = linx_int_canonicalize(a, srctype);
    uint64_t res = 0;

    if (width == 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_from_fcsr(env);
    switch (dsttype & 0x1fu) {
    case 0:
        if (linx_int_type_is_signed(srctype)) {
            res = width > 32u ? (uint64_t)int64_to_float64((int64_t)canon, &env->fp_status)
                              : (uint64_t)int32_to_float64((int32_t)canon, &env->fp_status);
        } else {
            res = width > 32u ? (uint64_t)uint64_to_float64(canon, &env->fp_status)
                              : (uint64_t)uint32_to_float64((uint32_t)canon, &env->fp_status);
        }
        break;
    case 1:
        if (linx_int_type_is_signed(srctype)) {
            res = width > 32u ? (uint64_t)(uint32_t)int64_to_float32((int64_t)canon,
                                                                     &env->fp_status)
                              : (uint64_t)(uint32_t)int32_to_float32((int32_t)canon,
                                                                     &env->fp_status);
        } else {
            res = width > 32u ? (uint64_t)(uint32_t)uint64_to_float32(canon,
                                                                      &env->fp_status)
                              : (uint64_t)(uint32_t)uint32_to_float32((uint32_t)canon,
                                                                      &env->fp_status);
        }
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

static uint64_t linx_fp_ucvtf(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);

    uint64_t res = 0;
    const unsigned dt = dsttype & 0x1fu;

    if (dt != 0 && dt != 1) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    switch (srctype & 0x3u) {
    case 0: { /* ud */
        uint64_t v = a;
        if (dt == 0) {
            res = (uint64_t)uint64_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)uint64_to_float32(v, &env->fp_status);
        }
        break;
    }
    case 1: { /* uw */
        uint32_t v = (uint32_t)a;
        if (dt == 0) {
            res = (uint64_t)uint32_to_float64(v, &env->fp_status);
        } else {
            res = (uint64_t)(uint32_t)uint32_to_float32(v, &env->fp_status);
        }
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return res;
}

uint64_t HELPER(linx_fadd)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_add(env, a, b, srctype);
}

uint64_t HELPER(linx_fsub)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_sub(env, a, b, srctype);
}

uint64_t HELPER(linx_fmul)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_mul(env, a, b, srctype);
}

uint64_t HELPER(linx_fdiv)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_div(env, a, b, srctype);
}

uint64_t HELPER(linx_fabs)(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    return linx_fp_unop_fabs(env, a, srctype);
}

uint64_t HELPER(linx_feq)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_eq(env, a, b, srctype);
}

uint64_t HELPER(linx_flt)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_lt(env, a, b, srctype);
}

uint64_t HELPER(linx_fge)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_ge(env, a, b, srctype);
}

uint64_t HELPER(linx_fne)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_eq(env, a, b, srctype) ? 0 : 1;
}

uint64_t HELPER(linx_feqs)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_eq(env, a, b, srctype);
}

uint64_t HELPER(linx_fnes)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_eq(env, a, b, srctype) ? 0 : 1;
}

uint64_t HELPER(linx_flts)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_lt(env, a, b, srctype);
}

uint64_t HELPER(linx_fges)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_cmp_ge(env, a, b, srctype);
}

uint64_t HELPER(linx_fmax)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_max(env, a, b, srctype);
}

uint64_t HELPER(linx_fmin)(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    return linx_fp_binop_min(env, a, b, srctype);
}

uint64_t HELPER(linx_fmadd)(CPULinxState *env, uint64_t a, uint64_t b,
                            uint64_t c, uint32_t srctype)
{
    return linx_fp_ternop_muladd(env, a, b, c, srctype, 0);
}

uint64_t HELPER(linx_fmsub)(CPULinxState *env, uint64_t a, uint64_t b,
                            uint64_t c, uint32_t srctype)
{
    return linx_fp_ternop_muladd(env, a, b, c, srctype, float_muladd_negate_c);
}

uint64_t HELPER(linx_fnmadd)(CPULinxState *env, uint64_t a, uint64_t b,
                             uint64_t c, uint32_t srctype)
{
    return linx_fp_ternop_muladd(env, a, b, c, srctype, float_muladd_negate_product);
}

uint64_t HELPER(linx_fnmsub)(CPULinxState *env, uint64_t a, uint64_t b,
                             uint64_t c, uint32_t srctype)
{
    return linx_fp_ternop_muladd(env, a, b, c, srctype,
                                 float_muladd_negate_product | float_muladd_negate_c);
}

uint64_t HELPER(linx_fsqrt)(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    return linx_fp_unop_sqrt(env, a, srctype);
}

uint64_t HELPER(linx_frecip)(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    return linx_fp_unop_recip(env, a, srctype);
}

uint64_t HELPER(linx_fexp)(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    return linx_fp_unop_exp(env, a, srctype);
}

uint64_t HELPER(linx_fcvt)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvt(env, a, dsttype, srctype);
}

uint64_t HELPER(linx_fcvta)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvti_round(env, a, dsttype, srctype, float_round_ties_away);
}

uint64_t HELPER(linx_fcvtm)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvti_round(env, a, dsttype, srctype, float_round_down);
}

uint64_t HELPER(linx_fcvtn)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvti_round(env, a, dsttype, srctype, float_round_nearest_even);
}

uint64_t HELPER(linx_fcvtp)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvti_round(env, a, dsttype, srctype, float_round_up);
}

uint64_t HELPER(linx_fcvtz)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvtz(env, a, dsttype, srctype);
}

uint64_t HELPER(linx_scvtf)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_scvtf(env, a, dsttype, srctype);
}

uint64_t HELPER(linx_ucvtf)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_ucvtf(env, a, dsttype, srctype);
}

void HELPER(linx_ebreak)(CPULinxState *env, uint32_t imm)
{
    CPUState *cs = env_cpu(env);
    const bool semihost_enabled = linx_semihost_enabled_p();
    uint64_t trap_pc = env->pc;

    if (linx_reconstruct_ebreak_pc(env, imm, &trap_pc)) {
        env->pc = trap_pc;
    }

    qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK imm=%d, a0=0x%lx, a1=0x%lx, a2=0x%lx\n",
                  imm, (unsigned long)env->gpr[LINX_REG_A0],
                  (unsigned long)env->gpr[LINX_REG_A1],
                  (unsigned long)env->gpr[LINX_REG_A2]);

    /*
     * Native Linx Linux/QEMU poweroff path:
     * allow a dedicated kernel-mode EBREAK immediate to request guest
     * shutdown without depending on opt-in semihost mode or MMIO exit
     * plumbing.
     */
    if (!semihost_enabled && (env->acr & 0xFu) != 2 && imm == 1) {
        qemu_log_mask(CPU_LOG_INT,
                      "Linx: kernel shutdown EBREAK at PC=0x%lx\n",
                      (unsigned long)env->pc);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        cpu_loop_exit_noexc(cs);
        return;
    }

    /*
     * ARM-style policy for Linx bring-up:
     * - default: all EBREAK immediates are architectural SW_BREAKPOINT traps
     * - opt-in semihost: LINX_SEMIHOST=1 enables imm[0..3] helper behavior.
     */
    if (semihost_enabled) {
        switch (imm) {
        case LINX_SEMIHOST_EXIT:
            qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK EXIT at PC=0x%lx\n",
                          (unsigned long)env->pc);
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            cpu_loop_exit_noexc(cs);
            break;

        case LINX_SEMIHOST_PUTCHAR: {
            int ch = env->gpr[LINX_REG_A0] & 0xff;
            qemu_log_mask(CPU_LOG_INT, "Linx: PUTCHAR '%c' (0x%02x)\n",
                          (ch >= 32 && ch < 127) ? ch : '.', ch);
            fputc(ch, stderr);
            fflush(stderr);
            env->gpr[LINX_REG_A0] = ch;
            return;
        }

        case LINX_SEMIHOST_WRITE: {
            uint64_t buf_addr = env->gpr[LINX_REG_A1];
            uint64_t len = env->gpr[LINX_REG_A2];
            uint64_t i;

            qemu_log_mask(CPU_LOG_INT, "Linx: WRITE buf=0x%lx len=%lu\n",
                          (unsigned long)buf_addr, (unsigned long)len);
            for (i = 0; i < len; i++) {
                uint8_t ch = cpu_ldub_data(env, buf_addr + i);
                fputc(ch, stderr);
            }
            fflush(stderr);
            env->gpr[LINX_REG_A0] = len;
            return;
        }

        case LINX_SEMIHOST_READ:
            env->gpr[LINX_REG_A0] = 0;
            return;
        default:
            break;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: EBREAK trap imm=%u acr=%u at PC=0x%lx (LINX_SEMIHOST=%u)\n",
                  imm, env->acr & 0xFu, (unsigned long)env->pc,
                  semihost_enabled ? 1u : 0u);
    env->pending_trap_cause = imm & 0xffu;
    cs->exception_index = LINX_EXCP_BREAKPOINT;
    cpu_loop_exit_restore(cs, GETPC());
}

void HELPER(raise_exception)(CPULinxState *env, uint32_t exception)
{
    CPUState *cs = env_cpu(env);
    if (exception == LINX_EXCP_ILLEGAL_INST && env->pc == 0x1b536) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: helper illegal pc=0x%" PRIx64
                      " next=0x%" PRIx64 " ri_count=%u ior_count=%u"
                      " blocktype=%u in_body=%u body_tpc=0x%" PRIx64
                      " return_pc=0x%" PRIx64
                      " lc=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]\n",
                      env->pc, env->insn_pc_next, env->vec_ri_count,
                      env->tile_ior_count, env->blocktype, env->in_body,
                      env->body_tpc, env->return_pc,
                      env->lc[0], env->lc[1], env->lc[2]);
        for (unsigned i = 0; i < env->tile_ior_count && i < LINX_TILE_MAX_IOR; i++) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: helper illegal ior[%u]=0x%016" PRIx64 "\n",
                          i, env->tile_ior_desc[i]);
        }
    }
    cs->exception_index = exception;
    cpu_loop_exit_restore(cs, GETPC());
}

/* ------------------------------------------------------------------------- */
/* Tile block helpers (TAU bring-up)                                         */
/* ------------------------------------------------------------------------- */

enum {
    LINX_BLOCK_STD  = 0,
    LINX_BLOCK_TMA  = 2,
    LINX_BLOCK_CUBE = 6,
    LINX_BLOCK_TEPL = 7,
};

enum {
    LINX_TMA_TLOAD  = 0,
    LINX_TMA_TSTORE = 1,
    LINX_TMA_TMOV = 2,
    LINX_TMA_TPREFETCH = 3,
    LINX_TMA_MGATHER = 4,
    LINX_TMA_MSCATTER = 5,
    LINX_TMA_MGATHER_MASK = 6,
    LINX_TMA_MSCATTER_MASK = 7,
    LINX_TMA_MGATHER_CAS = 8,
    LINX_TMA_TCVT_COMPAT = 31,
};

enum {
    LINX_CUBE_MAMULB = 0,
    LINX_CUBE_MAMULB_BIAS = 1,
    LINX_CUBE_MAMULB_ACC = 2,
    LINX_CUBE_ACCCVT = 8,
    LINX_CUBE_TGEMV = 16,
    LINX_CUBE_TGEMV_ACC = 18,
};

enum {
    LINX_IOT_S0V = 1u << 0,
    LINX_IOT_S1V = 1u << 1,
    LINX_IOT_S0R = 1u << 2,
    LINX_IOT_S1R = 1u << 3,
};

enum {
    LINX_TILE_IOT_SRC0_SHIFT = 0,
    LINX_TILE_IOT_SRC1_SHIFT = 6,
    LINX_TILE_IOT_DST_SHIFT = 12,
    LINX_TILE_IOT_LAST_SHIFT = 15,
    LINX_TILE_IOT_FLAGS_SHIFT = 16,
    LINX_TILE_IOT_REG_SHIFT = 20,
    LINX_TILE_IOT_SIZE_SHIFT = 25,
    LINX_TILE_IOT_HAS_SIZE_SHIFT = 30,
};

typedef struct LinxTileIOTDesc {
    uint32_t src0;
    uint32_t src1;
    uint32_t dst;
    uint32_t last;
    uint32_t flags;
    uint32_t reg;
    uint32_t size;
    bool has_size;
} LinxTileIOTDesc;

static inline LinxTileIOTDesc linx_tile_decode_iot(uint64_t packed)
{
    LinxTileIOTDesc d;
    d.src0 = (packed >> LINX_TILE_IOT_SRC0_SHIFT) & 0x3fu;
    d.src1 = (packed >> LINX_TILE_IOT_SRC1_SHIFT) & 0x3fu;
    d.dst = (packed >> LINX_TILE_IOT_DST_SHIFT) & 0x7u;
    d.last = (packed >> LINX_TILE_IOT_LAST_SHIFT) & 0x1u;
    d.flags = (packed >> LINX_TILE_IOT_FLAGS_SHIFT) & 0xfu;
    d.reg = (packed >> LINX_TILE_IOT_REG_SHIFT) & 0x1fu;
    d.size = (packed >> LINX_TILE_IOT_SIZE_SHIFT) & 0x1fu;
    d.has_size = ((packed >> LINX_TILE_IOT_HAS_SIZE_SHIFT) & 0x1u) != 0;
    return d;
}

static bool linx_tile_output_hand(const LinxTileIOTDesc *desc,
                                  unsigned *hand_out)
{
    if (desc->dst < LINX_TILE_HAND_COUNT) {
        *hand_out = desc->dst;
        return true;
    }
    return false;
}

static bool linx_tile_reserve_output(const uint8_t live[LINX_TILE_HAND_COUNT],
                                     const LinxTileIOTDesc *desc,
                                     unsigned *tile_out)
{
    unsigned hand = 0;

    if (!linx_tile_output_hand(desc, &hand)) {
        return false;
    }
    for (unsigned depth = 0; depth < LINX_TILE_HAND_DEPTH; depth++) {
        if ((live[hand] & (1u << depth)) == 0) {
            *tile_out = hand * LINX_TILE_HAND_DEPTH + depth;
            return true;
        }
    }
    return false;
}

static void linx_tile_publish_output(uint8_t live[LINX_TILE_HAND_COUNT],
                                     unsigned tile)
{
    const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
    const unsigned depth = tile % LINX_TILE_HAND_DEPTH;

    if (hand < LINX_TILE_HAND_COUNT) {
        live[hand] |= 1u << depth;
    }
}

static bool linx_tile_publish_order_state(
    uint8_t order[LINX_TILE_HAND_COUNT][LINX_TILE_HAND_DEPTH],
    uint8_t count_by_hand[LINX_TILE_HAND_COUNT], unsigned tile)
{
    const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
    const unsigned count = hand < LINX_TILE_HAND_COUNT
                           ? count_by_hand[hand] : 0;

    if (hand >= LINX_TILE_HAND_COUNT || count >= LINX_TILE_HAND_DEPTH) {
        return false;
    }
    for (unsigned rank = count; rank > 0; rank--) {
        order[hand][rank] = order[hand][rank - 1u];
    }
    order[hand][0] = tile;
    count_by_hand[hand] = count + 1u;
    return true;
}

static void linx_tile_remove_order_state(
    uint8_t order[LINX_TILE_HAND_COUNT][LINX_TILE_HAND_DEPTH],
    uint8_t count_by_hand[LINX_TILE_HAND_COUNT], unsigned tile)
{
    const unsigned hand = tile / LINX_TILE_HAND_DEPTH;

    if (hand >= LINX_TILE_HAND_COUNT) {
        return;
    }
    const unsigned count = count_by_hand[hand];
    for (unsigned rank = 0; rank < count; rank++) {
        if (order[hand][rank] != tile) {
            continue;
        }
        for (unsigned next = rank + 1u; next < count; next++) {
            order[hand][next - 1u] = order[hand][next];
        }
        order[hand][count - 1u] = 0;
        count_by_hand[hand] = count - 1u;
        return;
    }
}

static bool linx_tile_resolve_source(const CPULinxState *env,
                                     const uint8_t live[LINX_TILE_HAND_COUNT],
                                     unsigned encoded,
                                     unsigned *tile_out)
{
    const unsigned hand = (encoded >> 4) & 0x3u;
    const unsigned rank = encoded & 0xfu;
    unsigned tile;

    /* The 4x8 bring-up model rejects architectural ranks #9..#16. */
    if (rank >= LINX_TILE_HAND_DEPTH || rank >= env->tile_hand_count[hand]) {
        return false;
    }
    tile = env->tile_hand_order[hand][rank];
    const unsigned physical_hand = tile / LINX_TILE_HAND_DEPTH;
    const unsigned physical_depth = tile % LINX_TILE_HAND_DEPTH;
    if (physical_hand != hand ||
        (live[hand] & (1u << physical_depth)) == 0 ||
        env->tile_reg_bytes[tile] == 0) {
        return false;
    }
    *tile_out = tile;
    return true;
}

static bool linx_tile_pin_source(CPULinxState *env, unsigned tile)
{
    const uint16_t owner = 1u << (env->acr & 0xfu);

    if (tile >= LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH) {
        return false;
    }
    const uint16_t pinned = env->tile_pin_owner[tile];
    if (pinned != 0 && pinned != owner) {
        return false;
    }
    env->tile_pin_owner[tile] = owner;
    return true;
}

static void linx_tile_unpin_bindings(CPULinxState *env)
{
    const uint16_t owner = 1u << (env->acr & 0xfu);

    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        for (unsigned source = 0; source < 2; source++) {
            if ((env->tile_iot_src_valid[i] & (1u << source)) != 0) {
                const unsigned tile = env->tile_iot_src_phys[i][source];
                env->tile_pin_owner[tile] &= ~owner;
            }
        }
    }
}

static void linx_tile_invalidate_acc_sources_on_output(
    unsigned tile, uint8_t *acc_sources_valid,
    uint8_t acc_src0, uint8_t acc_src1)
{
    if (*acc_sources_valid && (tile == acc_src0 || tile == acc_src1)) {
        *acc_sources_valid = 0;
    }
}

static void linx_tile_release_source(uint8_t live[LINX_TILE_HAND_COUNT],
                                     uint8_t order[LINX_TILE_HAND_COUNT]
                                                  [LINX_TILE_HAND_DEPTH],
                                     uint8_t count_by_hand[LINX_TILE_HAND_COUNT],
                                     unsigned tile, bool reuse,
                                     uint8_t *carrier_valid,
                                     uint8_t *carrier)
{
    const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
    const unsigned depth = tile % LINX_TILE_HAND_DEPTH;

    if (reuse || hand >= LINX_TILE_HAND_COUNT) {
        return;
    }
    live[hand] &= ~(1u << depth);
    if (order) {
        linx_tile_remove_order_state(order, count_by_hand, tile);
    }
    if (*carrier_valid && *carrier == tile) {
        *carrier_valid = 0;
    }
}

static bool linx_tile_size_code_valid(unsigned size_code)
{
    const uint64_t bytes = size_code < 60u ? 1ull << (size_code + 4u) : 0;

    return bytes != 0 && bytes <= LINX_TILE_MAX_BYTES && (bytes & 3u) == 0;
}

typedef enum LinxTileLayout {
    LINX_TILE_LAYOUT_ND = 0,
    LINX_TILE_LAYOUT_DN = 1,
    LINX_TILE_LAYOUT_NZ = 2,
    LINX_TILE_LAYOUT_ZN = 3,
} LinxTileLayout;

typedef struct LinxTileFormatDesc {
    bool valid;
    LinxTileLayout src;
    LinxTileLayout dst;
} LinxTileFormatDesc;

typedef enum LinxTMATransferDir {
    LINX_TMA_GM_TO_TR = 0,
    LINX_TMA_TR_TO_GM = 1,
} LinxTMATransferDir;

enum {
    LINX_TMA_FMT_NORM  = 0u,
    LINX_TMA_FMT_ND2NZ = 1u,
    LINX_TMA_FMT_ND2ZN = 2u,
    LINX_TMA_FMT_DN2NZ = 3u,
    LINX_TMA_FMT_DN2ZN = 4u,
};

static inline uint32_t linx_tile_arg_format(uint32_t arg)
{
    return arg & 0x7u;
}

static inline uint32_t linx_tile_arg_pad(uint32_t arg)
{
    return (arg >> 3) & 0x3u;
}

static inline LinxTileFormatDesc linx_tile_decode_tma_format(uint32_t arg,
                                                             LinxTMATransferDir dir)
{
    LinxTileFormatDesc d = { .valid = true, .src = LINX_TILE_LAYOUT_ND, .dst = LINX_TILE_LAYOUT_ND };
    LinxTileLayout gm = LINX_TILE_LAYOUT_ND;
    LinxTileLayout tr = LINX_TILE_LAYOUT_ND;

    switch (linx_tile_arg_format(arg)) {
    case LINX_TMA_FMT_NORM:
        gm = LINX_TILE_LAYOUT_ND;
        tr = LINX_TILE_LAYOUT_ND;
        break;
    case LINX_TMA_FMT_ND2NZ:
        gm = LINX_TILE_LAYOUT_ND;
        tr = LINX_TILE_LAYOUT_NZ;
        break;
    case LINX_TMA_FMT_ND2ZN:
        gm = LINX_TILE_LAYOUT_ND;
        tr = LINX_TILE_LAYOUT_ZN;
        break;
    case LINX_TMA_FMT_DN2NZ:
        gm = LINX_TILE_LAYOUT_DN;
        tr = LINX_TILE_LAYOUT_NZ;
        break;
    case LINX_TMA_FMT_DN2ZN:
        gm = LINX_TILE_LAYOUT_DN;
        tr = LINX_TILE_LAYOUT_ZN;
        break;
    default:
        d.valid = false;
        return d;
    }

    if (dir == LINX_TMA_GM_TO_TR) {
        d.src = gm;
        d.dst = tr;
    } else {
        d.src = tr;
        d.dst = gm;
    }
    return d;
}

static inline unsigned linx_tile_dtype_elem_bytes(uint32_t dtype)
{
    switch (dtype & 0x1fu) {
    case 2u:  /* FP16 */
    case 4u:  /* legacy FP16/BF16-like bring-up encoding */
    case 6u:  /* BF16 */
    case 18u: /* INT16 */
    case 26u: /* UINT16 */
        return 2u;
    case 3u:  /* FP8 */
    case 7u:  /* FPL8 */
    case 19u: /* INT8 */
    case 27u: /* UINT8 */
        return 1u;
    case 1u:  /* FP32 */
    case 17u: /* INT32 */
    case 25u: /* UINT32 */
    case 0u:  /* FP64 compatibility remains word-backed in this model */
    case 16u: /* INT64 compatibility remains word-backed in this model */
    case 24u: /* UINT64 compatibility remains word-backed in this model */
    default:
        return 4u;
    }
}

static inline void linx_tile_set_elem_bytes(CPULinxState *env, unsigned tile,
                                            unsigned elem_bytes)
{
    if (tile < LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH &&
        (elem_bytes == 1u || elem_bytes == 2u ||
         elem_bytes == 4u || elem_bytes == 8u)) {
        env->tile_reg_elem_bytes[tile] = (uint8_t)elem_bytes;
    }
}

static inline void linx_tile_set_dtype(CPULinxState *env, unsigned tile,
                                       uint32_t dtype)
{
    if (tile < LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH) {
        env->tile_reg_dtype[tile] = dtype & 0x1fu;
    }
}

static bool linx_tile_set_shape(CPULinxState *env, unsigned tile,
                                uint32_t valid_cols, uint32_t valid_rows,
                                uint32_t cols, uint32_t rows)
{
    if (tile >= 32u || valid_cols == 0u || valid_rows == 0u || cols == 0u ||
        rows == 0u || valid_cols > cols || valid_rows > rows ||
        valid_cols > UINT16_MAX || valid_rows > UINT16_MAX ||
        cols > UINT16_MAX || rows > UINT16_MAX) {
        return false;
    }
    env->tile_reg_valid_cols[tile] = valid_cols;
    env->tile_reg_valid_rows[tile] = valid_rows;
    env->tile_reg_cols[tile] = cols;
    env->tile_reg_rows[tile] = rows;
    return true;
}

static bool linx_tile_set_block_shape(CPULinxState *env, unsigned tile,
                                      uint32_t bytes, unsigned elem_bytes)
{
    uint32_t valid_cols = (uint32_t)(env->lb[0] & 0xffffu);
    uint32_t valid_rows = (uint32_t)(env->lb[1] & 0xffffu);
    uint32_t cols = (uint32_t)(env->lb[2] & 0xffffu);

    if (elem_bytes == 0u || bytes == 0u || bytes % elem_bytes != 0u) {
        return false;
    }
    const uint32_t elems = bytes / elem_bytes;
    if (valid_cols == 0u) {
        valid_cols = elems;
    }
    if (valid_rows == 0u) {
        valid_rows = 1u;
    }
    if (cols == 0u) {
        cols = valid_cols;
    }
    if (cols == 0u || elems % cols != 0u) {
        return false;
    }
    return linx_tile_set_shape(env, tile, valid_cols, valid_rows, cols,
                               elems / cols);
}

static void linx_tile_copy_shape(CPULinxState *env, unsigned dst,
                                 unsigned src)
{
    env->tile_reg_valid_cols[dst] = env->tile_reg_valid_cols[src];
    env->tile_reg_valid_rows[dst] = env->tile_reg_valid_rows[src];
    env->tile_reg_cols[dst] = env->tile_reg_cols[src];
    env->tile_reg_rows[dst] = env->tile_reg_rows[src];
}

static bool linx_tile_shape_covers(const CPULinxState *env, unsigned tile,
                                   uint32_t valid_cols, uint32_t valid_rows,
                                   uint32_t cols, uint32_t rows)
{
    return tile < 32u && env->tile_reg_valid_cols[tile] >= valid_cols &&
           env->tile_reg_valid_rows[tile] >= valid_rows &&
           env->tile_reg_cols[tile] == cols &&
           env->tile_reg_rows[tile] >= rows;
}

static inline uint32_t linx_tile_pad_value(uint32_t pad_mode, uint32_t dtype,
                                           unsigned elem_bytes, uint32_t seed)
{
    uint32_t value = 0;
    const uint32_t mode = pad_mode & 0x1fu;
    const uint32_t dt = dtype & 0x1fu;

    switch (mode) {
    case 1u: /* Zero */
        value = 0u;
        break;
    case 2u: /* Max */
        if (elem_bytes == 2u) {
            value = (dt == 4u) ? 0x7bffu : 0xffffu;
        } else {
            value = (dt == 1u) ? 0x7f7fffffu : 0x7fffffffu;
        }
        break;
    case 3u: /* Min */
        if (elem_bytes == 2u) {
            value = (dt == 4u) ? 0xfbffu : 0x8000u;
        } else {
            value = (dt == 1u) ? 0xff7fffffu : 0x80000000u;
        }
        break;
    case 0u: /* Null / unspecified */
    default:
        /* Deterministic pseudo-random fill in bring-up mode. */
        value = (seed * 1664525u) + 1013904223u;
        break;
    }

    if (elem_bytes == 2u) {
        return value & 0xffffu;
    }
    return value;
}

static inline bool linx_tile_linear_index(LinxTileLayout layout, unsigned outer,
                                          unsigned inner, unsigned elem_bytes,
                                          unsigned o, unsigned i, uint32_t *idx_out)
{
    if (outer == 0u || inner == 0u || o >= outer || i >= inner) {
        return false;
    }

    if (layout == LINX_TILE_LAYOUT_ND) {
        *idx_out = (uint32_t)(o * inner + i);
        return true;
    }
    if (layout == LINX_TILE_LAYOUT_DN) {
        *idx_out = (uint32_t)(i * outer + o);
        return true;
    }

    const unsigned blk_inner = MAX(1u, 32u / elem_bytes);
    const unsigned blk_outer = 16u;
    if ((inner % blk_inner) != 0u || (outer % blk_outer) != 0u) {
        return false;
    }

    const unsigned nblk_outer = outer / blk_outer;
    const unsigned nblk_inner = inner / blk_inner;
    const unsigned bo = o / blk_outer;
    const unsigned bi = i / blk_inner;
    const unsigned io = o % blk_outer;
    const unsigned ii = i % blk_inner;
    const unsigned blk_area = blk_outer * blk_inner;
    uint32_t blk_index = 0;
    uint32_t inblk_index = 0;

    if (layout == LINX_TILE_LAYOUT_NZ) {
        blk_index = (uint32_t)(bi * nblk_outer + bo);           /* inter-block column-major */
        inblk_index = (uint32_t)(io * blk_inner + ii);          /* intra-block row-major */
    } else { /* ZN */
        blk_index = (uint32_t)(bo * nblk_inner + bi);           /* inter-block row-major */
        inblk_index = (uint32_t)(ii * blk_outer + io);          /* intra-block column-major */
    }
    *idx_out = blk_index * blk_area + inblk_index;
    return true;
}

static inline bool linx_tile_layout_shape_valid(LinxTileLayout layout,
                                                uint32_t outer,
                                                uint32_t inner,
                                                unsigned elem_bytes)
{
    return (layout != LINX_TILE_LAYOUT_NZ && layout != LINX_TILE_LAYOUT_ZN) ||
           (((uint64_t)inner * elem_bytes) % 32u == 0u &&
            (outer % 16u) == 0u);
}

/* ------------------------------------------------------------------------- */
/* Restartable template blocks                                               */
/* ------------------------------------------------------------------------- */

static inline int linx_next_fentry_reg(int current)
{
    current++;
    if (current > 23) {
        current = 2;
    }
    return current;
}

static inline int linx_fentry_reg_count(int begin, int end)
{
    if (begin <= end) {
        return end - begin + 1;
    }
    return (23 - begin + 1) + (end - 2 + 1);
}

static inline void linx_template_clear(CPULinxState *env)
{
    env->tmpl_pc = 0;
    env->tmpl_kind = 0;
    env->tmpl_step = 0;
    env->tmpl_reg_cur = 0;
    env->tmpl_reg_begin = 0;
    env->tmpl_reg_end = 0;
    env->tmpl_stacksize = 0;
    env->tmpl_mem_dst = 0;
    env->tmpl_mem_src = 0;
    env->tmpl_mem_remaining = 0;
    env->tmpl_mem_value = 0;
}

static inline void linx_frame_storeq_after_probe(CPULinxState *env,
                                                 uint64_t addr,
                                                 uint64_t value,
                                                 int mmu_idx,
                                                 void *host)
{
    if (likely(host != NULL)) {
        stq_le_p(host, value);
        return;
    }
    cpu_stq_le_mmuidx_ra(env, (abi_ptr)addr, value, mmu_idx, GETPC());
}

static inline uint64_t linx_frame_loadq_cached_or_fallback(CPULinxState *env,
                                                           uint64_t addr,
                                                           int mmu_idx,
                                                           bool use_cached_host,
                                                           int *host_loads,
                                                           int *fallback_loads)
{
    if (use_cached_host &&
        (addr & (TARGET_PAGE_SIZE - 1)) <= TARGET_PAGE_SIZE - 8) {
        void *host = tlb_vaddr_to_host(env, (vaddr)addr, MMU_DATA_LOAD,
                                       mmu_idx);

        if (likely(host != NULL)) {
            const uint64_t host_value = ldq_le_p(host);

            if (host_loads) {
                (*host_loads)++;
            }
            if (unlikely(linx_frame_restore_host_verify_enabled_p())) {
                const uint64_t fallback_value =
                    cpu_ldq_le_mmuidx_ra(env, (abi_ptr)addr, mmu_idx, GETPC());

                linx_frame_stat_restore_host_verify_loads++;
                if (unlikely(host_value != fallback_value)) {
                    linx_frame_stat_restore_host_verify_mismatches++;
                    if (!linx_frame_restore_host_verify_emit_limit ||
                        linx_frame_restore_host_verify_emitted <
                            linx_frame_restore_host_verify_emit_limit) {
                        linx_frame_restore_host_verify_emitted++;
                        fprintf(stderr,
                                "LINX_FRAME_RESTORE_HOST_MISMATCH"
                                " count=%" PRIu64
                                " addr=0x%" PRIx64
                                " mmu=%d acr=%u"
                                " host=0x%" PRIx64
                                " fallback=0x%" PRIx64
                                " envpc=0x%" PRIx64
                                " bpc=0x%" PRIx64
                                " tpc=0x%" PRIx64
                                " sp=0x%" PRIx64
                                " cstate=0x%" PRIx64
                                "\n",
                                env->insn_count, addr, mmu_idx,
                                (unsigned)(env->acr & 0xfu),
                                host_value, fallback_value, env->pc,
                                env->bpc, env->body_tpc,
                                env->gpr[LINX_REG_SP], env->ssr[0x20]);
                        fflush(stderr);
                    }
                }
            }
            return host_value;
        }
    }

    if (fallback_loads) {
        (*fallback_loads)++;
    }
    return cpu_ldq_le_mmuidx_ra(env, (abi_ptr)addr, mmu_idx, GETPC());
}

static bool linx_frame_restore_prepare_page_fast(CPULinxState *env,
                                                 uint64_t stacksize,
                                                 uint64_t new_sp,
                                                 uint64_t restore_base,
                                                 int begin, int end,
                                                 uint32_t regs[LINX_GPR_COUNT],
                                                 uint64_t addrs[LINX_GPR_COUNT],
                                                 uint64_t values[LINX_GPR_COUNT],
                                                 int *host_loads,
                                                 int *fallback_loads,
                                                 int *count_out)
{
    const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
    int reg = begin;
    uint32_t step = 1;
    int n = 0;

    if (!linx_frame_page_fast_shape(begin, end, stacksize, count) ||
        linx_frame_restore_host_verify_enabled_p()) {
        return false;
    }

    const uint64_t bytes = (uint64_t)count * 8ull;
    const uint64_t low_addr = new_sp - restore_base - bytes;
    if (!linx_frame_range_one_page(low_addr, bytes)) {
        return false;
    }

    const int mmu_idx = linx_env_mmu_index(env);
    uint8_t *host = probe_read(env, (vaddr)low_addr, (int)bytes, mmu_idx,
                               GETPC());
    if (host == NULL) {
        return false;
    }

    while (1) {
        const uint64_t addr = new_sp - restore_base - ((uint64_t)step * 8ull);
        const uint64_t off = addr - low_addr;

        regs[n] = (uint32_t)reg;
        addrs[n] = addr;
        values[n] = ldq_le_p(host + off);
        n++;
        if (reg == end) {
            break;
        }
        reg = linx_next_fentry_reg(reg);
        step++;
    }

    if (host_loads) {
        *host_loads = n;
    }
    if (fallback_loads) {
        *fallback_loads = 0;
    }
    if (count_out) {
        *count_out = n;
    }
    linx_frame_stat_page_fast_restore++;
    return true;
}

static int linx_frame_restore_prepare(CPULinxState *env, uint64_t stacksize,
                                      uint64_t new_sp, uint64_t restore_base,
                                      int begin, int end,
                                      uint32_t regs[LINX_GPR_COUNT],
                                      uint64_t addrs[LINX_GPR_COUNT],
                                      uint64_t values[LINX_GPR_COUNT],
                                      int *host_loads,
                                      int *fallback_loads)
{
    const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
    const int mmu_idx = linx_env_mmu_index(env);
    const bool use_cached_host = linx_frame_restore_host_load_enabled_p();
    int reg = begin;
    uint32_t step = 1;
    int n = 0;

    if (host_loads) {
        *host_loads = 0;
    }
    if (fallback_loads) {
        *fallback_loads = 0;
    }

    if (count <= 0) {
        return 0;
    }

    if (linx_frame_restore_prepare_page_fast(env, stacksize, new_sp,
                                             restore_base, begin, end,
                                             regs, addrs, values,
                                             host_loads, fallback_loads, &n)) {
        return n;
    }

    while (1) {
        const uint64_t addr = new_sp - restore_base - ((uint64_t)step * 8ull);

        if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
            regs[n] = (uint32_t)reg;
            addrs[n] = addr;
            values[n] =
                linx_frame_loadq_cached_or_fallback(env, addr, mmu_idx,
                                                    use_cached_host,
                                                    host_loads,
                                                    fallback_loads);
            n++;
        }
        if (reg == end) {
            break;
        }
        reg = linx_next_fentry_reg(reg);
        step++;
    }

    return n;
}

static void linx_frame_restore_commit(CPULinxState *env, uint64_t cur_pc,
                                      const uint32_t regs[LINX_GPR_COUNT],
                                      const uint64_t addrs[LINX_GPR_COUNT],
                                      const uint64_t values[LINX_GPR_COUNT],
                                      int count)
{
    for (int i = 0; i < count; i++) {
        const uint32_t reg = regs[i];
        const uint64_t addr = addrs[i];
        const uint64_t v = values[i];

        env->gpr[reg] = v;
        linx_trace_mem(env, false, addr, 0, v, 8);
        linx_trace_wb(env, reg, v);
        if (reg == LINX_REG_RA) {
            trace_linx_ra_trace(cur_pc, 3, env->gpr[LINX_REG_SP],
                                env->gpr[LINX_REG_RA],
                                env->brtype & 0x7u, env->cond, env->carg,
                                env->tgt, addr, v);
        }
    }
}

static bool linx_template_fentry_single_reg_fast(CPULinxState *env,
                                                 CPUState *cs,
                                                 uint64_t cur_pc,
                                                 uint64_t next_pc,
                                                 uint32_t reg,
                                                 uint64_t stacksize,
                                                 uint64_t old_sp,
                                                 uint64_t new_sp,
                                                 int mmu_idx,
                                                 bool frame_stats,
                                                 bool chain)
{
    if (!linx_frame_single_reg_fast_enabled_p() ||
        !linx_frame_single_reg_fast_shape(reg, reg, stacksize)) {
        return false;
    }

    const uint64_t addr = new_sp + stacksize - 8;
    void *host = probe_write(env, (vaddr)addr, 8, mmu_idx, GETPC());

    if (frame_stats) {
        linx_frame_stat_fentry_save_probes++;
    }
    linx_frame_shape_hot_record(env, LINX_TEMPLATE_FENTRY, reg, reg,
                                stacksize, 1);

    env->gpr[LINX_REG_SP] = new_sp;
    linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
    trace_linx_ra_trace(cur_pc, 2, env->gpr[LINX_REG_SP],
                        env->gpr[LINX_REG_RA], env->brtype & 0x7u,
                        env->cond, env->carg, env->tgt, old_sp,
                        env->gpr[LINX_REG_SP]);

    const uint64_t v = env->gpr[reg];
    linx_trace_mem(env, true, addr, v, 0, 8);
    linx_frame_storeq_after_probe(env, addr, v, mmu_idx, host);
    if (frame_stats) {
        linx_frame_stat_fentry_save_slots++;
        if (host != NULL) {
            linx_frame_stat_fentry_host_stores++;
        } else {
            linx_frame_stat_fentry_fallback_stores++;
        }
        linx_frame_stat_single_fast_fentry++;
    }
    if (reg == LINX_REG_RA) {
        trace_linx_ra_trace(cur_pc, 2, env->gpr[LINX_REG_SP],
                            env->gpr[LINX_REG_RA], env->brtype & 0x7u,
                            env->cond, env->carg, env->tgt, addr, v);
    }

    linx_template_clear(env);
    env->pc = next_pc;
    linx_template_commit_or_chain(env, cs, env->pc, chain);
    return true;
}

static bool linx_template_fentry_page_fast(CPULinxState *env,
                                           CPUState *cs,
                                           uint64_t cur_pc,
                                           uint64_t next_pc,
                                           uint32_t reg_begin,
                                           uint32_t reg_end,
                                           uint64_t stacksize,
                                           uint64_t old_sp,
                                           uint64_t new_sp,
                                           int mmu_idx,
                                           int count,
                                           bool frame_stats,
                                           bool chain)
{
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    int reg = begin;
    uint32_t step = 1;

    if (!linx_frame_page_fast_shape(begin, end, stacksize, count)) {
        return false;
    }

    const uint64_t bytes = (uint64_t)count * 8ull;
    const uint64_t low_addr = new_sp + stacksize - bytes;
    if (!linx_frame_range_one_page(low_addr, bytes)) {
        return false;
    }

    uint8_t *host = probe_write(env, (vaddr)low_addr, (int)bytes, mmu_idx,
                                GETPC());
    if (host == NULL) {
        return false;
    }

    if (frame_stats) {
        linx_frame_stat_fentry_save_probes++;
    }
    linx_frame_shape_hot_record(env, LINX_TEMPLATE_FENTRY, reg_begin, reg_end,
                                stacksize, count);

    env->gpr[LINX_REG_SP] = new_sp;
    linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
    trace_linx_ra_trace(cur_pc, 2, env->gpr[LINX_REG_SP],
                        env->gpr[LINX_REG_RA], env->brtype & 0x7u,
                        env->cond, env->carg, env->tgt, old_sp,
                        env->gpr[LINX_REG_SP]);

    while (1) {
        const uint64_t addr = new_sp + stacksize - ((uint64_t)step * 8ull);
        const uint64_t off = addr - low_addr;
        const uint64_t v = env->gpr[reg];

        linx_trace_mem(env, true, addr, v, 0, 8);
        stq_le_p(host + off, v);
        if (frame_stats) {
            linx_frame_stat_fentry_save_slots++;
            linx_frame_stat_fentry_host_stores++;
        }
        if (reg == LINX_REG_RA) {
            trace_linx_ra_trace(cur_pc, 2, env->gpr[LINX_REG_SP],
                                env->gpr[LINX_REG_RA], env->brtype & 0x7u,
                                env->cond, env->carg, env->tgt, addr, v);
        }
        if (reg == end) {
            break;
        }
        reg = linx_next_fentry_reg(reg);
        step++;
    }

    if (frame_stats) {
        linx_frame_stat_page_fast_fentry++;
    }
    linx_template_clear(env);
    env->pc = next_pc;
    linx_template_commit_or_chain(env, cs, env->pc, chain);
    return true;
}

static bool linx_template_fret_stk_single_reg_fast(CPULinxState *env,
                                                   CPUState *cs,
                                                   uint64_t cur_pc,
                                                   uint32_t reg,
                                                   uint64_t stacksize,
                                                   uint64_t old_sp,
                                                   uint64_t new_sp,
                                                   uint64_t restore_base,
                                                   bool frame_stats,
                                                   bool chain)
{
    if (!linx_frame_single_reg_fast_enabled_p() ||
        !linx_frame_single_reg_fast_shape(reg, reg, stacksize) ||
        linx_fret_stk_trace_enabled_fast()) {
        return false;
    }

    const int mmu_idx = linx_env_mmu_index(env);
    const bool use_cached_host =
        linx_frame_restore_host_load_enabled_p() ||
        linx_frame_single_restore_host_load_enabled_p();
    int restore_host_loads = 0;
    int restore_fallback_loads = 0;
    const uint64_t addr = new_sp - restore_base - 8;
    const uint64_t value =
        linx_frame_loadq_cached_or_fallback(env, addr, mmu_idx,
                                            use_cached_host,
                                            &restore_host_loads,
                                            &restore_fallback_loads);

    linx_frame_shape_hot_record(env, LINX_TEMPLATE_FRET_STK, reg, reg,
                                stacksize, 1);

    if (frame_stats) {
        linx_frame_stat_fret_stk_calls++;
        linx_frame_stat_restore_slots++;
        linx_frame_stat_restore_host_loads += restore_host_loads;
        linx_frame_stat_restore_fallback_loads += restore_fallback_loads;
        linx_frame_stat_single_fast_fret_stk++;
    }

    env->gpr[LINX_REG_SP] = new_sp;
    linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);

    env->gpr[reg] = value;
    linx_trace_mem(env, false, addr, 0, value, 8);
    linx_trace_wb(env, reg, value);
    if (reg == LINX_REG_RA) {
        trace_linx_ra_trace(cur_pc, 3, env->gpr[LINX_REG_SP],
                            env->gpr[LINX_REG_RA], env->brtype & 0x7u,
                            env->cond, env->carg, env->tgt, addr, value);
    }

    const uint64_t ra = env->gpr[LINX_REG_RA];
    linx_call_trace_emit(env, LINX_CALL_TRACE_FRET_STK, cur_pc, ra, old_sp);
    const bool ret_fast_hit = linx_bstart_cache_fast_hit(env, ra);
    if (frame_stats) {
        if (ret_fast_hit) {
            linx_frame_stat_ret_fast_hits++;
        } else {
            linx_frame_stat_ret_checks++;
        }
    }
    if (!ret_fast_hit) {
        HELPER(linx_check_bstart_target)(env, ra);
    }
    linx_template_clear(env);
    env->pc = ra;
    linx_template_commit_or_chain(env, cs, env->pc, chain);
    return true;
}

static inline uint8_t linx_extctx_byte(const CPULinxState *env, uint64_t ext_kind, uint64_t off)
{
    static const uint8_t magic[8] = { 'L', 'I', 'N', 'X', '_', 'E', 'X', 'T' };

    if (off < 8) {
        return magic[off];
    }
    if (off < 16) {
        const unsigned sh = (unsigned)((off - 8) * 8u);
        return (uint8_t)((ext_kind >> sh) & 0xffu);
    }
    if (off < 40) {
        const unsigned idx = (unsigned)((off - 16) / 8u);
        const unsigned sh = (unsigned)(((off - 16) % 8u) * 8u);
        return (uint8_t)((env->lb[idx] >> sh) & 0xffu);
    }
    if (off < 64) {
        const unsigned idx = (unsigned)((off - 40) / 8u);
        const unsigned sh = (unsigned)(((off - 40) % 8u) * 8u);
        return (uint8_t)((env->lc[idx] >> sh) & 0xffu);
    }
    return 0;
}

static inline void linx_extctx_write_byte(CPULinxState *env, uint64_t off, uint8_t v)
{
    if (off >= 16 && off < 40) {
        const unsigned idx = (unsigned)((off - 16) / 8u);
        const unsigned sh = (unsigned)(((off - 16) % 8u) * 8u);
        env->lb[idx] = (env->lb[idx] & ~(0xffull << sh)) | ((uint64_t)v << sh);
        return;
    }
    if (off >= 40 && off < 64) {
        const unsigned idx = (unsigned)((off - 40) / 8u);
        const unsigned sh = (unsigned)(((off - 40) % 8u) * 8u);
        env->lc[idx] = (env->lc[idx] & ~(0xffull << sh)) | ((uint64_t)v << sh);
        return;
    }
}

static void linx_template_fentry_impl(CPULinxState *env, uint64_t cur_pc,
                                      uint64_t next_pc, uint32_t reg_begin,
                                      uint32_t reg_end, uint64_t stacksize,
                                      bool chain)
{
    CPUState *cs = env_cpu(env);
    const uint64_t adj = stacksize + linx_callframe_size;
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
    const uint64_t old_sp = env->gpr[LINX_REG_SP];
    const uint64_t new_sp = old_sp - adj;
    const int mmu_idx = linx_env_mmu_index(env);
    const bool fentry_trace_enabled = linx_fentry_trace_enabled_fast();
    const bool frame_stats = unlikely(linx_frame_stats_enabled_p());
    bool fentry_trace = false;

    if (frame_stats) {
        linx_frame_stat_fentry_calls++;
    }

    linx_call_trace_emit(env, LINX_CALL_TRACE_FENTRY, cur_pc, new_sp, stacksize);

    if (!fentry_trace_enabled &&
        count == 1 &&
        linx_template_fentry_single_reg_fast(env, cs, cur_pc, next_pc,
                                             reg_begin, stacksize, old_sp,
                                             new_sp, mmu_idx, frame_stats,
                                             chain)) {
        return;
    }
    if (!fentry_trace_enabled &&
        linx_template_fentry_page_fast(env, cs, cur_pc, next_pc, reg_begin,
                                       reg_end, stacksize, old_sp, new_sp,
                                       mmu_idx, count, frame_stats, chain)) {
        return;
    }

    void *save_hosts[LINX_GPR_COUNT];
    if (fentry_trace_enabled) {
        memset(save_hosts, 0, sizeof(save_hosts));
    }

    /*
     * User stacks can grow on the first save below the old SP.  Probe the save
     * slots before committing SP so a handled page fault retries from the
     * original architectural state instead of subtracting the frame twice.
     */
    if (count > 0) {
        int reg = begin;
        uint32_t step = 1;
        while (1) {
            const int64_t off = (int64_t)stacksize - ((int64_t)step * 8);
            if (off < 0) {
                break;
            }
            if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t addr = new_sp + (uint64_t)off;
                save_hosts[reg] = probe_write(env, (vaddr)addr, 8, mmu_idx,
                                              GETPC());
                if (frame_stats) {
                    linx_frame_stat_fentry_save_probes++;
                }
            }
            if (reg == end) {
                break;
            }
            reg = linx_next_fentry_reg(reg);
            step++;
        }
    }
    linx_frame_shape_hot_record(env, LINX_TEMPLATE_FENTRY, reg_begin, reg_end,
                                stacksize, count);

    if (fentry_trace_enabled) {
        fentry_trace = linx_fentry_trace_matches(env, cur_pc, old_sp, new_sp,
                                                 env->gpr[LINX_REG_RA]);
    }

    if (adj) {
        env->gpr[LINX_REG_SP] = new_sp;
        linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
        trace_linx_ra_trace(cur_pc, 2, env->gpr[LINX_REG_SP],
                            env->gpr[LINX_REG_RA], env->brtype & 0x7u,
                            env->cond, env->carg, env->tgt, old_sp,
                            env->gpr[LINX_REG_SP]);
    }

    if (fentry_trace) {
        linx_fentry_trace_begin(env, cur_pc, next_pc, old_sp, new_sp,
                                stacksize, begin, end, count, mmu_idx);
    }

    if (count > 0) {
        int reg = begin;
        uint32_t step = 1;
        while (1) {
            const int64_t off = (int64_t)stacksize - ((int64_t)step * 8);
            if (off < 0) {
                break;
            }
            if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t addr = env->gpr[LINX_REG_SP] + (uint64_t)off;
                const uint64_t v = env->gpr[reg];
                linx_trace_mem(env, true, addr, v, 0, 8);
                linx_frame_storeq_after_probe(env, addr, v, mmu_idx,
                                              save_hosts[reg]);
                if (frame_stats) {
                    linx_frame_stat_fentry_save_slots++;
                    if (save_hosts[reg] != NULL) {
                        linx_frame_stat_fentry_host_stores++;
                    } else {
                        linx_frame_stat_fentry_fallback_stores++;
                    }
                }
                if (fentry_trace) {
                    linx_fentry_trace_slot(env, cur_pc, reg, addr, v, mmu_idx,
                                           save_hosts[reg]);
                }
                if (reg == LINX_REG_RA) {
                    trace_linx_ra_trace(cur_pc, 2, env->gpr[LINX_REG_SP],
                                        env->gpr[LINX_REG_RA],
                                        env->brtype & 0x7u, env->cond,
                                        env->carg, env->tgt, addr, v);
                }
            }
            if (reg == end) {
                break;
            }
            reg = linx_next_fentry_reg(reg);
            step++;
        }
    }

    if (fentry_trace) {
        linx_fentry_trace_end(env, cur_pc, new_sp);
    }

    linx_template_clear(env);
    env->pc = next_pc;
    linx_template_commit_or_chain(env, cs, env->pc, chain);
}

void HELPER(linx_template_fentry)(CPULinxState *env, uint64_t cur_pc,
                                  uint64_t next_pc, uint32_t reg_begin,
                                  uint32_t reg_end, uint64_t stacksize)
{
    linx_template_fentry_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                              stacksize, false);
    g_assert_not_reached();
}

void HELPER(linx_template_fentry_chain)(CPULinxState *env, uint64_t cur_pc,
                                        uint64_t next_pc, uint32_t reg_begin,
                                        uint32_t reg_end, uint64_t stacksize)
{
    linx_template_fentry_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                              stacksize, true);
}

static void linx_template_fexit_impl(CPULinxState *env, uint64_t cur_pc,
                                     uint64_t next_pc, uint32_t reg_begin,
                                     uint32_t reg_end, uint64_t stacksize,
                                     bool chain)
{
    CPUState *cs = env_cpu(env);
    const uint64_t adj = stacksize + linx_callframe_size;
    const uint64_t restore_base = linx_callframe_size;
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    const uint64_t old_sp = env->gpr[LINX_REG_SP];
    const uint64_t new_sp = old_sp + adj;
    const bool frame_stats = unlikely(linx_frame_stats_enabled_p());

    uint32_t regs[LINX_GPR_COUNT];
    uint64_t addrs[LINX_GPR_COUNT];
    uint64_t values[LINX_GPR_COUNT];
    int restore_host_loads = 0;
    int restore_fallback_loads = 0;
    const int restore_count =
        linx_frame_restore_prepare(env, stacksize, new_sp, restore_base,
                                   begin, end, regs, addrs, values,
                                   &restore_host_loads,
                                   &restore_fallback_loads);
    linx_frame_shape_hot_record(env, LINX_TEMPLATE_FEXIT, reg_begin, reg_end,
                                stacksize, restore_count);

    if (frame_stats) {
        linx_frame_stat_fexit_calls++;
        linx_frame_stat_restore_slots += restore_count;
        linx_frame_stat_restore_host_loads += restore_host_loads;
        linx_frame_stat_restore_fallback_loads += restore_fallback_loads;
    }

    if (adj) {
        env->gpr[LINX_REG_SP] = new_sp;
        linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
    }

    linx_frame_restore_commit(env, cur_pc, regs, addrs, values,
                              restore_count);

    linx_template_clear(env);
    env->pc = next_pc;
    linx_template_commit_or_chain(env, cs, env->pc, chain);
}

void HELPER(linx_template_fexit)(CPULinxState *env, uint64_t cur_pc,
                                 uint64_t next_pc, uint32_t reg_begin,
                                 uint32_t reg_end, uint64_t stacksize)
{
    linx_template_fexit_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                             stacksize, false);
    g_assert_not_reached();
}

void HELPER(linx_template_fexit_chain)(CPULinxState *env, uint64_t cur_pc,
                                       uint64_t next_pc, uint32_t reg_begin,
                                       uint32_t reg_end, uint64_t stacksize)
{
    linx_template_fexit_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                             stacksize, true);
}

static void linx_template_fret_stk_impl(CPULinxState *env, uint64_t cur_pc,
                                        uint64_t next_pc, uint32_t reg_begin,
                                        uint32_t reg_end, uint64_t stacksize,
                                        bool chain)
{
    CPUState *cs = env_cpu(env);
    const uint64_t adj = stacksize + linx_callframe_size;
    const uint64_t restore_base = linx_callframe_size;
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    const uint64_t old_sp = env->gpr[LINX_REG_SP];
    const uint64_t new_sp = old_sp + adj;
    const bool frame_stats = unlikely(linx_frame_stats_enabled_p());

    if (reg_begin == reg_end &&
        linx_template_fret_stk_single_reg_fast(env, cs, cur_pc, reg_begin,
                                               stacksize, old_sp, new_sp,
                                               restore_base, frame_stats,
                                               chain)) {
        return;
    }

    uint32_t regs[LINX_GPR_COUNT];
    uint64_t addrs[LINX_GPR_COUNT];
    uint64_t values[LINX_GPR_COUNT];
    int restore_host_loads = 0;
    int restore_fallback_loads = 0;
    const int restore_count =
        linx_frame_restore_prepare(env, stacksize, new_sp, restore_base,
                                   begin, end, regs, addrs, values,
                                   &restore_host_loads,
                                   &restore_fallback_loads);
    linx_frame_shape_hot_record(env, LINX_TEMPLATE_FRET_STK, reg_begin,
                                reg_end, stacksize, restore_count);

    if (frame_stats) {
        linx_frame_stat_fret_stk_calls++;
        linx_frame_stat_restore_slots += restore_count;
        linx_frame_stat_restore_host_loads += restore_host_loads;
        linx_frame_stat_restore_fallback_loads += restore_fallback_loads;
    }

    linx_fret_stk_trace_emit(env, cur_pc, next_pc, old_sp, new_sp, stacksize,
                             restore_base, begin, end, regs, addrs, values,
                             restore_count);

    if (adj) {
        env->gpr[LINX_REG_SP] = new_sp;
        linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
    }

    linx_frame_restore_commit(env, cur_pc, regs, addrs, values,
                              restore_count);

    const uint64_t ra = env->gpr[LINX_REG_RA];
    linx_call_trace_emit(env, LINX_CALL_TRACE_FRET_STK, cur_pc, ra, old_sp);
    const bool ret_fast_hit = linx_bstart_cache_fast_hit(env, ra);
    if (frame_stats) {
        if (ret_fast_hit) {
            linx_frame_stat_ret_fast_hits++;
        } else {
            linx_frame_stat_ret_checks++;
        }
    }
    if (!ret_fast_hit) {
        HELPER(linx_check_bstart_target)(env, ra);
    }
    linx_template_clear(env);
    env->pc = ra;
    linx_template_commit_or_chain(env, cs, env->pc, chain);
}

void HELPER(linx_template_fret_stk)(CPULinxState *env, uint64_t cur_pc,
                                    uint64_t next_pc, uint32_t reg_begin,
                                    uint32_t reg_end, uint64_t stacksize)
{
    linx_template_fret_stk_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                                stacksize, false);
    g_assert_not_reached();
}

void HELPER(linx_template_fret_stk_chain)(CPULinxState *env, uint64_t cur_pc,
                                          uint64_t next_pc,
                                          uint32_t reg_begin,
                                          uint32_t reg_end,
                                          uint64_t stacksize)
{
    linx_template_fret_stk_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                                stacksize, true);
}

static void linx_template_fret_ra_impl(CPULinxState *env, uint64_t cur_pc,
                                       uint64_t next_pc, uint32_t reg_begin,
                                       uint32_t reg_end, uint64_t stacksize,
                                       bool chain)
{
    CPUState *cs = env_cpu(env);
    const uint64_t adj = stacksize + linx_callframe_size;
    const uint64_t restore_base = linx_callframe_size;
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    const uint64_t retRa = env->gpr[LINX_REG_RA];
    const uint64_t old_sp = env->gpr[LINX_REG_SP];
    const uint64_t new_sp = old_sp + adj;
    uint32_t regs[LINX_GPR_COUNT];
    uint64_t addrs[LINX_GPR_COUNT];
    uint64_t values[LINX_GPR_COUNT];
    const bool frame_stats = unlikely(linx_frame_stats_enabled_p());
    int restore_host_loads = 0;
    int restore_fallback_loads = 0;
    const int restore_count =
        linx_frame_restore_prepare(env, stacksize, new_sp, restore_base,
                                   begin, end, regs, addrs, values,
                                   &restore_host_loads,
                                   &restore_fallback_loads);
    linx_frame_shape_hot_record(env, LINX_TEMPLATE_FRET_RA, reg_begin, reg_end,
                                stacksize, restore_count);

    if (frame_stats) {
        linx_frame_stat_fret_ra_calls++;
        linx_frame_stat_restore_slots += restore_count;
        linx_frame_stat_restore_host_loads += restore_host_loads;
        linx_frame_stat_restore_fallback_loads += restore_fallback_loads;
    }

    if (adj) {
        env->gpr[LINX_REG_SP] = new_sp;
        linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
    }

    linx_frame_restore_commit(env, cur_pc, regs, addrs, values,
                              restore_count);

    const bool ret_fast_hit = linx_bstart_cache_fast_hit(env, retRa);
    if (frame_stats) {
        if (ret_fast_hit) {
            linx_frame_stat_ret_fast_hits++;
        } else {
            linx_frame_stat_ret_checks++;
        }
    }
    if (!ret_fast_hit) {
        HELPER(linx_check_bstart_target)(env, retRa);
    }
    linx_template_clear(env);
    env->pc = retRa;
    linx_template_commit_or_chain(env, cs, env->pc, chain);
}

void HELPER(linx_template_fret_ra)(CPULinxState *env, uint64_t cur_pc,
                                   uint64_t next_pc, uint32_t reg_begin,
                                   uint32_t reg_end, uint64_t stacksize)
{
    linx_template_fret_ra_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                               stacksize, false);
    g_assert_not_reached();
}

void HELPER(linx_template_fret_ra_chain)(CPULinxState *env, uint64_t cur_pc,
                                         uint64_t next_pc, uint32_t reg_begin,
                                         uint32_t reg_end, uint64_t stacksize)
{
    linx_template_fret_ra_impl(env, cur_pc, next_pc, reg_begin, reg_end,
                               stacksize, true);
}

void HELPER(linx_template_step)(CPULinxState *env, uint32_t kind,
                                uint64_t cur_pc, uint64_t next_pc,
                                uint32_t op0, uint32_t op1, uint64_t op2)
{
    CPUState *cs = env_cpu(env);

    if (env->tmpl_pc != cur_pc || env->tmpl_kind != kind) {
        env->tmpl_pc = cur_pc;
        env->tmpl_kind = kind;
        env->tmpl_step = 0;
        env->tmpl_reg_cur = 0;
        env->tmpl_reg_begin = 0;
        env->tmpl_reg_end = 0;
        env->tmpl_stacksize = 0;
        env->tmpl_mem_dst = 0;
        env->tmpl_mem_src = 0;
        env->tmpl_mem_remaining = 0;
        env->tmpl_mem_value = 0;

        switch (kind) {
        case LINX_TEMPLATE_MCOPY: {
            const uint32_t dst_reg = op0;
            const uint32_t src_reg = op1;
            const uint32_t size_reg = (uint32_t)op2;
            if (dst_reg >= LINX_GPR_COUNT || src_reg >= LINX_GPR_COUNT ||
                size_reg >= LINX_GPR_COUNT) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            env->tmpl_mem_dst = env->gpr[dst_reg];
            env->tmpl_mem_src = env->gpr[src_reg];
            env->tmpl_mem_remaining = env->gpr[size_reg];
            break;
        }

        case LINX_TEMPLATE_MSET: {
            const uint32_t dst_reg = op0;
            const uint32_t val_reg = op1;
            const uint32_t size_reg = (uint32_t)op2;
            if (dst_reg >= LINX_GPR_COUNT || val_reg >= LINX_GPR_COUNT ||
                size_reg >= LINX_GPR_COUNT) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            env->tmpl_mem_dst = env->gpr[dst_reg];
            env->tmpl_mem_value = env->gpr[val_reg] & 0xffu;
            env->tmpl_mem_remaining = env->gpr[size_reg];
            break;
        }

        case LINX_TEMPLATE_ESAVE:
        case LINX_TEMPLATE_ERCOV: {
            const uint32_t base_reg = op0;
            const uint32_t len_reg = op1;
            const uint32_t kind_reg = (uint32_t)op2;
            if (base_reg >= LINX_GPR_COUNT || len_reg >= LINX_GPR_COUNT ||
                kind_reg >= LINX_GPR_COUNT) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            env->tmpl_mem_dst = env->gpr[base_reg];
            env->tmpl_mem_remaining = env->gpr[len_reg];
            env->tmpl_mem_value = env->gpr[kind_reg];
            break;
        }

        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
    }

    switch (kind) {
    case LINX_TEMPLATE_MCOPY: {
        uint64_t dst = env->tmpl_mem_dst;
        uint64_t src = env->tmpl_mem_src;
        uint64_t remaining = env->tmpl_mem_remaining;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_trace_mem_clear(env);
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /*
         * One restartable step per helper invocation so commit-tracing can
         * treat each step like a single committed micro-op.
         *
         * Trace convention: record the destination store only (the source read
         * is internal and not representable in the single mem_* slot schema).
         */
        uint32_t sz = 1;
        if (remaining >= 8) {
            sz = 8;
        } else if (remaining >= 4) {
            sz = 4;
        } else if (remaining >= 2) {
            sz = 2;
        }

        uint64_t v = 0;
        switch (sz) {
        case 8:
            v = cpu_ldq_le_data(env, (abi_ptr)src);
            cpu_stq_le_data(env, (abi_ptr)dst, v);
            break;
        case 4:
            v = cpu_ldl_le_data(env, (abi_ptr)src);
            cpu_stl_le_data(env, (abi_ptr)dst, (uint32_t)v);
            break;
        case 2:
            v = cpu_lduw_le_data(env, (abi_ptr)src);
            cpu_stw_le_data(env, (abi_ptr)dst, (uint16_t)v);
            break;
        default:
            v = cpu_ldub_data(env, (abi_ptr)src);
            cpu_stb_data(env, (abi_ptr)dst, (uint8_t)v);
            break;
        }
        linx_trace_mem(env, true, dst, v, 0, sz);

        src += sz;
        dst += sz;
        remaining -= sz;
        env->tmpl_mem_src = src;
        env->tmpl_mem_dst = dst;
        env->tmpl_mem_remaining = remaining;
        env->tmpl_step += sz;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_trace_mem_clear(env);
            linx_template_commit_and_exit(env, cs, env->pc);
        } else {
            env->pc = cur_pc;
            linx_template_exit_without_commit(env, cs);
        }
        break;
    }

    case LINX_TEMPLATE_MSET: {
        uint64_t dst = env->tmpl_mem_dst;
        uint64_t remaining = env->tmpl_mem_remaining;
        const uint8_t v = (uint8_t)env->tmpl_mem_value;
        const int mmu_idx = linx_env_mmu_index(env);

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_trace_mem_clear(env);
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        if (linx_trace_capture_active(env)) {
            uint32_t sz = 1;
            if (remaining >= 8) {
                sz = 8;
            } else if (remaining >= 4) {
                sz = 4;
            } else if (remaining >= 2) {
                sz = 2;
            }

            uint64_t pat = 0;
            for (uint32_t i = 0; i < sz; i++) {
                pat |= (uint64_t)v << (i * 8u);
            }
            switch (sz) {
            case 8:
                cpu_stq_le_data(env, (abi_ptr)dst, pat);
                break;
            case 4:
                cpu_stl_le_data(env, (abi_ptr)dst, (uint32_t)pat);
                break;
            case 2:
                cpu_stw_le_data(env, (abi_ptr)dst, (uint16_t)pat);
                break;
            default:
                cpu_stb_data(env, (abi_ptr)dst, (uint8_t)pat);
                break;
            }
            linx_trace_mem(env, true, dst, pat, 0, sz);

            dst += sz;
            remaining -= sz;
            env->tmpl_mem_dst = dst;
            env->tmpl_mem_remaining = remaining;
            env->tmpl_step += sz;
        } else {
            uint64_t page_left = TARGET_PAGE_SIZE - (dst & (TARGET_PAGE_SIZE - 1));
            uint64_t sz;
            void *host;

            if (page_left == 0) {
                page_left = TARGET_PAGE_SIZE;
            }
            sz = MIN(remaining, page_left);

            host = tlb_vaddr_to_host(env, (vaddr)dst, MMU_DATA_STORE, mmu_idx);
#ifndef CONFIG_USER_ONLY
            if (unlikely(!host)) {
                cpu_stb_mmuidx_ra(env, (abi_ptr)dst, v, mmu_idx, GETPC());
                sz = 1;
            } else
#endif
            {
                set_helper_retaddr(GETPC());
                memset(host, v, sz);
                clear_helper_retaddr();
            }

            dst += sz;
            remaining -= sz;
            env->tmpl_mem_dst = dst;
            env->tmpl_mem_remaining = remaining;
            env->tmpl_step += sz;
        }

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_trace_mem_clear(env);
            linx_template_commit_and_exit(env, cs, env->pc);
        } else {
            env->pc = cur_pc;
            linx_template_exit_without_commit(env, cs);
        }
        break;
    }

    case LINX_TEMPLATE_ESAVE:
    case LINX_TEMPLATE_ERCOV: {
        const uint64_t base = env->tmpl_mem_dst;
        uint64_t remaining = env->tmpl_mem_remaining;
        const uint64_t ext_kind = env->tmpl_mem_value;
        uint64_t off = env->tmpl_step;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_trace_mem_clear(env);
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        const uint64_t addr = base + off;
        uint8_t byte = 0;

        /*
         * Bring-up ext-context blob (64 bytes, little-endian fields):
         *  [0..7]   magic "LINX_EXT"
         *  [8..15]  ext_kind (operand RegSrc2)
         *  [16..39] LB0/LB1/LB2 (u64 each)
         *  [40..63] LC0/LC1/LC2 (u64 each)
         *
         * Bytes beyond 64 are zero on ESAVE and ignored on ERCOV.
         *
         * Use a byte-at-a-time restartable transfer to keep fault/interrupt
         * restart semantics deterministic (idempotent on restart).
         */
        if (kind == LINX_TEMPLATE_ESAVE) {
            byte = linx_extctx_byte(env, ext_kind, off);
            cpu_stb_data(env, (abi_ptr)addr, byte);
            linx_trace_mem(env, true, addr, byte, 0, 1);
        } else {
            byte = cpu_ldub_data(env, (abi_ptr)addr);
            linx_trace_mem(env, false, addr, 0, byte, 1);
            linx_extctx_write_byte(env, off, byte);
        }

        off += 1;
        remaining -= 1;
        env->tmpl_step = (uint32_t)off;
        env->tmpl_mem_remaining = remaining;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_trace_mem_clear(env);
            linx_template_commit_and_exit(env, cs, env->pc);
        } else {
            env->pc = cur_pc;
            linx_template_exit_without_commit(env, cs);
        }
        break;
    }

    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        break;
    }

    g_assert_not_reached();
}

enum {
    LINX_TRAPCAUSE_CAT_IOMMU_PF = 3,
    LINX_TRAPCAUSE_ACC_LOAD    = 0,
    LINX_TRAPCAUSE_ACC_STORE   = 1,
};

static inline bool linx_iova_is_canonical(uint64_t va)
{
    const uint64_t top = (va >> 48) & 0xffffu;
    const uint64_t sign = (va >> 47) & 1u;
    return top == (sign ? 0xffffu : 0x0000u);
}

static bool linx_iommu_translate(CPULinxState *env, uint64_t iova,
                                 bool is_store, hwaddr *pa_out)
{
    const uint64_t iotcr = env->ssr_acr[1][LINX_SSR_IOTCR];
    const bool ime = (iotcr & 1u) != 0;

    if (!ime) {
        /* Bring-up: identity translation, with the NOMMU physical mask. */
        *pa_out = (hwaddr)(iova & 0x1fffffffULL);
        return true;
    }

    if (!linx_iova_is_canonical(iova)) {
        return false;
    }

    /* v0.2 bring-up subset: only 48-bit IOVA supported (SZ must be 16). */
    const uint32_t sz = (uint32_t)((iotcr >> 1) & 0x3fu);
    if (sz != 16) {
        return false;
    }

    const uint64_t iottbr = env->ssr_acr[1][LINX_SSR_IOTTBR];
    if ((iottbr & 0xfffu) != 0) {
        return false;
    }

    hwaddr table = (hwaddr)(iottbr & 0x0000fffffffff000ULL);

    for (int level = 0; level < 4; level++) {
        const uint32_t shift = 39u - (uint32_t)level * 9u;
        const uint64_t idx = (iova >> shift) & 0x1ffu;
        const hwaddr desc_addr = table + (hwaddr)(idx * 8u);
        MemTxResult result = MEMTX_OK;
        const uint64_t desc = address_space_ldq_le(&address_space_memory, desc_addr,
                                                   MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            return false;
        }

        const uint32_t type = (uint32_t)(desc & 0x3u);
        if (type == 0) {
            return false;
        }

        if (type == 3) {
            /* Table descriptor. */
            if ((desc & 0xffcULL) != 0) {
                return false;
            }
            if ((desc >> 48) != 0) {
                return false;
            }
            table = (hwaddr)(desc & 0x0000fffffffff000ULL);
            continue;
        }

        /* Leaf descriptor: Page at L3, Block at L1/L2 (optional). */
        if (level == 0) {
            return false;
        }

        hwaddr block_size = TARGET_PAGE_SIZE;
        if (type == 2) {
            if (level == 1) {
                block_size = (hwaddr)1ull << 30; /* 1 GiB */
            } else if (level == 2) {
                block_size = (hwaddr)1ull << 21; /* 2 MiB */
            } else {
                return false;
            }
        } else if (type == 1) {
            if (level != 3) {
                return false;
            }
        } else {
            return false;
        }

        const hwaddr out_base = (hwaddr)(desc & 0x0000fffffffff000ULL);
        if ((desc >> 48) != 0) {
            return false;
        }
        if ((out_base & (block_size - 1u)) != 0) {
            return false;
        }
        if ((desc & (3ull << 10)) != 0) {
            return false;
        }
        const uint32_t attridx = (uint32_t)((desc >> 7) & 0x7u);
        if (attridx > 2u) {
            return false;
        }
        const bool af = ((desc >> 6) & 1u) != 0;
        if (!af) {
            return false;
        }

        const bool w = ((desc >> 3) & 1u) != 0;
        const bool r = ((desc >> 2) & 1u) != 0;

        if (is_store && !w) {
            return false;
        }
        if (!is_store && !r) {
            return false;
        }

        const hwaddr pa = out_base | (hwaddr)(iova & (uint64_t)(block_size - 1u));
        if (((uint64_t)pa >> 48) != 0) {
            return false;
        }
        *pa_out = pa;
        return true;
    }

    return false;
}

static inline uint32_t linx_tile_mem_read(CPULinxState *env, uint64_t addr,
                                          unsigned elem_bytes)
{
    hwaddr pa;
    if (!linx_iommu_translate(env, addr, false, &pa)) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_LOAD);
        helper_raise_exception(env, LINX_EXCP_LOAD_ACCESS_FAULT);
    }

    MemTxResult result = MEMTX_OK;
    uint32_t v = 0;
    switch (elem_bytes) {
    case 1u:
        v = address_space_ldub(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &result);
        break;
    case 2u:
        v = address_space_lduw_le(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &result);
        break;
    case 4u:
        v = address_space_ldl_le(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &result);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }
    if (result != MEMTX_OK) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_LOAD);
        helper_raise_exception(env, LINX_EXCP_LOAD_ACCESS_FAULT);
    }
    return v;
}

static inline void linx_tile_mem_write(CPULinxState *env, uint64_t addr,
                                       unsigned elem_bytes, uint32_t v)
{
    hwaddr pa;
    if (!linx_iommu_translate(env, addr, true, &pa)) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_STORE);
        helper_raise_exception(env, LINX_EXCP_STORE_ACCESS_FAULT);
    }

    MemTxResult result = MEMTX_OK;
    switch (elem_bytes) {
    case 1u:
        address_space_stb(&address_space_memory, pa, v & 0xffu,
                          MEMTXATTRS_UNSPECIFIED, &result);
        break;
    case 2u:
        address_space_stw_le(&address_space_memory, pa, v & 0xffffu,
                             MEMTXATTRS_UNSPECIFIED, &result);
        break;
    case 4u:
        address_space_stl_le(&address_space_memory, pa, v,
                             MEMTXATTRS_UNSPECIFIED, &result);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (result != MEMTX_OK) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_STORE);
        helper_raise_exception(env, LINX_EXCP_STORE_ACCESS_FAULT);
    }
}

static inline uint32_t linx_tile_mem_cmpxchg(CPULinxState *env, uint64_t addr,
                                             unsigned elem_bytes,
                                             uint32_t cmpv, uint32_t newv)
{
    const uint32_t old = linx_tile_mem_read(env, addr, elem_bytes);
    uint32_t mask = UINT32_MAX;

    if (elem_bytes == 1u) {
        mask = 0xffu;
    } else if (elem_bytes == 2u) {
        mask = 0xffffu;
    }
    if ((old & mask) == (cmpv & mask)) {
        linx_tile_mem_write(env, addr, elem_bytes, newv & mask);
    }
    return old & mask;
}

static inline bool linx_tile_set_elem(CPULinxState *env, unsigned tile,
                                      uint32_t elem_idx, unsigned elem_bytes,
                                      uint32_t value)
{
    uint8_t *buf = (uint8_t *)env->tile_reg[tile];
    const size_t off = (size_t)elem_idx * elem_bytes;
    if (off + elem_bytes > LINX_TILE_MAX_BYTES) {
        return false;
    }
    switch (elem_bytes) {
    case 1u:
        buf[off] = (uint8_t)(value & 0xffu);
        return true;
    case 2u:
        stw_le_p(buf + off, value & 0xffffu);
        return true;
    case 4u:
        stl_le_p(buf + off, value);
        return true;
    default:
        return false;
    }
}

static inline bool linx_tile_get_elem(const CPULinxState *env, unsigned tile,
                                      uint32_t elem_idx, unsigned elem_bytes,
                                      uint32_t *value_out)
{
    const uint8_t *buf = (const uint8_t *)env->tile_reg[tile];
    const size_t off = (size_t)elem_idx * elem_bytes;
    if (off + elem_bytes > LINX_TILE_MAX_BYTES) {
        return false;
    }
    switch (elem_bytes) {
    case 1u:
        *value_out = (uint32_t)buf[off];
        return true;
    case 2u:
        *value_out = (uint32_t)lduw_le_p(buf + off);
        return true;
    case 4u:
        *value_out = ldl_le_p(buf + off);
        return true;
    default:
        return false;
    }
}

static inline bool linx_tile_get_elem64(const CPULinxState *env, unsigned tile,
                                        uint32_t elem_idx, unsigned elem_bytes,
                                        uint64_t *value_out)
{
    const uint8_t *buf = (const uint8_t *)env->tile_reg[tile];
    const size_t off = (size_t)elem_idx * elem_bytes;
    if (off + elem_bytes > LINX_TILE_MAX_BYTES) {
        return false;
    }
    switch (elem_bytes) {
    case 2u:
        *value_out = (uint64_t)lduw_le_p(buf + off);
        return true;
    case 4u:
        *value_out = (uint64_t)ldl_le_p(buf + off);
        return true;
    case 8u:
        *value_out = ldq_le_p(buf + off);
        return true;
    default:
        return false;
    }
}

static inline float linx_tile_word_as_f32(uint32_t word)
{
    union {
        uint32_t u;
        float f;
    } cvt = { .u = word };
    return cvt.f;
}

static inline uint32_t linx_tile_f32_as_word(float value)
{
    union {
        float f;
        uint32_t u;
    } cvt = { .f = value };
    return cvt.u;
}

static inline bool linx_tile_dtype_is_signed(uint32_t dtype)
{
    const uint32_t dt = dtype & 0x1fu;
    return dt == 16u || dt == 17u || dt == 18u || dt == 19u;
}

static inline int32_t linx_tile_sign_extend(uint32_t value,
                                            unsigned elem_bytes)
{
    if (elem_bytes == 1u) {
        return (int8_t)value;
    }
    if (elem_bytes == 2u) {
        return (int16_t)value;
    }
    return (int32_t)value;
}

static inline float linx_tile_value_as_f32(uint32_t value, uint32_t dtype,
                                           unsigned elem_bytes)
{
    if ((dtype & 0x1fu) == 1u && elem_bytes == 4u) {
        return linx_tile_word_as_f32(value);
    }
    if (linx_tile_dtype_is_signed(dtype)) {
        return (float)linx_tile_sign_extend(value, elem_bytes);
    }
    return (float)value;
}

static inline uint32_t linx_tile_f32_as_dtype(float value, uint32_t dtype,
                                              unsigned elem_bytes)
{
    if ((dtype & 0x1fu) == 1u && elem_bytes == 4u) {
        return linx_tile_f32_as_word(value);
    }
    if (linx_tile_dtype_is_signed(dtype)) {
        if (elem_bytes == 1u) {
            value = MAX(-128.0f, MIN(127.0f, value));
            return (uint8_t)(int8_t)lrintf(value);
        }
        if (elem_bytes == 2u) {
            value = MAX(-32768.0f, MIN(32767.0f, value));
            return (uint16_t)(int16_t)lrintf(value);
        }
        return (uint32_t)(int32_t)lrintf(value);
    }
    if (elem_bytes == 1u) {
        value = MAX(0.0f, MIN(255.0f, value));
        return (uint8_t)lrintf(value);
    }
    if (elem_bytes == 2u) {
        value = MAX(0.0f, MIN(65535.0f, value));
        return (uint16_t)lrintf(value);
    }
    return (uint32_t)llrintf(value);
}

static inline uint32_t linx_tile_scalar_as_dtype(uint64_t scalar,
                                                 uint32_t dtype,
                                                 unsigned elem_bytes)
{
    if ((dtype & 0x1fu) == 1u) {
        return (uint32_t)scalar;
    }
    if (elem_bytes == 1u) {
        return (uint8_t)scalar;
    }
    if (elem_bytes == 2u) {
        return (uint16_t)scalar;
    }
    return (uint32_t)scalar;
}

static inline uint32_t linx_tile_tepl_binary_word(CPULinxState *env,
                                                  uint32_t op, uint32_t dtype,
                                                  uint32_t lhs, uint32_t rhs)
{
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);

    if ((dtype & 0x1fu) == 1u) {
        const float a = linx_tile_word_as_f32(lhs);
        const float b = linx_tile_word_as_f32(rhs);
        float out = 0.0f;

        switch (op) {
        case 0x000u:
        case 0x020u:
            out = a + b;
            break;
        case 0x001u:
        case 0x021u:
            out = a - b;
            break;
        case 0x002u:
        case 0x022u:
            out = a * b;
            break;
        case 0x003u:
        case 0x023u:
            out = b == 0.0f ? 0.0f : a / b;
            break;
        case 0x004u:
        case 0x024u:
            out = a > b ? a : b;
            break;
        case 0x005u:
        case 0x025u:
            out = a < b ? a : b;
            break;
        case 0x030u:
        case 0x032u:
            if (b == 0.0f) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            out = a - floorf(a / b) * b;
            break;
        default:
            return 0;
        }
        return linx_tile_f32_as_word(out);
    }
    if ((dtype & 0x1fu) == 2u) {
        const float16 a = make_float16((uint16_t)lhs);
        const float16 b = make_float16((uint16_t)rhs);
        float16 out;

        switch (op) {
        case 0x000u:
        case 0x020u:
            out = float16_add(a, b, &env->fp_status);
            break;
        case 0x001u:
        case 0x021u:
            out = float16_sub(a, b, &env->fp_status);
            break;
        case 0x002u:
        case 0x022u:
            out = float16_mul(a, b, &env->fp_status);
            break;
        case 0x003u:
        case 0x023u:
            out = float16_div(a, b, &env->fp_status);
            break;
        case 0x004u:
        case 0x024u:
            out = float16_max(a, b, &env->fp_status);
            break;
        case 0x005u:
        case 0x025u:
            out = float16_min(a, b, &env->fp_status);
            break;
        default:
            return 0u;
        }
        return float16_val(out);
    }
    if ((dtype & 0x1fu) == 6u) {
        const bfloat16 a = (bfloat16)(uint16_t)lhs;
        const bfloat16 b = (bfloat16)(uint16_t)rhs;
        bfloat16 out;

        switch (op) {
        case 0x000u:
        case 0x020u:
            out = bfloat16_add(a, b, &env->fp_status);
            break;
        case 0x001u:
        case 0x021u:
            out = bfloat16_sub(a, b, &env->fp_status);
            break;
        case 0x002u:
        case 0x022u:
            out = bfloat16_mul(a, b, &env->fp_status);
            break;
        case 0x003u:
        case 0x023u:
            out = bfloat16_div(a, b, &env->fp_status);
            break;
        case 0x004u:
        case 0x024u:
            out = bfloat16_max(a, b, &env->fp_status);
            break;
        case 0x005u:
        case 0x025u:
            out = bfloat16_min(a, b, &env->fp_status);
            break;
        default:
            return 0u;
        }
        return out;
    }

    switch (op) {
    case 0x000u:
    case 0x020u:
        return lhs + rhs;
    case 0x001u:
    case 0x021u:
        return lhs - rhs;
    case 0x002u:
    case 0x022u:
        return lhs * rhs;
    case 0x003u:
    case 0x023u:
        if (rhs == 0u) {
            return 0u;
        }
        if (linx_tile_dtype_is_signed(dtype)) {
            const int64_t a = linx_tile_sign_extend(lhs, elem_bytes);
            const int64_t b = linx_tile_sign_extend(rhs, elem_bytes);
            return b == 0 ? 0u : (uint32_t)(a / b);
        }
        return lhs / rhs;
    case 0x004u:
    case 0x024u:
        return linx_tile_dtype_is_signed(dtype)
                   ? (linx_tile_sign_extend(lhs, elem_bytes) >
                              linx_tile_sign_extend(rhs, elem_bytes)
                          ? lhs
                          : rhs)
                   : (lhs > rhs ? lhs : rhs);
    case 0x005u:
    case 0x025u:
        return linx_tile_dtype_is_signed(dtype)
                   ? (linx_tile_sign_extend(lhs, elem_bytes) <
                              linx_tile_sign_extend(rhs, elem_bytes)
                          ? lhs
                          : rhs)
                   : (lhs < rhs ? lhs : rhs);
    case 0x006u:
    case 0x026u:
        return lhs & rhs;
    case 0x007u:
    case 0x027u:
        return lhs | rhs;
    case 0x008u:
    case 0x028u:
        return lhs ^ rhs;
    case 0x009u:
    case 0x029u:
        return lhs << (rhs & 31u);
    case 0x00au:
    case 0x02au:
        return linx_tile_dtype_is_signed(dtype)
                   ? (uint32_t)(linx_tile_sign_extend(lhs, elem_bytes) >>
                                (rhs & 31u))
                   : lhs >> (rhs & 31u);
    case 0x030u:
    case 0x032u:
        if (rhs == 0u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        }
        if (linx_tile_dtype_is_signed(dtype)) {
            const int64_t a = linx_tile_sign_extend(lhs, elem_bytes);
            const int64_t b = linx_tile_sign_extend(rhs, elem_bytes);
            int64_t rem = a % b;
            if (rem != 0 && ((rem < 0) != (b < 0))) {
                rem += b;
            }
            return (uint32_t)rem;
        }
        return lhs % rhs;
    default:
        return 0;
    }
}

static bool linx_tile_tcmp_lane(CPULinxState *env, uint32_t dtype,
                                uint32_t lhs, uint32_t rhs, uint32_t mode,
                                bool *result)
{
    bool eq;
    bool lt;
    bool gt;

    switch (dtype & 0x1fu) {
    case 1u: { /* FP32 */
        const float a = linx_tile_word_as_f32(lhs);
        const float b = linx_tile_word_as_f32(rhs);
        eq = a == b;
        lt = a < b;
        gt = a > b;
        break;
    }
    case 2u: { /* FP16 */
        const float16 a = make_float16((uint16_t)lhs);
        const float16 b = make_float16((uint16_t)rhs);
        eq = float16_eq_quiet(a, b, &env->fp_status);
        lt = float16_lt_quiet(a, b, &env->fp_status);
        gt = float16_lt_quiet(b, a, &env->fp_status);
        break;
    }
    case 17u: /* INT32 */
    case 18u: /* INT16 */
    case 19u: /* INT8 */
        eq = linx_tile_sign_extend(lhs, linx_tile_dtype_elem_bytes(dtype)) ==
             linx_tile_sign_extend(rhs, linx_tile_dtype_elem_bytes(dtype));
        lt = linx_tile_sign_extend(lhs, linx_tile_dtype_elem_bytes(dtype)) <
             linx_tile_sign_extend(rhs, linx_tile_dtype_elem_bytes(dtype));
        gt = linx_tile_sign_extend(lhs, linx_tile_dtype_elem_bytes(dtype)) >
             linx_tile_sign_extend(rhs, linx_tile_dtype_elem_bytes(dtype));
        break;
    case 25u: /* UINT32 */
    case 26u: /* UINT16 */
    case 27u: /* UINT8 */
        eq = lhs == rhs;
        lt = lhs < rhs;
        gt = lhs > rhs;
        break;
    default:
        return false;
    }

    switch (mode) {
    case 0u: /* EQ */
        *result = eq;
        return true;
    case 1u: /* NE */
        *result = !eq;
        return true;
    case 2u: /* LT */
        *result = lt;
        return true;
    case 3u: /* LE */
        *result = lt || eq;
        return true;
    case 4u: /* GT */
        *result = gt;
        return true;
    case 5u: /* GE */
        *result = gt || eq;
        return true;
    default:
        return false;
    }
}

static bool linx_tile_tepl_tcmp(CPULinxState *env, unsigned dst_tile,
                                unsigned src0_tile, unsigned src1_tile,
                                uint32_t rows, uint32_t cols,
                                uint32_t physical_cols, uint32_t bytes)
{
    const uint32_t dtype = env->tile_reg_dtype[src0_tile] & 0x1fu;
    const unsigned elem_bytes = env->tile_reg_elem_bytes[src0_tile];
    const uint32_t mode = (env->tile_attr_raw >> 22) & 0x7u;
    const uint32_t mask_words_per_row = (cols + 31u) / 32u;
    const uint64_t mask_bytes =
        (uint64_t)rows * mask_words_per_row * sizeof(uint32_t);

    if ((env->tile_dtype & 0x1fu) != dtype ||
        env->tile_reg_dtype[src1_tile] != env->tile_reg_dtype[src0_tile] ||
        env->tile_reg_elem_bytes[src1_tile] != elem_bytes || mode > 5u ||
        mask_bytes > bytes ||
        (uint64_t)rows * physical_cols * elem_bytes >
            env->tile_reg_bytes[src0_tile] ||
        (uint64_t)rows * physical_cols * elem_bytes >
            env->tile_reg_bytes[src1_tile]) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            const uint32_t src_lane = r * physical_cols + c;
            const uint32_t mask_word = r * mask_words_per_row + c / 32u;
            uint32_t lhs = 0;
            uint32_t rhs = 0;
            bool lane_result = false;

            if (!linx_tile_get_elem(env, src0_tile, src_lane, elem_bytes,
                                    &lhs) ||
                !linx_tile_get_elem(env, src1_tile, src_lane, elem_bytes,
                                    &rhs) ||
                !linx_tile_tcmp_lane(env, dtype, lhs, rhs, mode,
                                     &lane_result)) {
                return false;
            }
            if (lane_result) {
                uint32_t packed = ldl_le_p(
                    (uint8_t *)env->tile_reg[dst_tile] + mask_word * 4u);
                packed |= 1u << (c & 31u);
                stl_le_p((uint8_t *)env->tile_reg[dst_tile] + mask_word * 4u,
                         packed);
            }
        }
    }

    env->tile_reg_bytes[dst_tile] = bytes;
    linx_tile_set_elem_bytes(env, dst_tile, sizeof(uint32_t));
    linx_tile_set_dtype(env, dst_tile, 25u); /* packed predicate is U32 */
    if (!linx_tile_set_shape(env, dst_tile, mask_words_per_row, rows,
                             mask_words_per_row, rows)) {
        return false;
    }
    return true;
}

static bool linx_tile_tepl_tcmps(CPULinxState *env, unsigned dst_tile,
                                 unsigned src_tile, uint32_t scalar,
                                 uint32_t rows, uint32_t cols,
                                 uint32_t physical_cols, uint32_t bytes)
{
    const uint32_t dtype = env->tile_reg_dtype[src_tile] & 0x1fu;
    const unsigned elem_bytes = env->tile_reg_elem_bytes[src_tile];
    const uint32_t mode = (env->tile_attr_raw >> 22) & 0x7u;
    const uint32_t mask_words_per_row = (cols + 31u) / 32u;
    const uint64_t mask_bytes =
        (uint64_t)rows * mask_words_per_row * sizeof(uint32_t);

    if ((env->tile_dtype & 0x1fu) != dtype || mode > 5u ||
        mask_bytes > bytes ||
        (uint64_t)rows * physical_cols * elem_bytes >
            env->tile_reg_bytes[src_tile]) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            const uint32_t src_lane = r * physical_cols + c;
            const uint32_t mask_word = r * mask_words_per_row + c / 32u;
            uint32_t lhs = 0;
            bool lane_result = false;

            if (!linx_tile_get_elem(env, src_tile, src_lane, elem_bytes,
                                    &lhs) ||
                !linx_tile_tcmp_lane(env, dtype, lhs, scalar, mode,
                                     &lane_result)) {
                return false;
            }
            if (lane_result) {
                uint32_t packed = ldl_le_p(
                    (uint8_t *)env->tile_reg[dst_tile] + mask_word * 4u);
                packed |= 1u << (c & 31u);
                stl_le_p((uint8_t *)env->tile_reg[dst_tile] + mask_word * 4u,
                         packed);
            }
        }
    }

    env->tile_reg_bytes[dst_tile] = bytes;
    linx_tile_set_elem_bytes(env, dst_tile, sizeof(uint32_t));
    linx_tile_set_dtype(env, dst_tile, 25u); /* packed predicate is U32 */
    return linx_tile_set_shape(env, dst_tile, mask_words_per_row, rows,
                               mask_words_per_row, rows);
}

static bool linx_tile_tepl_select(CPULinxState *env, unsigned dst_tile,
                                  unsigned mask_tile, unsigned src0_tile,
                                  unsigned src1_tile, bool scalar_false,
                                  uint32_t scalar, uint32_t rows,
                                  uint32_t cols, uint32_t physical_cols)
{
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(env->tile_dtype);
    const uint32_t mask_words_per_row = (cols + 31u) / 32u;
    const uint32_t mask_physical_cols = env->tile_reg_cols[mask_tile];

    if ((env->tile_reg_dtype[mask_tile] & 0x1fu) != 25u ||
        env->tile_reg_elem_bytes[mask_tile] != sizeof(uint32_t) ||
        env->tile_reg_valid_cols[mask_tile] < mask_words_per_row ||
        env->tile_reg_valid_rows[mask_tile] < rows ||
        mask_physical_cols < mask_words_per_row ||
        env->tile_reg_rows[mask_tile] < rows ||
        (uint64_t)rows * mask_physical_cols * sizeof(uint32_t) >
            env->tile_reg_bytes[mask_tile] ||
        env->tile_reg_elem_bytes[src0_tile] != elem_bytes ||
        (env->tile_reg_dtype[src0_tile] & 0x1fu) !=
            (env->tile_dtype & 0x1fu) ||
        (!scalar_false &&
         (env->tile_reg_elem_bytes[src1_tile] != elem_bytes ||
          env->tile_reg_dtype[src1_tile] != env->tile_reg_dtype[src0_tile]))) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            const uint32_t lane = r * physical_cols + c;
            const uint32_t mask_lane =
                r * mask_physical_cols + c / 32u;
            uint32_t mask_word = 0;
            uint32_t value = scalar;

            if (!linx_tile_get_elem(env, mask_tile, mask_lane,
                                    sizeof(uint32_t), &mask_word)) {
                return false;
            }
            if ((mask_word & (1u << (c & 31u))) != 0u) {
                if (!linx_tile_get_elem(env, src0_tile, lane, elem_bytes,
                                        &value)) {
                    return false;
                }
            } else if (!scalar_false &&
                       !linx_tile_get_elem(env, src1_tile, lane, elem_bytes,
                                           &value)) {
                return false;
            }
            if (!linx_tile_set_elem(env, dst_tile, lane, elem_bytes, value)) {
                return false;
            }
        }
    }
    return true;
}

static uint32_t linx_tile_tepl_one(uint32_t dtype)
{
    switch (dtype & 0x1fu) {
    case 1u: /* FP32 */
        return 0x3f800000u;
    case 2u: /* FP16 */
        return 0x3c00u;
    case 6u: /* BF16 */
        return 0x3f80u;
    default:
        return 1u;
    }
}

static bool linx_tile_tepl_product(CPULinxState *env, unsigned dst_tile,
                                   unsigned src_tile, bool row_reduce,
                                   uint32_t rows, uint32_t cols,
                                   uint32_t physical_cols, uint32_t bytes)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    const uint32_t output_count = row_reduce ? rows : cols;

    if ((env->tile_reg_dtype[src_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[src_tile] != elem_bytes ||
        output_count == 0u || bytes < output_count * elem_bytes) {
        return false;
    }

    for (uint32_t output = 0; output < output_count; output++) {
        const uint32_t reduce_count = row_reduce ? cols : rows;
        uint32_t product = linx_tile_tepl_one(dtype);

        for (uint32_t reduce = 0; reduce < reduce_count; reduce++) {
            const uint32_t r = row_reduce ? output : reduce;
            const uint32_t c = row_reduce ? reduce : output;
            uint32_t value = 0;

            if (!linx_tile_get_elem(env, src_tile, r * physical_cols + c,
                                    elem_bytes, &value)) {
                return false;
            }
            product = linx_tile_tepl_binary_word(env, 0x002u, dtype,
                                                 product, value);
        }
        if (!linx_tile_set_elem(env, dst_tile, output, elem_bytes, product)) {
            return false;
        }
    }

    env->tile_reg_bytes[dst_tile] = bytes;
    linx_tile_set_elem_bytes(env, dst_tile, elem_bytes);
    linx_tile_set_dtype(env, dst_tile, dtype);
    if (row_reduce) {
        return linx_tile_set_shape(env, dst_tile, 1u, rows, 1u,
                                   bytes / elem_bytes);
    }
    if (bytes % (cols * elem_bytes) != 0u) {
        return false;
    }
    return linx_tile_set_shape(env, dst_tile, cols, 1u, cols,
                               bytes / (cols * elem_bytes));
}

static bool linx_tile_tepl_arg_reduce(CPULinxState *env, unsigned dst_tile,
                                      unsigned src_tile, bool row_reduce,
                                      bool find_max, uint32_t rows,
                                      uint32_t cols, uint32_t physical_cols,
                                      uint32_t bytes)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    const uint32_t output_count = row_reduce ? rows : cols;

    if ((env->tile_reg_dtype[src_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[src_tile] != elem_bytes ||
        output_count == 0u || bytes < output_count * sizeof(uint32_t)) {
        return false;
    }

    for (uint32_t output = 0; output < output_count; output++) {
        const uint32_t reduce_count = row_reduce ? cols : rows;
        const uint32_t first_r = row_reduce ? output : 0u;
        const uint32_t first_c = row_reduce ? 0u : output;
        uint32_t best_value = 0;
        uint32_t best_index = 0;

        if (!linx_tile_get_elem(env, src_tile,
                                first_r * physical_cols + first_c,
                                elem_bytes, &best_value)) {
            return false;
        }
        for (uint32_t reduce = 1; reduce < reduce_count; reduce++) {
            const uint32_t r = row_reduce ? output : reduce;
            const uint32_t c = row_reduce ? reduce : output;
            uint32_t value = 0;
            bool better = false;

            if (!linx_tile_get_elem(env, src_tile, r * physical_cols + c,
                                    elem_bytes, &value) ||
                !linx_tile_tcmp_lane(env, dtype, value, best_value,
                                     find_max ? 4u : 2u, &better)) {
                return false;
            }
            if (better) {
                best_value = value;
                best_index = reduce;
            }
        }
        if (!linx_tile_set_elem(env, dst_tile, output, sizeof(uint32_t),
                                best_index)) {
            return false;
        }
    }

    env->tile_reg_bytes[dst_tile] = bytes;
    linx_tile_set_elem_bytes(env, dst_tile, sizeof(uint32_t));
    linx_tile_set_dtype(env, dst_tile, 25u);
    if (row_reduce) {
        return linx_tile_set_shape(env, dst_tile, 1u, rows, 1u,
                                   bytes / sizeof(uint32_t));
    }
    if (bytes % (cols * sizeof(uint32_t)) != 0u) {
        return false;
    }
    return linx_tile_set_shape(env, dst_tile, cols, 1u, cols,
                               bytes / (cols * sizeof(uint32_t)));
}

static bool linx_tile_tepl_expand(CPULinxState *env, unsigned dst_tile,
                                  unsigned src0_tile, unsigned src1_tile,
                                  bool pure_expand, bool row_expand,
                                  unsigned expand_op, uint32_t rows,
                                  uint32_t cols, uint32_t physical_cols)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    const unsigned vector_tile = pure_expand ? src0_tile : src1_tile;
    const uint32_t vector_cols = env->tile_reg_cols[vector_tile];

    if ((env->tile_reg_dtype[vector_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[vector_tile] != elem_bytes ||
        vector_cols == 0u ||
        (row_expand &&
         (env->tile_reg_valid_rows[vector_tile] < rows ||
          env->tile_reg_valid_cols[vector_tile] < 1u ||
          env->tile_reg_rows[vector_tile] < rows)) ||
        (!row_expand &&
         (env->tile_reg_valid_rows[vector_tile] < 1u ||
          env->tile_reg_valid_cols[vector_tile] < cols ||
          vector_cols < cols))) {
        return false;
    }
    if (!pure_expand &&
        ((env->tile_reg_dtype[src0_tile] & 0x1fu) != dtype ||
         env->tile_reg_elem_bytes[src0_tile] != elem_bytes)) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            const uint32_t vector_lane = row_expand ? r * vector_cols : c;
            const uint32_t dst_lane = r * physical_cols + c;
            uint32_t expanded = 0;
            uint32_t result = 0;

            if (!linx_tile_get_elem(env, vector_tile, vector_lane, elem_bytes,
                                    &expanded)) {
                return false;
            }
            if (pure_expand) {
                result = expanded;
            } else {
                uint32_t source = 0;
                if (!linx_tile_get_elem(env, src0_tile, dst_lane, elem_bytes,
                                        &source)) {
                    return false;
                }
                if (expand_op < 6u) {
                    result = linx_tile_tepl_binary_word(
                        env, expand_op, dtype, source, expanded);
                } else if (dtype == 1u) {
                    const float difference =
                        linx_tile_word_as_f32(source) -
                        linx_tile_word_as_f32(expanded);
                    result = linx_tile_f32_as_word(expf(difference));
                } else if (dtype == 2u) {
                    const float16 difference = float16_sub(
                        make_float16((uint16_t)source),
                        make_float16((uint16_t)expanded), &env->fp_status);
                    const float32 difference32 = float16_to_float32(
                        difference, true, &env->fp_status);
                    const float value = expf(linx_tile_word_as_f32(
                        float32_val(difference32)));
                    result = float16_val(float32_to_float16(
                        make_float32(linx_tile_f32_as_word(value)), true,
                        &env->fp_status));
                } else {
                    return false;
                }
            }
            if (!linx_tile_set_elem(env, dst_tile, dst_lane, elem_bytes,
                                    result)) {
                return false;
            }
        }
    }
    return true;
}

static bool linx_tile_tepl_fillpad_value(uint32_t dtype, unsigned elem_bytes,
                                         uint32_t pad_mode,
                                         uint32_t *value_out)
{
    const uint32_t dt = dtype & 0x1fu;

    if (pad_mode == 0u || pad_mode == 1u) {
        *value_out = 0u;
        return true;
    }
    if (pad_mode != 2u && pad_mode != 3u) {
        return false;
    }

    if (dt == 1u) { /* FP32 uses positive/negative infinity. */
        *value_out = pad_mode == 2u ? 0x7f800000u : 0xff800000u;
    } else if (dt == 2u) { /* FP16 */
        *value_out = pad_mode == 2u ? 0x7c00u : 0xfc00u;
    } else if (dt == 6u) { /* BF16 */
        *value_out = pad_mode == 2u ? 0x7f80u : 0xff80u;
    } else if (linx_tile_dtype_is_signed(dtype)) {
        *value_out = pad_mode == 2u
                         ? (elem_bytes == 1u ? 0x7fu
                            : elem_bytes == 2u ? 0x7fffu
                                               : 0x7fffffffu)
                         : (elem_bytes == 1u ? 0x80u
                            : elem_bytes == 2u ? 0x8000u
                                               : 0x80000000u);
    } else {
        *value_out = pad_mode == 2u
                         ? (elem_bytes == 1u ? 0xffu
                            : elem_bytes == 2u ? 0xffffu
                                               : 0xffffffffu)
                         : 0u;
    }
    return true;
}

static bool linx_tile_tepl_fillpad(CPULinxState *env, unsigned dst_tile,
                                   unsigned src_tile, uint32_t valid_rows,
                                   uint32_t valid_cols,
                                   uint32_t physical_rows,
                                   uint32_t physical_cols)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    uint32_t pad_value = 0;

    if ((env->tile_reg_dtype[src_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[src_tile] != elem_bytes ||
        !linx_tile_tepl_fillpad_value(dtype, elem_bytes,
                                      env->tile_attr_pad & 0x1fu,
                                      &pad_value)) {
        return false;
    }

    for (uint32_t r = 0; r < physical_rows; r++) {
        for (uint32_t c = 0; c < physical_cols; c++) {
            const uint32_t lane = r * physical_cols + c;
            uint32_t value = pad_value;

            if (r < valid_rows && c < valid_cols &&
                !linx_tile_get_elem(env, src_tile, lane, elem_bytes,
                                    &value)) {
                return false;
            }
            if (!linx_tile_set_elem(env, dst_tile, lane, elem_bytes, value)) {
                return false;
            }
        }
    }
    return true;
}

static bool linx_tile_tepl_partial_binary(CPULinxState *env,
                                          unsigned dst_tile,
                                          unsigned src0_tile,
                                          unsigned src1_tile,
                                          unsigned binary_op,
                                          uint32_t dst_rows,
                                          uint32_t dst_cols,
                                          uint32_t physical_cols)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    const uint32_t src0_rows = env->tile_reg_valid_rows[src0_tile];
    const uint32_t src0_cols = env->tile_reg_valid_cols[src0_tile];
    const uint32_t src1_rows = env->tile_reg_valid_rows[src1_tile];
    const uint32_t src1_cols = env->tile_reg_valid_cols[src1_tile];
    const bool src0_full = src0_rows == dst_rows && src0_cols == dst_cols;
    const bool src1_full = src1_rows == dst_rows && src1_cols == dst_cols;

    if ((env->tile_reg_dtype[src0_tile] & 0x1fu) != dtype ||
        (env->tile_reg_dtype[src1_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[src0_tile] != elem_bytes ||
        env->tile_reg_elem_bytes[src1_tile] != elem_bytes ||
        env->tile_reg_cols[src0_tile] != physical_cols ||
        env->tile_reg_cols[src1_tile] != physical_cols ||
        src0_rows > dst_rows || src0_cols > dst_cols ||
        src1_rows > dst_rows || src1_cols > dst_cols ||
        (!src0_full && !src1_full)) {
        return false;
    }

    for (uint32_t r = 0; r < dst_rows; r++) {
        for (uint32_t c = 0; c < dst_cols; c++) {
            const uint32_t lane = r * physical_cols + c;
            const bool src0_valid = r < src0_rows && c < src0_cols;
            const bool src1_valid = r < src1_rows && c < src1_cols;
            uint32_t src0 = 0;
            uint32_t src1 = 0;
            uint32_t result = 0;

            if (src0_valid &&
                !linx_tile_get_elem(env, src0_tile, lane, elem_bytes,
                                    &src0)) {
                return false;
            }
            if (src1_valid &&
                !linx_tile_get_elem(env, src1_tile, lane, elem_bytes,
                                    &src1)) {
                return false;
            }
            if (src0_valid && src1_valid) {
                result = linx_tile_tepl_binary_word(env, binary_op, dtype,
                                                    src0, src1);
            } else if (src0_valid) {
                result = src0;
            } else if (src1_valid) {
                result = src1;
            } else {
                return false;
            }
            if (!linx_tile_set_elem(env, dst_tile, lane, elem_bytes, result)) {
                return false;
            }
        }
    }
    return true;
}

static bool linx_tile_resolve_ior(const CPULinxState *env, unsigned slot,
                                  unsigned *addr_reg_out);

static bool linx_tile_tepl_concat(CPULinxState *env, unsigned dst_tile,
                                  unsigned src0_tile, unsigned src1_tile,
                                  uint32_t rows, uint32_t cols,
                                  uint32_t physical_cols)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    const uint32_t src0_cols = env->tile_reg_valid_cols[src0_tile];
    const uint32_t src1_cols = env->tile_reg_valid_cols[src1_tile];
    const uint32_t src0_stride = env->tile_reg_cols[src0_tile];
    const uint32_t src1_stride = env->tile_reg_cols[src1_tile];

    if ((env->tile_reg_dtype[src0_tile] & 0x1fu) != dtype ||
        (env->tile_reg_dtype[src1_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[src0_tile] != elem_bytes ||
        env->tile_reg_elem_bytes[src1_tile] != elem_bytes ||
        env->tile_reg_valid_rows[src0_tile] != rows ||
        env->tile_reg_valid_rows[src1_tile] != rows ||
        src0_cols + src1_cols != cols ||
        src0_stride < src0_cols || src1_stride < src1_cols) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            const bool from_src0 = c < src0_cols;
            const unsigned src_tile = from_src0 ? src0_tile : src1_tile;
            const uint32_t src_col = from_src0 ? c : c - src0_cols;
            const uint32_t src_stride = from_src0 ? src0_stride : src1_stride;
            uint32_t value = 0;

            if (!linx_tile_get_elem(env, src_tile,
                                    r * src_stride + src_col,
                                    elem_bytes, &value) ||
                !linx_tile_set_elem(env, dst_tile,
                                    r * physical_cols + c,
                                    elem_bytes, value)) {
                return false;
            }
        }
    }
    return true;
}

static bool linx_tile_tepl_gatherb(CPULinxState *env, unsigned dst_tile,
                                   unsigned src_tile, unsigned offset_tile,
                                   uint32_t rows, uint32_t cols,
                                   uint32_t physical_cols)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    const uint32_t src_bytes = env->tile_reg_bytes[src_tile];
    const uint32_t offset_stride = env->tile_reg_cols[offset_tile];

    if ((env->tile_reg_dtype[src_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[src_tile] != elem_bytes ||
        (env->tile_reg_dtype[offset_tile] & 0x1fu) != 25u ||
        env->tile_reg_elem_bytes[offset_tile] != sizeof(uint32_t) ||
        env->tile_reg_valid_rows[offset_tile] < rows ||
        env->tile_reg_valid_cols[offset_tile] < cols ||
        offset_stride < cols || src_bytes < elem_bytes) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t offset = 0;
            uint32_t value = 0;

            if (!linx_tile_get_elem(env, offset_tile,
                                    r * offset_stride + c,
                                    sizeof(uint32_t), &offset)) {
                return false;
            }
            offset = MIN(offset, src_bytes - elem_bytes);
            memcpy(&value,
                   (const uint8_t *)env->tile_reg[src_tile] + offset,
                   elem_bytes);
            if (!linx_tile_set_elem(env, dst_tile,
                                    r * physical_cols + c,
                                    elem_bytes, value)) {
                return false;
            }
        }
    }
    return true;
}

static bool linx_tile_tepl_extract(CPULinxState *env, unsigned dst_tile,
                                   unsigned src_tile, uint32_t rows,
                                   uint32_t cols, uint32_t physical_cols)
{
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    const uint32_t src_stride = env->tile_reg_cols[src_tile];
    unsigned row_reg = 0;
    unsigned col_reg = 0;

    if (!linx_tile_resolve_ior(env, 0, &row_reg) ||
        !linx_tile_resolve_ior(env, 1, &col_reg)) {
        return false;
    }
    const uint32_t index_row = env->gpr[row_reg] & 0xffffu;
    const uint32_t index_col = env->gpr[col_reg] & 0xffffu;
    if ((env->tile_reg_dtype[src_tile] & 0x1fu) != dtype ||
        env->tile_reg_elem_bytes[src_tile] != elem_bytes ||
        index_row + rows > env->tile_reg_valid_rows[src_tile] ||
        index_col + cols > env->tile_reg_valid_cols[src_tile] ||
        src_stride < env->tile_reg_valid_cols[src_tile]) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t value = 0;
            if (!linx_tile_get_elem(env, src_tile,
                                    (index_row + r) * src_stride +
                                        index_col + c,
                                    elem_bytes, &value) ||
                !linx_tile_set_elem(env, dst_tile,
                                    r * physical_cols + c,
                                    elem_bytes, value)) {
                return false;
            }
        }
    }
    return true;
}

static bool linx_tile_tepl_dequant(CPULinxState *env, unsigned dst_tile,
                                   unsigned src_tile, unsigned scale_tile,
                                   unsigned offset_tile, uint32_t rows,
                                   uint32_t cols, uint32_t physical_cols)
{
    const uint32_t src_dtype = env->tile_attr_dtype & 0x1fu;
    const unsigned src_bytes = linx_tile_dtype_elem_bytes(src_dtype);
    const uint32_t src_stride = env->tile_reg_cols[src_tile];
    const uint32_t scale_stride = env->tile_reg_cols[scale_tile];
    const uint32_t offset_stride = env->tile_reg_cols[offset_tile];

    if ((env->tile_dtype & 0x1fu) != 1u ||
        (src_dtype != 19u && src_dtype != 18u) ||
        (env->tile_reg_dtype[src_tile] & 0x1fu) != src_dtype ||
        env->tile_reg_elem_bytes[src_tile] != src_bytes ||
        env->tile_reg_valid_rows[src_tile] != rows ||
        env->tile_reg_valid_cols[src_tile] != cols ||
        src_stride < cols ||
        (env->tile_reg_dtype[scale_tile] & 0x1fu) != 1u ||
        (env->tile_reg_dtype[offset_tile] & 0x1fu) != 1u ||
        env->tile_reg_elem_bytes[scale_tile] != sizeof(uint32_t) ||
        env->tile_reg_elem_bytes[offset_tile] != sizeof(uint32_t) ||
        env->tile_reg_valid_rows[scale_tile] < rows ||
        env->tile_reg_valid_rows[offset_tile] < rows ||
        env->tile_reg_valid_cols[scale_tile] < 1u ||
        env->tile_reg_valid_cols[offset_tile] < 1u) {
        return false;
    }

    for (uint32_t r = 0; r < rows; r++) {
        uint32_t scale_word = 0;
        uint32_t offset_word = 0;
        if (!linx_tile_get_elem(env, scale_tile, r * scale_stride,
                                sizeof(uint32_t), &scale_word) ||
            !linx_tile_get_elem(env, offset_tile, r * offset_stride,
                                sizeof(uint32_t), &offset_word)) {
            return false;
        }
        const float scale = linx_tile_word_as_f32(scale_word);
        const float offset = linx_tile_word_as_f32(offset_word);
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t src = 0;
            if (!linx_tile_get_elem(env, src_tile, r * src_stride + c,
                                    src_bytes, &src)) {
                return false;
            }
            const float value =
                ((float)linx_tile_sign_extend(src, src_bytes) - offset) *
                scale;
            if (!linx_tile_set_elem(env, dst_tile,
                                    r * physical_cols + c,
                                    sizeof(uint32_t),
                                    linx_tile_f32_as_word(value))) {
                return false;
            }
        }
    }
    return true;
}

static inline uint32_t linx_tile_tepl_unary_word(uint32_t op, uint32_t dtype,
                                                 uint32_t value)
{
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);

    if ((dtype & 0x1fu) == 1u) {
        const float input = linx_tile_word_as_f32(value);
        float out = 0.0f;

        switch (op) {
        case 0x00bu:
            out = input > 0.0f ? input : 0.0f;
            break;
        case 0x00du:
        case 0x01cu:
            out = input;
            break;
        case 0x00eu:
            out = expf(input);
            break;
        case 0x00fu:
            out = input > 0.0f ? logf(input) : -INFINITY;
            break;
        case 0x010u:
            out = input >= 0.0f ? sqrtf(input) : NAN;
            break;
        case 0x011u:
            out = input > 0.0f ? 1.0f / sqrtf(input) : 0.0f;
            break;
        case 0x018u:
            out = input == 0.0f ? 0.0f : 1.0f / input;
            break;
        case 0x02du:
            out = fabsf(input);
            break;
        case 0x02fu:
            return value ^ 0x80000000u;
        default:
            return 0;
        }
        return linx_tile_f32_as_word(out);
    }

    switch (op) {
    case 0x00bu:
        return linx_tile_dtype_is_signed(dtype) &&
                       linx_tile_sign_extend(value, elem_bytes) < 0
                   ? 0u
                   : value;
    case 0x00du:
    case 0x01cu:
        return value;
    case 0x02du:
        if (linx_tile_dtype_is_signed(dtype)) {
            const int64_t signed_value =
                linx_tile_sign_extend(value, elem_bytes);
            return (uint32_t)(signed_value < 0 ? -signed_value
                                               : signed_value);
        }
        return value;
    case 0x02eu:
        return ~value;
    case 0x02fu:
        if ((dtype & 0x1fu) == 2u || (dtype & 0x1fu) == 6u) {
            return value ^ 0x8000u;
        }
        return 0u - value;
    default:
        return 0;
    }
}

static bool linx_tile_tepl_shape(const CPULinxState *env, unsigned elem_bytes,
                                 uint32_t elems, uint32_t *rows_out,
                                 uint32_t *cols_out,
                                 uint32_t *physical_cols_out)
{
    uint32_t cols = (uint32_t)(env->lb[0] & 0xffffffffu);
    uint32_t rows = (uint32_t)(env->lb[1] & 0xffffffffu);
    uint32_t physical_cols = (uint32_t)(env->lb[2] & 0xffffffffu);

    if (cols == 0u) {
        cols = elems;
    }
    if (rows == 0u) {
        rows = 1u;
    }
    if (physical_cols == 0u) {
        physical_cols = cols;
    }
    if (elem_bytes == 0u || cols == 0u || physical_cols < cols ||
        rows > elems / physical_cols) {
        return false;
    }
    *rows_out = rows;
    *cols_out = cols;
    *physical_cols_out = physical_cols;
    return true;
}

static bool linx_tile_interleave_dtype_supported(uint32_t dtype)
{
    switch (dtype & 0x1fu) {
    case 1u:  /* FP32 */
    case 2u:  /* FP16 */
    case 6u:  /* BF16 */
    case 17u: /* INT32 */
    case 18u: /* INT16 */
    case 19u: /* INT8 */
    case 25u: /* UINT32 */
    case 26u: /* UINT16 */
    case 27u: /* UINT8 */
        return true;
    default:
        return false;
    }
}

static bool linx_tile_interleave(CPULinxState *env, uint32_t op,
                                 const unsigned outputs[2],
                                 const unsigned sources[2],
                                 unsigned size_code)
{
    const uint64_t bytes64 =
        size_code < 60u ? (1ull << (size_code + 4u)) : 0ull;
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t physical_cols = 0;

    if ((op != 0x08au && op != 0x08bu) ||
        !linx_tile_interleave_dtype_supported(dtype) || bytes64 == 0u ||
        bytes64 > LINX_TILE_MAX_BYTES || (bytes64 % elem_bytes) != 0u ||
        !linx_tile_tepl_shape(env, elem_bytes,
                              (uint32_t)(bytes64 / elem_bytes),
                              &rows, &cols, &physical_cols) ||
        (cols & 1u) != 0u) {
        return false;
    }

    for (unsigned i = 0; i < 2; i++) {
        const unsigned src = sources[i];
        const unsigned dst = outputs[i];
        if (src >= 32u || dst >= 32u ||
            (env->tile_reg_dtype[src] & 0x1fu) != dtype ||
            env->tile_reg_elem_bytes[src] != elem_bytes ||
            env->tile_reg_valid_rows[src] != rows ||
            env->tile_reg_valid_cols[src] != cols ||
            env->tile_reg_cols[src] != physical_cols ||
            env->tile_reg_valid_rows[dst] != rows ||
            env->tile_reg_valid_cols[dst] != cols ||
            env->tile_reg_cols[dst] != physical_cols) {
            return false;
        }
    }

    /* Canonical operand order is dst1, dst0, src1, src0. */
    const unsigned dst1 = outputs[0];
    const unsigned dst0 = outputs[1];
    const unsigned src1 = sources[0];
    const unsigned src0 = sources[1];
    const uint32_t half = cols / 2u;

    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t col = 0; col < cols; col++) {
            uint32_t value0 = 0;
            uint32_t value1 = 0;
            const uint32_t lane = row * physical_cols + col;
            uint32_t dst0_lane;
            uint32_t dst1_lane;

            if (op == 0x08bu) {
                const uint32_t dst = 2u * (col % half);
                dst0_lane = row * physical_cols + dst;
                dst1_lane = dst0_lane + 1u;
                if (!linx_tile_get_elem(env, src0, lane, elem_bytes,
                                        &value0) ||
                    !linx_tile_get_elem(env, src1, lane, elem_bytes,
                                        &value1)) {
                    return false;
                }
                const unsigned out = col < half ? dst0 : dst1;
                if (!linx_tile_set_elem(env, out, dst0_lane, elem_bytes,
                                        value0) ||
                    !linx_tile_set_elem(env, out, dst1_lane, elem_bytes,
                                        value1)) {
                    return false;
                }
            } else {
                const unsigned src = col < half ? src0 : src1;
                const uint32_t src_col = 2u * (col % half);
                if (!linx_tile_get_elem(
                        env, src, row * physical_cols + src_col,
                        elem_bytes, &value0) ||
                    !linx_tile_get_elem(
                        env, src, row * physical_cols + src_col + 1u,
                        elem_bytes, &value1) ||
                    !linx_tile_set_elem(env, dst0, lane, elem_bytes,
                                        value0) ||
                    !linx_tile_set_elem(env, dst1, lane, elem_bytes,
                                        value1)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool linx_tile_tepl_selector_executable(uint32_t op)
{
    switch (op) {
    case 0x000u: /* TADD */
    case 0x001u: /* TSUB */
    case 0x002u: /* TMUL */
    case 0x003u: /* TDIV */
    case 0x004u: /* TMAX */
    case 0x005u: /* TMIN */
    case 0x006u: /* TAND */
    case 0x007u: /* TOR */
    case 0x008u: /* TXOR */
    case 0x009u: /* TSHL */
    case 0x00au: /* TSHR */
    case 0x00bu: /* TRELU */
    case 0x00du: /* TCVT */
    case 0x00eu: /* TEXP */
    case 0x00fu: /* TLOG */
    case 0x010u: /* TSQRT */
    case 0x011u: /* TRSQRT */
    case 0x012u: /* TROWMAX */
    case 0x013u: /* TROWMIN */
    case 0x014u: /* TROWSUM */
    case 0x015u: /* TCOLMAX */
    case 0x016u: /* TCOLMIN */
    case 0x017u: /* TCOLSUM */
    case 0x018u: /* TRECIP */
    case 0x019u: /* TEXPANDS */
    case 0x01au: /* TGATHER */
    case 0x01bu: /* TSCATTER */
    case 0x01cu: /* TRESHAPE */
    case 0x01du: /* TTRANS */
    case 0x01eu: /* TCOLEXPAND */
    case 0x01fu: /* TROWEXPAND */
    case 0x020u: /* TADDS */
    case 0x021u: /* TSUBS */
    case 0x022u: /* TMULS */
    case 0x023u: /* TDIVS */
    case 0x024u: /* TMAXS */
    case 0x025u: /* TMINS */
    case 0x026u: /* TANDS */
    case 0x027u: /* TORS */
    case 0x028u: /* TXORS */
    case 0x029u: /* TSHLS */
    case 0x02au: /* TSHRS */
    case 0x02bu: /* TCMP */
    case 0x02cu: /* TSEL */
    case 0x02du: /* TABS */
    case 0x02eu: /* TNOT */
    case 0x02fu: /* TNEG */
    case 0x030u: /* TREM */
    case 0x032u: /* TREMS */
    case 0x033u: /* TCMPS */
    case 0x034u: /* TSELS */
    case 0x035u: /* TROWPROD */
    case 0x036u: /* TROWARGMAX */
    case 0x037u: /* TROWARGMIN */
    case 0x038u: /* TCOLPROD */
    case 0x039u: /* TCOLARGMAX */
    case 0x03au: /* TCOLARGMIN */
    case 0x03bu: /* TROWEXPANDADD */
    case 0x03cu: /* TROWEXPANDSUB */
    case 0x03du: /* TROWEXPANDMUL */
    case 0x03eu: /* TROWEXPANDDIV */
    case 0x03fu: /* TROWEXPANDMAX */
    case 0x040u: /* TROWEXPANDMIN */
    case 0x041u: /* TROWEXPANDEXPDIF */
    case 0x042u: /* TCOLEXPANDADD */
    case 0x043u: /* TCOLEXPANDSUB */
    case 0x044u: /* TCOLEXPANDMUL */
    case 0x045u: /* TCOLEXPANDDIV */
    case 0x046u: /* TCOLEXPANDMAX */
    case 0x047u: /* TCOLEXPANDMIN */
    case 0x048u: /* TCOLEXPANDEXPDIF */
    case 0x082u: /* TFILLPAD */
    case 0x084u: /* TDEQUANT */
    case 0x085u: /* TEXTRACT */
    case 0x080u: /* TCI */
    case 0x081u: /* TTRI */
    case 0x087u: /* TCONCAT */
    case 0x089u: /* TGATHERB */
    case 0x08au: /* TDEINTERLEAVE */
    case 0x08bu: /* TINTERLEAVE */
    case 0x0c3u: /* TPARTADD */
    case 0x0c4u: /* TPARTMUL */
    case 0x0c5u: /* TPARTMAX */
    case 0x0c6u: /* TPARTMIN */
        return true;
    default:
        return false;
    }
}

static int linx_tile_tepl_source_arity(uint32_t op)
{
    switch (op) {
    case 0x019u: /* TEXPANDS */
    case 0x080u: /* TCI */
    case 0x081u: /* TTRI */
        return 0;
    case 0x00bu: /* TRELU */
    case 0x00du: /* TCVT */
    case 0x00eu: /* TEXP */
    case 0x00fu: /* TLOG */
    case 0x010u: /* TSQRT */
    case 0x011u: /* TRSQRT */
    case 0x012u: /* TROWMAX */
    case 0x013u: /* TROWMIN */
    case 0x014u: /* TROWSUM */
    case 0x015u: /* TCOLMAX */
    case 0x016u: /* TCOLMIN */
    case 0x017u: /* TCOLSUM */
    case 0x018u: /* TRECIP */
    case 0x01du: /* TTRANS */
    case 0x01cu: /* TRESHAPE */
    case 0x01eu: /* TCOLEXPAND */
    case 0x01fu: /* TROWEXPAND */
    case 0x020u: /* TADDS */
    case 0x021u: /* TSUBS */
    case 0x022u: /* TMULS */
    case 0x023u: /* TDIVS */
    case 0x024u: /* TMAXS */
    case 0x025u: /* TMINS */
    case 0x026u: /* TANDS */
    case 0x027u: /* TORS */
    case 0x028u: /* TXORS */
    case 0x029u: /* TSHLS */
    case 0x02au: /* TSHRS */
    case 0x02du: /* TABS */
    case 0x02eu: /* TNOT */
    case 0x02fu: /* TNEG */
    case 0x032u: /* TREMS */
    case 0x033u: /* TCMPS */
    case 0x035u: /* TROWPROD */
    case 0x036u: /* TROWARGMAX */
    case 0x037u: /* TROWARGMIN */
    case 0x038u: /* TCOLPROD */
    case 0x039u: /* TCOLARGMAX */
    case 0x03au: /* TCOLARGMIN */
    case 0x082u: /* TFILLPAD */
    case 0x085u: /* TEXTRACT */
        return 1;
    case 0x02cu: /* TSEL */
        return 3;
    case 0x084u: /* TDEQUANT */
        return 3;
    case 0x034u: /* TSELS */
        return 2;
    default:
        return linx_tile_tepl_selector_executable(op) ? 2 : -1;
    }
}

static void linx_tile_tepl(CPULinxState *env, unsigned dst_tile,
                           const unsigned *sources, unsigned source_count,
                           unsigned size_code, uint32_t op)
{
    const uint64_t bytes64 =
        size_code < 60u ? (1ull << (size_code + 4u)) : 0ull;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(env->tile_dtype);
    unsigned mask_tile = 0;
    unsigned src0_tile = source_count > 0u ? sources[0] : 0u;
    unsigned src1_tile = source_count > 1u ? sources[1] : 0u;
    bool has_src0 = source_count > 0u;
    bool has_src1 = source_count > 1u;

    if (op == 0x02cu) {
        mask_tile = sources[0];
        src0_tile = sources[1];
        src1_tile = sources[2];
        has_src0 = true;
        has_src1 = true;
    } else if (op == 0x034u) {
        mask_tile = sources[0];
        src0_tile = sources[1];
        has_src0 = true;
        has_src1 = false;
    }
    const unsigned src0_elem_bytes =
        has_src0 ? env->tile_reg_elem_bytes[src0_tile] : 0u;
    const unsigned src1_elem_bytes =
        has_src1 ? env->tile_reg_elem_bytes[src1_tile] : 0u;
    const uint32_t src0_dtype =
        has_src0 ? env->tile_reg_dtype[src0_tile] : 0u;
    const uint32_t src1_dtype =
        has_src1 ? env->tile_reg_dtype[src1_tile] : 0u;
    unsigned scalar_reg = 0;
    const bool scalar_mode =
        (env->tile_arg_format == 1u || env->tile_arg_format == 2u) &&
        linx_tile_resolve_ior(env, 0, &scalar_reg);
    const uint32_t scalar_word =
        scalar_mode
            ? linx_tile_scalar_as_dtype(env->gpr[scalar_reg], env->tile_dtype,
                                        elem_bytes)
            : 0u;
    uint32_t rows = 0;
    uint32_t cols = 0;
    uint32_t physical_cols = 0;

    if (!linx_tile_tepl_selector_executable(op) ||
        source_count != (unsigned)linx_tile_tepl_source_arity(op) ||
        dst_tile >= 32u ||
        bytes64 == 0u ||
        bytes64 > LINX_TILE_MAX_BYTES ||
        (elem_bytes != 1u && elem_bytes != 2u && elem_bytes != 4u) ||
        (bytes64 % elem_bytes) != 0u ||
        !linx_tile_tepl_shape(env, elem_bytes,
                              (uint32_t)(bytes64 / elem_bytes),
                              &rows, &cols, &physical_cols)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint32_t active = rows * cols;
    const uint32_t dtype = env->tile_dtype & 0x1fu;
    if (op == 0x02fu && dtype != 17u && dtype != 18u && dtype != 1u &&
        dtype != 2u && dtype != 6u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((op == 0x030u || op == 0x032u) && dtype != 17u && dtype != 25u &&
        dtype != 18u && dtype != 26u && dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x033u && dtype != 17u && dtype != 1u && dtype != 2u &&
        dtype != 26u && dtype != 18u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((op == 0x02cu || op == 0x034u) &&
        dtype != 17u && dtype != 18u && dtype != 19u &&
        dtype != 25u && dtype != 26u && dtype != 27u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x035u && dtype != 2u && dtype != 1u &&
        dtype != 17u && dtype != 18u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x038u && dtype != 2u && dtype != 1u && dtype != 6u &&
        dtype != 18u && dtype != 26u && dtype != 17u && dtype != 25u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((op == 0x036u || op == 0x037u) && dtype != 2u && dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((op == 0x039u || op == 0x03au) &&
        dtype != 19u && dtype != 27u && dtype != 18u && dtype != 26u &&
        dtype != 17u && dtype != 25u && dtype != 2u && dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((op >= 0x03bu && op <= 0x048u) && dtype != 1u && dtype != 2u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x082u && dtype != 19u && dtype != 27u &&
        dtype != 18u && dtype != 26u && dtype != 17u && dtype != 25u &&
        dtype != 2u && dtype != 6u && dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x080u && dtype != 17u && dtype != 25u &&
        dtype != 18u && dtype != 26u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x081u && dtype != 19u && dtype != 27u &&
        dtype != 18u && dtype != 26u && dtype != 17u && dtype != 25u &&
        dtype != 2u && dtype != 6u && dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((op >= 0x0c3u && op <= 0x0c6u) &&
        dtype != 19u && dtype != 27u && dtype != 18u && dtype != 26u &&
        dtype != 17u && dtype != 25u && dtype != 2u && dtype != 6u &&
        dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((op == 0x087u || op == 0x089u) &&
        dtype != 19u && dtype != 27u && dtype != 18u && dtype != 26u &&
        dtype != 17u && dtype != 25u && dtype != 2u && dtype != 6u &&
        dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x085u && dtype != 19u && dtype != 2u &&
        dtype != 6u && dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (op == 0x084u && dtype != 1u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint32_t physical_rows = (uint32_t)(bytes64 / elem_bytes) /
                                   physical_cols;
    const bool expand_op = op == 0x01eu || op == 0x01fu ||
                           (op >= 0x03bu && op <= 0x048u);
    const bool partial_op = op >= 0x0c3u && op <= 0x0c6u;
    const bool custom_shape_op = op == 0x01cu || op == 0x087u ||
                                 op == 0x089u || op == 0x085u ||
                                 op == 0x084u;
    if ((!expand_op && !partial_op && !custom_shape_op &&
         ((has_src0 &&
           !linx_tile_shape_covers(env, src0_tile, cols, rows, physical_cols,
                                   physical_rows)) ||
          (has_src1 &&
           !linx_tile_shape_covers(env, src1_tile, cols, rows, physical_cols,
                                   physical_rows)))) ||
        (expand_op && op >= 0x03bu &&
         !linx_tile_shape_covers(env, src0_tile, cols, rows, physical_cols,
                                 physical_rows))) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    memset(env->tile_reg[dst_tile], 0, LINX_TILE_MAX_BYTES);

    if (op == 0x02bu) {
        if (!has_src0 || !has_src1 ||
            !linx_tile_tepl_tcmp(env, dst_tile, src0_tile, src1_tile,
                                 rows, cols, physical_cols,
                                 (uint32_t)bytes64)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        return;
    } else if (op == 0x080u) {
        unsigned start_reg = 0;
        unsigned descending_reg = 0;
        if (!linx_tile_resolve_ior(env, 0, &start_reg) ||
            !linx_tile_resolve_ior(env, 1, &descending_reg) ||
            env->gpr[descending_reg] > 1u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        const uint32_t start = linx_tile_scalar_as_dtype(
            env->gpr[start_reg], dtype, elem_bytes);
        const bool descending = env->gpr[descending_reg] != 0u;
        for (uint32_t i = 0; i < cols; i++) {
            const uint32_t value = descending ? start - i : start + i;
            if (!linx_tile_set_elem(env, dst_tile, i, elem_bytes, value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
        }
    } else if (op == 0x081u) {
        unsigned diagonal_reg = 0;
        unsigned orientation_reg = 0;
        if (!linx_tile_resolve_ior(env, 0, &diagonal_reg) ||
            !linx_tile_resolve_ior(env, 1, &orientation_reg) ||
            env->gpr[orientation_reg] > 1u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        const int64_t diagonal = (int32_t)env->gpr[diagonal_reg];
        const bool upper = env->gpr[orientation_reg] != 0u;
        const uint32_t one = linx_tile_tepl_one(dtype);
        for (uint32_t r = 0; r < rows; r++) {
            for (uint32_t c = 0; c < cols; c++) {
                const int64_t boundary = (int64_t)r + diagonal;
                const bool selected = upper ? (int64_t)c >= boundary
                                            : (int64_t)c <= boundary;
                if (!linx_tile_set_elem(env, dst_tile,
                                        r * physical_cols + c,
                                        elem_bytes, selected ? one : 0u)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    return;
                }
            }
        }
    } else if (op == 0x02cu || op == 0x034u) {
        if ((op == 0x034u &&
             (!scalar_mode || env->tile_arg_format != 1u)) ||
            !linx_tile_tepl_select(env, dst_tile, mask_tile, src0_tile,
                                   src1_tile, op == 0x034u, scalar_word,
                                   rows, cols, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (op == 0x033u) {
        if (!has_src0 || has_src1 || !scalar_mode ||
            env->tile_arg_format != 1u ||
            !linx_tile_tepl_tcmps(env, dst_tile, src0_tile, scalar_word,
                                  rows, cols, physical_cols,
                                  (uint32_t)bytes64)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        return;
    } else if (op == 0x035u || op == 0x038u) {
        if (!linx_tile_tepl_product(env, dst_tile, src0_tile,
                                    op == 0x035u, rows, cols, physical_cols,
                                    (uint32_t)bytes64)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        }
        return;
    } else if (op >= 0x036u && op <= 0x03au) {
        const bool row_reduce = op == 0x036u || op == 0x037u;
        const bool find_max = op == 0x036u || op == 0x039u;

        if (!linx_tile_tepl_arg_reduce(env, dst_tile, src0_tile,
                                       row_reduce, find_max, rows, cols,
                                       physical_cols, (uint32_t)bytes64)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        }
        return;
    } else if (op == 0x082u) {
        if (!linx_tile_tepl_fillpad(env, dst_tile, src0_tile, rows, cols,
                                    physical_rows, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (partial_op) {
        static const unsigned binary_ops[4] = {
            0x000u, 0x002u, 0x004u, 0x005u,
        };
        if (!linx_tile_tepl_partial_binary(
                env, dst_tile, src0_tile, src1_tile,
                binary_ops[op - 0x0c3u], rows, cols, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (op == 0x01cu) {
        if (env->tile_reg_bytes[src0_tile] != bytes64) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        memcpy(env->tile_reg[dst_tile], env->tile_reg[src0_tile], bytes64);
    } else if (op == 0x087u) {
        if (!linx_tile_tepl_concat(env, dst_tile, src0_tile, src1_tile,
                                   rows, cols, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (op == 0x089u) {
        if (!linx_tile_tepl_gatherb(env, dst_tile, src0_tile, src1_tile,
                                    rows, cols, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (op == 0x085u) {
        if (!linx_tile_tepl_extract(env, dst_tile, src0_tile,
                                    rows, cols, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (op == 0x084u) {
        if (!linx_tile_tepl_dequant(env, dst_tile, sources[0], sources[1],
                                    sources[2], rows, cols, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (op == 0x00du) {
        if (!has_src0 || src0_elem_bytes == 0u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        const uint32_t src_elems =
            env->tile_reg_bytes[src0_tile] / src0_elem_bytes;
        const uint32_t count = MIN(active, src_elems);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t lane = (i / cols) * physical_cols + (i % cols);
            uint32_t value = 0;
            linx_tile_get_elem(env, src0_tile, lane, src0_elem_bytes, &value);
            const float converted =
                linx_tile_value_as_f32(value, src0_dtype, src0_elem_bytes);
            linx_tile_set_elem(
                env, dst_tile, lane, elem_bytes,
                linx_tile_f32_as_dtype(converted, env->tile_dtype,
                                       elem_bytes));
        }
    } else if (op == 0x019u && scalar_mode) {
        for (uint32_t r = 0; r < rows; r++)
            for (uint32_t c = 0; c < cols; c++)
                linx_tile_set_elem(env, dst_tile, r * physical_cols + c,
                                   elem_bytes, scalar_word);
    } else if (op == 0x01au) {
        if (!has_src0 || !has_src1 || src0_elem_bytes == 0u ||
            src1_elem_bytes == 0u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        const uint32_t index_elems =
            env->tile_reg_bytes[src1_tile] / src1_elem_bytes;
        const uint32_t count = MIN(active, index_elems);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t lane = (i / cols) * physical_cols + (i % cols);
            uint32_t index = 0;
            uint32_t value = 0;
            linx_tile_get_elem(env, src1_tile, lane, src1_elem_bytes, &index);
            if (linx_tile_dtype_is_signed(src1_dtype) &&
                linx_tile_sign_extend(index, src1_elem_bytes) < 0) {
                continue;
            }
            index %= MAX(1u, active);
            const uint32_t src_lane =
                (index / cols) * physical_cols + (index % cols);
            linx_tile_get_elem(env, src0_tile, src_lane, src0_elem_bytes, &value);
            linx_tile_set_elem(env, dst_tile, lane, elem_bytes, value);
        }
    } else if (op == 0x01bu) {
        if (!has_src0 || !has_src1 || src0_elem_bytes == 0u ||
            src1_elem_bytes == 0u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        const uint32_t data_elems =
            env->tile_reg_bytes[src0_tile] / src0_elem_bytes;
        const uint32_t index_elems =
            env->tile_reg_bytes[src1_tile] / src1_elem_bytes;
        const uint32_t count = MIN(active, MIN(data_elems, index_elems));
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t lane = (i / cols) * physical_cols + (i % cols);
            uint32_t index = 0;
            uint32_t value = 0;
            linx_tile_get_elem(env, src1_tile, lane, src1_elem_bytes, &index);
            if (linx_tile_dtype_is_signed(src1_dtype) &&
                linx_tile_sign_extend(index, src1_elem_bytes) < 0) {
                continue;
            }
            index %= MAX(1u, active);
            const uint32_t dst_lane =
                (index / cols) * physical_cols + (index % cols);
            linx_tile_get_elem(env, src0_tile, lane, src0_elem_bytes, &value);
            linx_tile_set_elem(env, dst_tile, dst_lane, elem_bytes, value);
        }
    } else if (op == 0x012u || op == 0x013u || op == 0x014u) {
        if (!has_src0) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (uint32_t r = 0; r < rows; r++) {
            uint32_t result = 0;
            if (!linx_tile_get_elem(env, src0_tile, r * physical_cols, elem_bytes,
                                    &result)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (op == 0x014u) {
                result = (env->tile_dtype & 0x1fu) == 1u
                             ? linx_tile_f32_as_word(0.0f)
                             : 0u;
            }
            for (uint32_t c = 0; c < cols; c++) {
                uint32_t value = 0;
                if (!linx_tile_get_elem(env, src0_tile, r * physical_cols + c,
                                        elem_bytes, &value)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    return;
                }
                if (op == 0x014u) {
                    result = linx_tile_tepl_binary_word(
                        env, 0x000u, env->tile_dtype, result, value);
                } else {
                    result = linx_tile_tepl_binary_word(
                        env, op == 0x012u ? 0x004u : 0x005u,
                        env->tile_dtype, result, value);
                }
            }
            linx_tile_set_elem(env, dst_tile, r * physical_cols, elem_bytes, result);
        }
    } else if (op == 0x015u || op == 0x016u || op == 0x017u) {
        if (!has_src0) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (uint32_t c = 0; c < cols; c++) {
            uint32_t result = 0;
            if (!linx_tile_get_elem(env, src0_tile, c, elem_bytes, &result)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (op == 0x017u) {
                result = (env->tile_dtype & 0x1fu) == 1u
                             ? linx_tile_f32_as_word(0.0f)
                             : 0u;
            }
            for (uint32_t r = 0; r < rows; r++) {
                uint32_t value = 0;
                linx_tile_get_elem(env, src0_tile, r * physical_cols + c, elem_bytes,
                                   &value);
                if (op == 0x017u) {
                    result = linx_tile_tepl_binary_word(
                        env, 0x000u, env->tile_dtype, result, value);
                } else {
                    result = linx_tile_tepl_binary_word(
                        env, op == 0x015u ? 0x004u : 0x005u,
                        env->tile_dtype, result, value);
                }
            }
            linx_tile_set_elem(env, dst_tile, c, elem_bytes, result);
        }
    } else if (op == 0x01du) {
        if (!has_src0) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (uint32_t r = 0; r < rows; r++) {
            for (uint32_t c = 0; c < cols; c++) {
                uint32_t value = 0;
                linx_tile_get_elem(env, src0_tile, r * physical_cols + c, elem_bytes,
                                   &value);
                linx_tile_set_elem(env, dst_tile, c * physical_cols + r, elem_bytes,
                                   value);
            }
        }
    } else if (op == 0x01eu || op == 0x01fu ||
               (op >= 0x03bu && op <= 0x048u)) {
        const bool pure_expand = op == 0x01eu || op == 0x01fu;
        const bool row_expand = op == 0x01fu ||
                                (op >= 0x03bu && op <= 0x041u);
        const unsigned operation = pure_expand
                                       ? 0u
                                       : (row_expand ? op - 0x03bu
                                                     : op - 0x042u);
        if (!linx_tile_tepl_expand(env, dst_tile, src0_tile, src1_tile,
                                   pure_expand, row_expand, operation,
                                   rows, cols, physical_cols)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else {
        for (uint32_t i = 0; i < active; i++) {
            const uint32_t lane = (i / cols) * physical_cols + (i % cols);
            uint32_t lhs = 0;
            uint32_t rhs = 0;
            uint32_t result = 0;
            if (has_src0 &&
                !linx_tile_get_elem(env, src0_tile, lane, elem_bytes, &lhs)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (has_src1 &&
                !linx_tile_get_elem(env, src1_tile, lane, elem_bytes, &rhs)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (has_src0 && has_src1) {
                result = linx_tile_tepl_binary_word(
                    env, op, env->tile_dtype, lhs, rhs);
            } else if (has_src0 && scalar_mode &&
                       env->tile_arg_format == 1u) {
                result = linx_tile_tepl_binary_word(
                    env, op, env->tile_dtype, lhs, scalar_word);
            } else if (has_src0) {
                result = linx_tile_tepl_unary_word(
                    op, env->tile_dtype, lhs);
            } else {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            linx_tile_set_elem(env, dst_tile, lane, elem_bytes, result);
        }
    }

    env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
    linx_tile_set_elem_bytes(env, dst_tile, elem_bytes);
    linx_tile_set_dtype(env, dst_tile, env->tile_dtype);
    if (!linx_tile_set_block_shape(env, dst_tile, (uint32_t)bytes64,
                                   elem_bytes)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
    }
}

static inline unsigned linx_tile_offset_elem_bytes(const CPULinxState *env,
                                                   unsigned tile,
                                                   uint32_t lane_count)
{
    const uint32_t bytes = env->tile_reg_bytes[tile];
    const unsigned elem_bytes = env->tile_reg_elem_bytes[tile];

    if (lane_count == 0 || bytes < lane_count * 2u) {
        return 0;
    }
    if ((elem_bytes == 2u || elem_bytes == 4u || elem_bytes == 8u) &&
        bytes >= lane_count * elem_bytes) {
        return elem_bytes;
    }
    if (bytes >= lane_count * 8u) {
        return 8u;
    }
    if (bytes >= lane_count * 4u) {
        return 4u;
    }
    return 2u;
}

static inline uint32_t linx_tile_mask_bit(const CPULinxState *env,
                                          unsigned tile, uint32_t lane)
{
    const uint8_t *buf = (const uint8_t *)env->tile_reg[tile];
    const size_t byte = lane >> 3;

    if (byte >= env->tile_reg_bytes[tile] || byte >= LINX_TILE_MAX_BYTES) {
        return 0;
    }
    return (buf[byte] >> (lane & 7u)) & 1u;
}

static bool linx_tile_resolve_transfer_shape(const CPULinxState *env,
                                             uint32_t tile_elems,
                                             uint32_t *tr_outer_out,
                                             uint32_t *tr_inner_out,
                                             uint32_t *gm_outer_out,
                                             uint32_t *gm_inner_out)
{
    uint32_t gm_inner = (uint32_t)(env->lb[0] & 0xffffffffu);
    uint32_t gm_outer = (uint32_t)(env->lb[1] & 0xffffffffu);
    uint32_t tr_inner = (uint32_t)(env->lb[2] & 0xffffffffu);
    uint32_t tr_outer = 0;

    /*
     * The size code allocates the carrier tile footprint, but LB0/LB1 still
     * define the logical 2D transfer rectangle when LB2 is absent.
     */
    if (tr_inner != 0u) {
        if ((tile_elems % tr_inner) != 0u) {
            return false;
        }
        tr_outer = tile_elems / tr_inner;
    } else if (gm_inner != 0u && gm_outer != 0u &&
               (uint64_t)gm_inner * (uint64_t)gm_outer <= (uint64_t)tile_elems) {
        tr_inner = gm_inner;
        tr_outer = gm_outer;
    } else {
        tr_inner = tile_elems;
        tr_outer = 1u;
    }

    if (tr_inner == 0u || tr_outer == 0u) {
        return false;
    }
    if (gm_inner == 0u) {
        gm_inner = tr_inner;
    }
    if (gm_outer == 0u) {
        gm_outer = tr_outer;
    }
    if ((uint64_t)gm_inner * (uint64_t)gm_outer >
        (uint64_t)tr_inner * (uint64_t)tr_outer) {
        return false;
    }

    *tr_outer_out = tr_outer;
    *tr_inner_out = tr_inner;
    *gm_outer_out = gm_outer;
    *gm_inner_out = gm_inner;
    return true;
}

static void linx_tile_load(CPULinxState *env, unsigned dst_tile, unsigned addr_reg,
                           unsigned size_code, uint64_t stride_bytes)
{
    if (dst_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const size_t bytes = (size_t)bytes64;
    const uint32_t dtype = env->tile_dtype;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    if ((bytes % elem_bytes) != 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint32_t tile_elems = (uint32_t)(bytes / elem_bytes);
    const LinxTileFormatDesc fmt =
        linx_tile_decode_tma_format(env->tile_arg_format, LINX_TMA_GM_TO_TR);
    if (!fmt.valid) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    uint32_t tr_outer = 0;
    uint32_t tr_inner = 0;
    uint32_t gm_outer = 0;
    uint32_t gm_inner = 0;
    if (!linx_tile_resolve_transfer_shape(env, tile_elems, &tr_outer, &tr_inner,
                                          &gm_outer, &gm_inner)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (!linx_tile_layout_shape_valid(fmt.dst, tr_outer, tr_inner,
                                      elem_bytes)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t base = env->gpr[addr_reg];
    memset(env->tile_reg[dst_tile], 0, LINX_TILE_MAX_BYTES);
    for (uint32_t to = 0; to < tr_outer; to++) {
        for (uint32_t ti = 0; ti < tr_inner; ti++) {
            uint32_t dst_idx = 0;
            uint32_t value = 0;
            if (!linx_tile_linear_index(fmt.dst, tr_outer, tr_inner, elem_bytes, to, ti, &dst_idx)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (to < gm_outer && ti < gm_inner) {
                uint32_t src_idx = 0;
                if (!linx_tile_linear_index(fmt.src, gm_outer, gm_inner, elem_bytes, to, ti, &src_idx)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    return;
                }
                uint64_t src_off = (uint64_t)src_idx * elem_bytes;
                if (stride_bytes != 0 && fmt.src == LINX_TILE_LAYOUT_ND) {
                    src_off = (uint64_t)to * stride_bytes +
                              (uint64_t)ti * elem_bytes;
                } else if (stride_bytes != 0 && fmt.src == LINX_TILE_LAYOUT_DN) {
                    src_off = (uint64_t)ti * stride_bytes +
                              (uint64_t)to * elem_bytes;
                }
                value = linx_tile_mem_read(env, base + src_off, elem_bytes);
            } else {
                const uint32_t seed = (to * tr_inner) ^ ti ^ (uint32_t)base ^ (env->tile_arg_format << 8);
                value = linx_tile_pad_value(linx_tile_arg_pad(env->tile_arg_format),
                                            dtype, elem_bytes, seed);
            }
            if (!linx_tile_set_elem(env, dst_tile, dst_idx, elem_bytes, value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
        }
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)bytes;
    linx_tile_set_elem_bytes(env, dst_tile, elem_bytes);
    linx_tile_set_dtype(env, dst_tile, env->tile_dtype);
    if (!linx_tile_set_shape(env, dst_tile, gm_inner, gm_outer,
                             tr_inner, tr_outer)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
    }
}

static void linx_tile_store(CPULinxState *env, unsigned src_tile, unsigned addr_reg,
                            unsigned size_code, uint64_t stride_bytes)
{
    if (src_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const size_t bytes = (size_t)bytes64;
    const uint32_t dtype = env->tile_dtype;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(dtype);
    if ((bytes % elem_bytes) != 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (env->tile_reg_bytes[src_tile] < bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint32_t tile_elems = (uint32_t)(bytes / elem_bytes);
    const LinxTileFormatDesc fmt =
        linx_tile_decode_tma_format(env->tile_arg_format, LINX_TMA_TR_TO_GM);
    if (!fmt.valid) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    uint32_t tr_outer = 0;
    uint32_t tr_inner = 0;
    uint32_t gm_outer = 0;
    uint32_t gm_inner = 0;
    if (!linx_tile_resolve_transfer_shape(env, tile_elems, &tr_outer, &tr_inner,
                                          &gm_outer, &gm_inner)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (!linx_tile_layout_shape_valid(fmt.src, tr_outer, tr_inner,
                                      elem_bytes)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (env->tile_reg_dtype[src_tile] != (dtype & 0x1fu) ||
        env->tile_reg_elem_bytes[src_tile] != elem_bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t base = env->gpr[addr_reg];
    for (uint32_t go = 0; go < gm_outer; go++) {
        for (uint32_t gi = 0; gi < gm_inner; gi++) {
            uint32_t src_idx = 0;
            uint32_t dst_idx = 0;
            uint32_t value = 0;
            if (!linx_tile_linear_index(fmt.src, tr_outer, tr_inner, elem_bytes, go, gi, &src_idx) ||
                !linx_tile_linear_index(fmt.dst, gm_outer, gm_inner, elem_bytes, go, gi, &dst_idx)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (!linx_tile_get_elem(env, src_tile, src_idx, elem_bytes, &value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            uint64_t dst_off = (uint64_t)dst_idx * elem_bytes;
            if (stride_bytes != 0 && fmt.dst == LINX_TILE_LAYOUT_ND) {
                dst_off = (uint64_t)go * stride_bytes +
                          (uint64_t)gi * elem_bytes;
            } else if (stride_bytes != 0 && fmt.dst == LINX_TILE_LAYOUT_DN) {
                dst_off = (uint64_t)gi * stride_bytes +
                          (uint64_t)go * elem_bytes;
            }
            linx_tile_mem_write(env, base + dst_off, elem_bytes, value);
        }
    }
}

static bool linx_tile_sparse_shape(const CPULinxState *env, unsigned dst_tile,
                                   unsigned size_code, uint32_t *row_out,
                                   uint32_t *col_out, uint32_t *valid_row_out,
                                   uint32_t *valid_col_out,
                                   uint32_t *lane_count_out,
                                   unsigned *elem_bytes_out)
{
    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(env->tile_dtype);
    uint32_t col = (uint32_t)(env->lb[2] & 0xffffffffu);
    uint32_t valid_col = (uint32_t)(env->lb[0] & 0xffffffffu);
    uint32_t valid_row = (uint32_t)(env->lb[1] & 0xffffffffu);

    if (dst_tile >= 32 || bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES ||
        elem_bytes == 0 || (bytes64 % elem_bytes) != 0) {
        return false;
    }
    const uint32_t lane_count = (uint32_t)(bytes64 / elem_bytes);
    if (valid_col == 0u) {
        return false;
    }
    if (valid_row == 0u) {
        valid_row = 1u;
    }
    if (col == 0u) {
        col = valid_col;
    }
    if (col == 0u || (lane_count % col) != 0u) {
        return false;
    }
    const uint32_t row = lane_count / col;
    if (valid_col > col || valid_row > row) {
        return false;
    }
    *row_out = row;
    *col_out = col;
    *valid_row_out = valid_row;
    *valid_col_out = valid_col;
    *lane_count_out = lane_count;
    *elem_bytes_out = elem_bytes;
    return true;
}

static bool linx_tile_collect_sources(const CPULinxState *env,
                                      unsigned out[LINX_TILE_MAX_IOT * 2],
                                      unsigned *count_out)
{
    unsigned count = 0;

    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        for (unsigned source = 0; source < 2; source++) {
            if ((env->tile_iot_src_valid[i] & (1u << source)) == 0) {
                continue;
            }
            if (count >= LINX_TILE_MAX_IOT * 2) {
                return false;
            }
            out[count++] = env->tile_iot_src_phys[i][source];
        }
    }
    *count_out = count;
    return true;
}

static void linx_tile_prefetch(CPULinxState *env, unsigned addr_reg,
                               unsigned size_code)
{
    uint32_t word_sink = 0;

    if (addr_reg >= LINX_GPR_COUNT || size_code >= 60u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t bytes64 = 1ull << (size_code + 4u);
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(env->tile_dtype);
    if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES ||
        elem_bytes == 0 || (bytes64 % elem_bytes) != 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint32_t tile_elems = (uint32_t)(bytes64 / elem_bytes);
    uint32_t tr_outer = 0;
    uint32_t tr_inner = 0;
    uint32_t gm_outer = 0;
    uint32_t gm_inner = 0;
    const LinxTileFormatDesc fmt =
        linx_tile_decode_tma_format(env->tile_arg_format, LINX_TMA_GM_TO_TR);
    if (!fmt.valid ||
        !linx_tile_resolve_transfer_shape(env, tile_elems, &tr_outer, &tr_inner,
                                          &gm_outer, &gm_inner)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = env->gpr[addr_reg];
    for (uint32_t to = 0; to < tr_outer; to++) {
        for (uint32_t ti = 0; ti < tr_inner; ti++) {
            if (to >= gm_outer || ti >= gm_inner) {
                continue;
            }
            uint32_t src_idx = 0;
            if (!linx_tile_linear_index(fmt.src, gm_outer, gm_inner,
                                        elem_bytes, to, ti, &src_idx)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            word_sink ^= linx_tile_mem_read(env,
                                            base + (uint64_t)src_idx * elem_bytes,
                                            elem_bytes);
        }
    }
    (void)word_sink;
}

static void linx_tile_mgather_common(CPULinxState *env, unsigned dst_tile,
                                     unsigned offset_tile, unsigned mask_tile,
                                     bool use_mask, unsigned addr_reg,
                                     unsigned size_code)
{
    uint32_t row = 0, col = 0, valid_row = 0, valid_col = 0, lane_count = 0;
    unsigned elem_bytes = 0;
    if (!linx_tile_sparse_shape(env, dst_tile, size_code, &row, &col,
                                &valid_row, &valid_col, &lane_count,
                                &elem_bytes) ||
        addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const unsigned off_bytes =
        linx_tile_offset_elem_bytes(env, offset_tile, lane_count);
    if (off_bytes == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    memset(env->tile_reg[dst_tile], 0, LINX_TILE_MAX_BYTES);
    const uint64_t base = env->gpr[addr_reg];
    for (uint32_t r = 0; r < row; r++) {
        for (uint32_t c = 0; c < col; c++) {
            const uint32_t lane = r * col + c;
            uint32_t value;
            if (r < valid_row && c < valid_col &&
                (!use_mask || linx_tile_mask_bit(env, mask_tile, lane))) {
                uint64_t off = 0;
                if (!linx_tile_get_elem64(env, offset_tile, lane, off_bytes, &off)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    return;
                }
                value = linx_tile_mem_read(env, base + off, elem_bytes);
            } else {
                const uint32_t seed = lane ^ (uint32_t)base ^ (env->tile_attr_raw << 8);
                value = linx_tile_pad_value(env->tile_attr_pad, env->tile_dtype,
                                            elem_bytes, seed);
            }
            if (!linx_tile_set_elem(env, dst_tile, lane, elem_bytes, value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
        }
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)(lane_count * elem_bytes);
    linx_tile_set_elem_bytes(env, dst_tile, elem_bytes);
    linx_tile_set_dtype(env, dst_tile, env->tile_dtype);
    if (!linx_tile_set_shape(env, dst_tile, valid_col, valid_row, col, row)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
    }
}

static void linx_tile_mscatter_common(CPULinxState *env, unsigned data_tile,
                                      unsigned offset_tile, unsigned mask_tile,
                                      bool use_mask, unsigned addr_reg,
                                      unsigned size_code)
{
    uint32_t row = 0, col = 0, valid_row = 0, valid_col = 0, lane_count = 0;
    unsigned elem_bytes = 0;
    if (!linx_tile_sparse_shape(env, data_tile, size_code, &row, &col,
                                &valid_row, &valid_col, &lane_count,
                                &elem_bytes) ||
        addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const unsigned off_bytes =
        linx_tile_offset_elem_bytes(env, offset_tile, lane_count);
    if (off_bytes == 0 || env->tile_reg_bytes[data_tile] < lane_count * elem_bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = env->gpr[addr_reg];
    for (uint32_t r = 0; r < row; r++) {
        for (uint32_t c = 0; c < col; c++) {
            const uint32_t lane = r * col + c;
            uint32_t value = 0;
            uint64_t off = 0;
            if (r >= valid_row || c >= valid_col ||
                (use_mask && !linx_tile_mask_bit(env, mask_tile, lane))) {
                continue;
            }
            if (!linx_tile_get_elem64(env, offset_tile, lane, off_bytes, &off) ||
                !linx_tile_get_elem(env, data_tile, lane, elem_bytes, &value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            linx_tile_mem_write(env, base + off, elem_bytes, value);
        }
    }
}

static void linx_tile_mgather_cas(CPULinxState *env, unsigned dst_tile,
                                  unsigned offset_tile, unsigned expected_tile,
                                  unsigned desired_tile, unsigned addr_reg,
                                  unsigned size_code)
{
    uint32_t row = 0, col = 0, valid_row = 0, valid_col = 0, lane_count = 0;
    unsigned elem_bytes = 0;
    if (!linx_tile_sparse_shape(env, dst_tile, size_code, &row, &col,
                                &valid_row, &valid_col, &lane_count,
                                &elem_bytes) ||
        addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const unsigned off_bytes =
        linx_tile_offset_elem_bytes(env, offset_tile, lane_count);
    if (off_bytes == 0 ||
        env->tile_reg_bytes[expected_tile] < lane_count * elem_bytes ||
        env->tile_reg_bytes[desired_tile] < lane_count * elem_bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    memset(env->tile_reg[dst_tile], 0, LINX_TILE_MAX_BYTES);
    const uint64_t base = env->gpr[addr_reg];
    for (uint32_t r = 0; r < row; r++) {
        for (uint32_t c = 0; c < col; c++) {
            const uint32_t lane = r * col + c;
            uint32_t old_value;
            if (r < valid_row && c < valid_col) {
                uint64_t off = 0;
                uint32_t expected = 0;
                uint32_t desired = 0;
                if (!linx_tile_get_elem64(env, offset_tile, lane, off_bytes, &off) ||
                    !linx_tile_get_elem(env, expected_tile, lane, elem_bytes, &expected) ||
                    !linx_tile_get_elem(env, desired_tile, lane, elem_bytes, &desired)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    return;
                }
                old_value = linx_tile_mem_cmpxchg(env, base + off, elem_bytes,
                                                  expected, desired);
            } else {
                const uint32_t seed = lane ^ (uint32_t)base ^ (env->tile_attr_raw << 8);
                old_value = linx_tile_pad_value(env->tile_attr_pad, env->tile_dtype,
                                                elem_bytes, seed);
            }
            if (!linx_tile_set_elem(env, dst_tile, lane, elem_bytes, old_value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
        }
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)(lane_count * elem_bytes);
    linx_tile_set_elem_bytes(env, dst_tile, elem_bytes);
    linx_tile_set_dtype(env, dst_tile, env->tile_dtype);
    if (!linx_tile_set_shape(env, dst_tile, valid_col, valid_row, col, row)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
    }
}

static void linx_tile_mamulb(CPULinxState *env, unsigned src_a, unsigned src_b,
                             unsigned size_code)
{
    if (src_a >= 32 || src_b >= 32) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned m = env->lb[0] ? MIN((unsigned)env->lb[0], 8u) : 8u;
    const unsigned n = env->lb[1] ? MIN((unsigned)env->lb[1], 8u) : 8u;
    const unsigned kdim = env->lb[2] ? MIN((unsigned)env->lb[2], 8u) : 8u;

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES || (bytes64 & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    env->tile_acc_bytes = (uint32_t)bytes64;
    const unsigned words = (unsigned)(bytes64 / 4u);

    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            int64_t acc = 0;
            for (unsigned k = 0; k < kdim; k++) {
                const int32_t a = (int32_t)env->tile_reg[src_a][i * 8u + k];
                const int32_t b = (int32_t)env->tile_reg[src_b][k * 8u + j];
                acc += (int64_t)a * (int64_t)b;
            }
            env->tile_acc[i * 8u + j] = (uint32_t)(int32_t)acc;
        }
    }

    /* Zero the rest of the accumulator for determinism. */
    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 8; j++) {
            if (i < m && j < n) {
                continue;
            }
            env->tile_acc[i * 8u + j] = 0;
        }
    }
    for (unsigned i = 64; i < words && i < LINX_TILE_MAX_WORDS; i++) {
        env->tile_acc[i] = 0;
    }
}

static bool linx_tile_mamulb_bias(CPULinxState *env, unsigned src_a,
                                  unsigned src_b, unsigned bias,
                                  unsigned size_code)
{
    const unsigned m = env->lb[0] ? MIN((unsigned)env->lb[0], 8u) : 8u;
    const unsigned n = env->lb[1] ? MIN((unsigned)env->lb[1], 8u) : 8u;
    const unsigned kdim = env->lb[2] ? MIN((unsigned)env->lb[2], 8u) : 8u;

    /*
     * The current CUBE execution model is an INT32, contiguous 8x8
     * compatibility profile.  Keep the bias variant on that same closed
     * profile until the packed S8 and floating-point CUBE layouts are
     * architecturally available to QEMU.
     */
    if ((env->tile_dtype & 0x1fu) != 17u || src_a >= 32u || src_b >= 32u ||
        bias >= 32u || env->tile_reg_dtype[src_a] != 17u ||
        env->tile_reg_dtype[src_b] != 17u ||
        env->tile_reg_dtype[bias] != 17u ||
        env->tile_reg_elem_bytes[src_a] != 4u ||
        env->tile_reg_elem_bytes[src_b] != 4u ||
        env->tile_reg_elem_bytes[bias] != 4u ||
        env->tile_reg_bytes[src_a] < m * 8u * 4u ||
        env->tile_reg_bytes[src_b] < kdim * 8u * 4u ||
        env->tile_reg_bytes[bias] < n * 4u ||
        env->tile_reg_valid_rows[bias] != 1u ||
        env->tile_reg_valid_cols[bias] < n ||
        env->tile_reg_cols[bias] < n) {
        return false;
    }

    linx_tile_mamulb(env, src_a, src_b, size_code);
    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            const int32_t product = (int32_t)env->tile_acc[i * 8u + j];
            const int32_t bias_value = (int32_t)env->tile_reg[bias][j];
            env->tile_acc[i * 8u + j] =
                (uint32_t)(int32_t)((int64_t)product + bias_value);
        }
    }
    return true;
}

static void linx_tile_mamulb_acc(CPULinxState *env, unsigned src_a, unsigned src_b,
                                 unsigned size_code)
{
    if (src_a >= 32 || src_b >= 32) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned m = env->lb[0] ? MIN((unsigned)env->lb[0], 8u) : 8u;
    const unsigned n = env->lb[1] ? MIN((unsigned)env->lb[1], 8u) : 8u;
    const unsigned kdim = env->lb[2] ? MIN((unsigned)env->lb[2], 8u) : 8u;

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES || (bytes64 & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    env->tile_acc_bytes = (uint32_t)bytes64;

    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            int64_t acc = (int32_t)env->tile_acc[i * 8u + j];
            for (unsigned k = 0; k < kdim; k++) {
                const int32_t a = (int32_t)env->tile_reg[src_a][i * 8u + k];
                const int32_t b = (int32_t)env->tile_reg[src_b][k * 8u + j];
                acc += (int64_t)a * (int64_t)b;
            }
            env->tile_acc[i * 8u + j] = (uint32_t)(int32_t)acc;
        }
    }
}

static void linx_tile_acccvt(CPULinxState *env, unsigned dst_tile, unsigned size_code)
{
    if (dst_tile >= 32) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES || (bytes64 & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const unsigned words = (unsigned)(bytes64 / 4u);

    if (env->tile_acc_bytes < bytes64) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    for (unsigned i = 0; i < words && i < LINX_TILE_MAX_WORDS; i++) {
        env->tile_reg[dst_tile][i] = env->tile_acc[i];
    }
    for (unsigned i = words; i < LINX_TILE_MAX_WORDS; i++) {
        env->tile_reg[dst_tile][i] = 0;
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
    linx_tile_set_elem_bytes(env, dst_tile, 4u);
    linx_tile_set_dtype(env, dst_tile, env->tile_dtype);
    if (!linx_tile_set_block_shape(env, dst_tile, (uint32_t)bytes64, 4u)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
    }
}

static unsigned linx_ior_desc_reg_in_authored_order(uint64_t desc,
                                                     unsigned slot)
{
    /*
     * The B.IOR assembly list is encoded as RegSrc1, RegSrc0, RegSrc2,
     * followed by RegDst.  Keep both RI consumers on this canonical order.
     */
    static const unsigned shifts[] = { 10, 5, 15, 0 };

    g_assert(slot < ARRAY_SIZE(shifts));
    return (desc >> shifts[slot]) & 0x1fu;
}

static bool linx_tile_resolve_ior(const CPULinxState *env, unsigned slot,
                                  unsigned *addr_reg_out)
{
    /*
     * Canonical v0.4 VEC contract: RI registers are an ordered namespace bound
     * by header B.IOR descriptors.
     *
     * Bring-up streams also use the trailing RegDst field as part of the RI
     * binding list for pair forms such as `B.IOR [a6],[a7]`.
     */
    if (slot >= LINX_TILE_MAX_IOR) {
        return false;
    }

    unsigned cur = 0;
    const unsigned desc_count = MIN(env->tile_ior_count, LINX_TILE_MAX_IOR);
    for (unsigned i = 0; i < desc_count; i++) {
        const uint64_t desc = env->tile_ior_desc[i];

        /*
         * Bring-up launcher streams treat RI bindings as the authored B.IOR
         * operand-list order across descriptors.
         */
        for (unsigned s = 0; s < 4; s++) {
            const unsigned reg = linx_ior_desc_reg_in_authored_order(desc, s);
            if (reg == 0) {
                continue;
            }
            if (cur == slot) {
                if (reg >= LINX_GPR_COUNT) {
                    return false;
                }
                *addr_reg_out = reg;
                return true;
            }
            cur++;
        }
    }

    return false;
}

static void linx_vec_capture_ri_values(CPULinxState *env)
{
    env->vec_ri_count = 0;

    const unsigned desc_count = MIN(env->tile_ior_count, LINX_TILE_MAX_IOR);
    for (unsigned i = 0; i < desc_count; i++) {
        const uint64_t desc = env->tile_ior_desc[i];

        for (unsigned s = 0; s < 4; s++) {
            const unsigned reg = linx_ior_desc_reg_in_authored_order(desc, s);
            if (reg == 0 || reg >= LINX_GPR_COUNT) {
                continue;
            }
            if (env->vec_ri_count >= LINX_VEC_RI_MAX) {
                return;
            }
            env->vec_ri_value[env->vec_ri_count++] = env->gpr[reg];
        }
    }
}

static bool linx_tile_get_base_reg(const CPULinxState *env, unsigned *addr_reg_out)
{
    if (env->tile_ior_count == 0) {
        return false;
    }
    const uint64_t desc = env->tile_ior_desc[env->tile_ior_count - 1u];
    const unsigned src0 = (unsigned)((desc >> 5) & 0x1fu);
    const unsigned src1 = (unsigned)((desc >> 10) & 0x1fu);
    const unsigned base = (src1 != 0) ? src1 : src0;
    if (base >= LINX_GPR_COUNT) {
        return false;
    }
    *addr_reg_out = base;
    return true;
}

static uint64_t linx_tile_get_stride_bytes(const CPULinxState *env)
{
    if (env->tile_ior_count == 0) {
        return 0;
    }
    const uint64_t desc = env->tile_ior_desc[env->tile_ior_count - 1u];
    const unsigned src0 = (unsigned)((desc >> 5) & 0x1fu);
    return src0 < LINX_GPR_COUNT ? env->gpr[src0] : 0;
}

static LinxTileIOTDesc linx_tile_get_iot_desc(const CPULinxState *env,
                                              unsigned index)
{
    if (env->tile_iot_count) {
        return linx_tile_decode_iot(env->tile_iot_desc[index]);
    }

    LinxTileIOTDesc d = {
        .src0 = env->tile_iot_src0 & 0x3f,
        .src1 = env->tile_iot_src1 & 0x3f,
        .dst = env->tile_iot_dst & 0x7,
        .last = env->tile_iot_grp & 0x1,
        .flags = env->tile_iot_flags & 0xf,
        .reg = env->tile_iot_reg & 0x1f,
        .size = env->tile_iot_size & 0x1f,
        .has_size = env->tile_iot_size != 0,
    };
    return d;
}

static bool linx_tile_get_bound_source(const CPULinxState *env,
                                       unsigned index, unsigned source,
                                       unsigned *tile_out)
{
    if (index >= env->tile_iot_count || source > 1 ||
        (env->tile_iot_src_valid[index] & (1u << source)) == 0) {
        return false;
    }
    *tile_out = env->tile_iot_src_phys[index][source];
    return true;
}

static bool linx_tile_get_bound_output(const CPULinxState *env,
                                       unsigned index, unsigned *tile_out)
{
    if (index >= env->tile_iot_count || !env->tile_iot_output_valid[index]) {
        return false;
    }
    *tile_out = env->tile_iot_output_phys[index];
    return true;
}

static bool linx_tile_complete_bound_output(CPULinxState *env,
                                            uint8_t live[LINX_TILE_HAND_COUNT],
                                            uint8_t reserved[LINX_TILE_HAND_COUNT],
                                            uint8_t order[LINX_TILE_HAND_COUNT]
                                                         [LINX_TILE_HAND_DEPTH],
                                            uint8_t count_by_hand[LINX_TILE_HAND_COUNT],
                                            unsigned index)
{
    unsigned tile;

    if (!linx_tile_get_bound_output(env, index, &tile)) {
        return false;
    }
    const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
    const unsigned depth = tile % LINX_TILE_HAND_DEPTH;
    reserved[hand] &= ~(1u << depth);
    linx_tile_publish_output(live, tile);
    return linx_tile_publish_order_state(order, count_by_hand, tile);
}

static void linx_tile_consume_bound_sources(
    CPULinxState *env, uint8_t live[LINX_TILE_HAND_COUNT], unsigned index,
    const LinxTileIOTDesc *desc,
    uint8_t order[LINX_TILE_HAND_COUNT][LINX_TILE_HAND_DEPTH],
    uint8_t count_by_hand[LINX_TILE_HAND_COUNT],
    uint8_t *carrier_valid, uint8_t *carrier)
{
    for (unsigned source = 0; source < 2; source++) {
        unsigned tile;
        const unsigned absent = source == 0 ? LINX_IOT_S0V : LINX_IOT_S1V;
        const unsigned reuse = source == 0 ? LINX_IOT_S0R : LINX_IOT_S1R;

        if ((desc->flags & absent) != 0 ||
            !linx_tile_get_bound_source(env, index, source, &tile)) {
            continue;
        }
        linx_tile_release_source(live, order, count_by_hand, tile,
                                 (desc->flags & reuse) != 0,
                                 carrier_valid, carrier);
    }
}

static bool linx_tile_collect_tepl_bindings(
    const CPULinxState *env, uint32_t op,
    unsigned sources[LINX_TILE_MAX_IOT * 2], unsigned *source_count_out,
    unsigned *output_index_out, unsigned *dst_tile_out,
    unsigned *size_code_out)
{
    unsigned output_count = 0;
    unsigned output_index = 0;
    unsigned dst_tile = 0;

    if (env->tile_iot_count == 0u ||
        !linx_tile_collect_sources(env, sources, source_count_out) ||
        linx_tile_tepl_source_arity(op) < 0 ||
        *source_count_out != (unsigned)linx_tile_tepl_source_arity(op)) {
        return false;
    }

    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        const LinxTileIOTDesc d = linx_tile_decode_iot(env->tile_iot_desc[i]);
        const bool final_desc = i + 1u == env->tile_iot_count;

        if ((d.last != 0u) != final_desc) {
            return false;
        }
        if (env->tile_iot_output_valid[i]) {
            if (++output_count != 1u || !final_desc || !d.has_size ||
                !linx_tile_get_bound_output(env, i, &dst_tile)) {
                return false;
            }
            output_index = i;
            *size_code_out = d.size & 0x1fu;
        } else if (d.has_size) {
            return false;
        }
    }

    if (output_count != 1u || !linx_tile_size_code_valid(*size_code_out)) {
        return false;
    }
    *output_index_out = output_index;
    *dst_tile_out = dst_tile;
    return true;
}

static bool linx_tile_collect_interleave_bindings(
    const CPULinxState *env, unsigned sources[2], unsigned outputs[2],
    unsigned output_indices[2], unsigned *size_code_out)
{
    unsigned collected_sources[LINX_TILE_MAX_IOT * 2];
    unsigned source_count = 0;

    if (env->tile_iot_count != 2u ||
        !linx_tile_collect_sources(env, collected_sources, &source_count) ||
        source_count != 2u || env->tile_iot_src_valid[0] != 3u ||
        env->tile_iot_src_valid[1] != 0u) {
        return false;
    }
    sources[0] = collected_sources[0];
    sources[1] = collected_sources[1];

    for (unsigned i = 0; i < 2; i++) {
        const LinxTileIOTDesc d =
            linx_tile_decode_iot(env->tile_iot_desc[i]);
        const bool final_desc = i == 1u;
        unsigned output = 0;

        if ((d.last != 0u) != final_desc || !d.has_size ||
            !linx_tile_get_bound_output(env, i, &output) ||
            !linx_tile_size_code_valid(d.size & 0x1fu)) {
            return false;
        }
        if (i == 0u) {
            *size_code_out = d.size & 0x1fu;
        } else if ((d.size & 0x1fu) != *size_code_out) {
            return false;
        }
        outputs[i] = output;
        output_indices[i] = i;
    }
    return true;
}

static bool linx_tile_collect_cube_sources(
    const CPULinxState *env, unsigned required_sources,
    unsigned sources[LINX_TILE_MAX_IOT * 2], unsigned *size_code_out)
{
    unsigned source_count = 0;
    unsigned size_count = 0;

    if (env->tile_iot_count == 0u ||
        !linx_tile_collect_sources(env, sources, &source_count) ||
        source_count != required_sources) {
        return false;
    }

    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        const LinxTileIOTDesc d = linx_tile_decode_iot(env->tile_iot_desc[i]);
        const bool final_desc = i + 1u == env->tile_iot_count;

        if ((d.last != 0u) != final_desc || env->tile_iot_output_valid[i]) {
            return false;
        }
        if (d.has_size) {
            if (!final_desc || ++size_count != 1u) {
                return false;
            }
            *size_code_out = d.size & 0x1fu;
        }
    }

    return size_count == 1u && linx_tile_size_code_valid(*size_code_out);
}

static bool linx_tile_transfer_preflight(const CPULinxState *env,
                                         unsigned size_code,
                                         LinxTMATransferDir dir)
{
    if (!linx_tile_size_code_valid(size_code)) {
        return false;
    }
    const uint64_t bytes = 1ull << (size_code + 4u);
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(env->tile_dtype);
    if ((bytes % elem_bytes) != 0) {
        return false;
    }
    const LinxTileFormatDesc fmt =
        linx_tile_decode_tma_format(env->tile_arg_format, dir);
    if (!fmt.valid) {
        return false;
    }

    uint32_t tr_outer;
    uint32_t tr_inner;
    uint32_t gm_outer;
    uint32_t gm_inner;
    if (!linx_tile_resolve_transfer_shape(
            env, (uint32_t)(bytes / elem_bytes), &tr_outer, &tr_inner,
            &gm_outer, &gm_inner)) {
        return false;
    }
    const LinxTileLayout tr_layout = dir == LINX_TMA_GM_TO_TR
                                      ? fmt.dst : fmt.src;
    return linx_tile_layout_shape_valid(tr_layout, tr_outer, tr_inner,
                                        elem_bytes);
}

static bool linx_tile_preflight_tma(
    CPULinxState *env, uint8_t planned_live[LINX_TILE_HAND_COUNT],
    uint8_t *planned_carrier_valid, uint8_t *planned_carrier)
{
    const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
    unsigned addr_reg;

    if (!linx_tile_get_base_reg(env, &addr_reg)) {
        return false;
    }

    switch (env->tile_func & 0x1f) {
    case LINX_TMA_MGATHER:
    case LINX_TMA_MGATHER_MASK:
    case LINX_TMA_MSCATTER:
    case LINX_TMA_MSCATTER_MASK:
    case LINX_TMA_MGATHER_CAS: {
        unsigned sources[LINX_TILE_MAX_IOT * 2];
        unsigned source_count = 0;
        const unsigned required_sources =
            (env->tile_func & 0x1f) == LINX_TMA_MGATHER ? 1u :
            (env->tile_func & 0x1f) == LINX_TMA_MGATHER_MASK ? 2u :
            (env->tile_func & 0x1f) == LINX_TMA_MSCATTER ? 2u : 3u;
        const bool produces_output =
            (env->tile_func & 0x1f) == LINX_TMA_MGATHER ||
            (env->tile_func & 0x1f) == LINX_TMA_MGATHER_MASK ||
            (env->tile_func & 0x1f) == LINX_TMA_MGATHER_CAS;
        bool output_seen = false;

        if (!linx_tile_collect_sources(env, sources, &source_count) ||
            source_count < required_sources) {
            return false;
        }
        for (unsigned i = 0; i < env->tile_iot_count; i++) {
            const LinxTileIOTDesc d = linx_tile_get_iot_desc(env, i);
            const unsigned size_code = d.has_size ? d.size & 0x1f
                                                  : env->tile_iot_size & 0x1f;
            if (!linx_tile_size_code_valid(size_code)) {
                return false;
            }
            if (env->tile_iot_output_valid[i]) {
                output_seen = true;
                linx_tile_publish_output(planned_live,
                                         env->tile_iot_output_phys[i]);
            }
            linx_tile_consume_bound_sources(env, planned_live, i, &d,
                                            NULL, NULL,
                                            planned_carrier_valid,
                                            planned_carrier);
        }
        return produces_output == output_seen || (!produces_output && !output_seen);
    }
    default:
        break;
    }

    for (unsigned i = 0; i < count; i++) {
        const LinxTileIOTDesc d = linx_tile_get_iot_desc(env, i);
        const unsigned size_code = d.has_size ? d.size & 0x1f
                                              : env->tile_iot_size & 0x1f;
        unsigned tile;

        switch (env->tile_func & 0x1f) {
        case LINX_TMA_TLOAD:
            if (!linx_tile_transfer_preflight(env, size_code,
                                              LINX_TMA_GM_TO_TR) ||
                !linx_tile_get_bound_output(env, i, &tile)) {
                return false;
            }
            linx_tile_publish_output(planned_live, tile);
            break;
        case LINX_TMA_TSTORE: {
            const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
            const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
            if (!linx_tile_transfer_preflight(env, size_code,
                                              LINX_TMA_TR_TO_GM)) {
                return false;
            }
            if (src0_present) {
                if (!linx_tile_get_bound_source(env, i, 0, &tile)) {
                    return false;
                }
            } else if (src1_present) {
                if (!linx_tile_get_bound_source(env, i, 1, &tile)) {
                    return false;
                }
            } else {
                return false;
            }
            const uint64_t bytes = 1ull << (size_code + 4u);
            if (env->tile_reg_bytes[tile] < bytes) {
                return false;
            }
            linx_tile_consume_bound_sources(env, planned_live, i, &d,
                                            NULL, NULL,
                                            planned_carrier_valid,
                                            planned_carrier);
            break;
        }
        case LINX_TMA_TMOV: {
            const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
            const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
            unsigned src_tile;
            unsigned dst_tile;
            if (!linx_tile_size_code_valid(size_code)) {
                return false;
            }
            if (src0_present) {
                if (!linx_tile_get_bound_source(env, i, 0, &src_tile)) {
                    return false;
                }
            } else if (src1_present) {
                if (!linx_tile_get_bound_source(env, i, 1, &src_tile)) {
                    return false;
                }
            } else {
                return false;
            }
            if (!linx_tile_get_bound_output(env, i, &dst_tile) ||
                env->tile_reg_bytes[src_tile] < (1ull << (size_code + 4u))) {
                return false;
            }
            linx_tile_consume_bound_sources(env, planned_live, i, &d,
                                            NULL, NULL,
                                            planned_carrier_valid,
                                            planned_carrier);
            linx_tile_publish_output(planned_live, dst_tile);
            break;
        }
        case LINX_TMA_TPREFETCH:
            if (!linx_tile_transfer_preflight(env, size_code,
                                              LINX_TMA_GM_TO_TR)) {
                return false;
            }
            break;
        case LINX_TMA_MGATHER:
        case LINX_TMA_MGATHER_MASK:
        case LINX_TMA_MGATHER_CAS:
            if (!linx_tile_size_code_valid(size_code) ||
                !linx_tile_get_bound_output(env, i, &tile)) {
                return false;
            }
            linx_tile_publish_output(planned_live, tile);
            break;
        case LINX_TMA_MSCATTER:
        case LINX_TMA_MSCATTER_MASK:
            if (!linx_tile_size_code_valid(size_code)) {
                return false;
            }
            linx_tile_consume_bound_sources(env, planned_live, i, &d,
                                            NULL, NULL,
                                            planned_carrier_valid,
                                            planned_carrier);
            break;
        case LINX_TMA_TCVT_COMPAT: {
            const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
            const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
            unsigned src_tile;
            unsigned dst_tile;
            if (!linx_tile_size_code_valid(size_code)) {
                return false;
            }
            if (src0_present) {
                if (!linx_tile_get_bound_source(env, i, 0, &src_tile)) {
                    return false;
                }
            } else if (src1_present) {
                if (!linx_tile_get_bound_source(env, i, 1, &src_tile)) {
                    return false;
                }
            } else {
                return false;
            }
            if (!linx_tile_get_bound_output(env, i, &dst_tile) ||
                env->tile_reg_bytes[src_tile] <
                    (1ull << (size_code + 4u))) {
                return false;
            }
            linx_tile_consume_bound_sources(env, planned_live, i, &d,
                                            NULL, NULL,
                                            planned_carrier_valid,
                                            planned_carrier);
            linx_tile_publish_output(planned_live, dst_tile);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

void HELPER(linx_tile_reset_block)(CPULinxState *env)
{
    linx_tile_unpin_bindings(env);
    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        if (env->tile_iot_output_valid[i]) {
            const unsigned tile = env->tile_iot_output_phys[i];
            const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
            const unsigned depth = tile % LINX_TILE_HAND_DEPTH;
            if (hand < LINX_TILE_HAND_COUNT) {
                env->tile_hand_reserved[hand] &= ~(1u << depth);
            }
        }
    }
    env->tile_arg_format = 0;
    env->tile_attr_pad = 0;
    env->tile_attr_dtype = 0;
    env->tile_ior_count = 0;
    env->vec_ri_count = 0;
    env->tile_iot_count = 0;
    memset(env->tile_iot_desc, 0, sizeof(env->tile_iot_desc));
    memset(env->tile_iot_src_valid, 0, sizeof(env->tile_iot_src_valid));
    memset(env->tile_iot_src_phys, 0, sizeof(env->tile_iot_src_phys));
    memset(env->tile_iot_output_valid, 0,
           sizeof(env->tile_iot_output_valid));
    memset(env->tile_iot_output_phys, 0,
           sizeof(env->tile_iot_output_phys));
}

void HELPER(linx_tile_set_arg)(CPULinxState *env, uint32_t format)
{
    env->tile_arg_format = format & 0x1fu;
}

void HELPER(linx_tile_set_attr)(CPULinxState *env, uint32_t packed)
{
    env->tile_attr_raw = packed;
    env->tile_attr_dtype = (packed >> 7) & 0x1fu;
    env->tile_attr_pad = (packed >> 12) & 0x1fu;
}

void HELPER(linx_tile_append_ior)(CPULinxState *env, uint64_t packed)
{
    if (env->tile_ior_count >= LINX_TILE_MAX_IOR) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    env->tile_ior_desc[env->tile_ior_count++] = packed;
}

void HELPER(linx_tile_append_iot)(CPULinxState *env, uint64_t packed)
{
    const LinxTileIOTDesc desc = linx_tile_decode_iot(packed);
    const bool vector_block = env->blocktype == 0 || env->blocktype == 1 ||
                              env->blocktype == 4 || env->blocktype == 5 ||
                              env->blocktype == LINX_BLOCK_TEPL;
    const uint32_t tma_func = env->tile_func & 0x1f;
    const bool tile_output =
        (env->blocktype == LINX_BLOCK_TMA &&
         (tma_func == LINX_TMA_TLOAD ||
          tma_func == LINX_TMA_TMOV ||
          tma_func == LINX_TMA_MGATHER ||
          tma_func == LINX_TMA_MGATHER_MASK ||
          tma_func == LINX_TMA_MGATHER_CAS ||
          tma_func == LINX_TMA_TCVT_COMPAT)) ||
        (env->blocktype == LINX_BLOCK_CUBE &&
         (env->tile_func & 0x1f) == LINX_CUBE_ACCCVT) ||
        env->blocktype == LINX_BLOCK_TEPL;

    if (desc.has_size && env->tile_iot_count > 0) {
        const unsigned prev_idx = env->tile_iot_count - 1;
        LinxTileIOTDesc prev = linx_tile_decode_iot(env->tile_iot_desc[prev_idx]);
        const bool reg_compatible = (desc.reg == 0) || (prev.reg == desc.reg);
        if (!prev.has_size &&
            prev.src0 == desc.src0 &&
            prev.src1 == desc.src1 &&
            prev.dst == desc.dst &&
            prev.last == desc.last &&
            prev.flags == desc.flags &&
            reg_compatible) {
            env->tile_iot_desc[prev_idx] |=
                (((uint64_t)(desc.size & 0x1fu)) << LINX_TILE_IOT_SIZE_SHIFT) |
                (1ull << LINX_TILE_IOT_HAS_SIZE_SHIFT);
            return;
        }
    }

    if (env->tile_iot_count >= LINX_TILE_MAX_IOT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned index = env->tile_iot_count;
    uint8_t src_valid = 0;
    unsigned src_phys[2] = { 0, 0 };
    bool src_was_pinned[2] = { false, false };

    if ((desc.flags & LINX_IOT_S0V) == 0) {
        if (!linx_tile_resolve_source(env, env->tile_hand_live,
                                      desc.src0, &src_phys[0])) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        src_valid |= 1u;
    }
    if ((desc.flags & LINX_IOT_S1V) == 0) {
        if (!linx_tile_resolve_source(env, env->tile_hand_live,
                                      desc.src1, &src_phys[1])) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        src_valid |= 2u;
    }

    const uint16_t owner = 1u << (env->acr & 0xfu);
    for (unsigned source = 0; source < 2; source++) {
        if ((src_valid & (1u << source)) != 0) {
            const uint16_t pinned = env->tile_pin_owner[src_phys[source]];
            if (pinned != 0 && pinned != owner) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            src_was_pinned[source] = pinned != 0;
        }
    }
    for (unsigned source = 0; source < 2; source++) {
        if ((src_valid & (1u << source)) != 0) {
            linx_tile_pin_source(env, src_phys[source]);
            env->tile_iot_src_phys[index][source] = src_phys[source];
        }
    }

    env->tile_iot_src_valid[index] = src_valid;
    env->tile_iot_output_valid[index] = 0;

    if (desc.has_size && (vector_block || tile_output)) {
        uint8_t occupied[LINX_TILE_HAND_COUNT];
        unsigned dst_tile;
        const uint64_t bytes64 = desc.has_size && desc.size < 60u
                                 ? (1ull << (desc.size + 4u)) : 0;

        for (unsigned hand = 0; hand < LINX_TILE_HAND_COUNT; hand++) {
            occupied[hand] = env->tile_hand_live[hand] |
                             env->tile_hand_reserved[hand];
        }
        for (unsigned tile = 0;
             tile < LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH; tile++) {
            if (env->tile_pin_owner[tile]) {
                occupied[tile / LINX_TILE_HAND_DEPTH] |=
                    1u << (tile % LINX_TILE_HAND_DEPTH);
            }
        }
        if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES ||
            (bytes64 & 3u) != 0 ||
            !linx_tile_reserve_output(occupied, &desc, &dst_tile)) {
            for (unsigned source = 0; source < 2; source++) {
                if ((src_valid & (1u << source)) != 0 &&
                    !src_was_pinned[source]) {
                    env->tile_pin_owner[src_phys[source]] &= ~owner;
                }
            }
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        const unsigned hand = dst_tile / LINX_TILE_HAND_DEPTH;
        const unsigned depth = dst_tile % LINX_TILE_HAND_DEPTH;
        env->tile_hand_reserved[hand] |= 1u << depth;
        env->tile_iot_output_valid[index] = 1;
        env->tile_iot_output_phys[index] = dst_tile;
        linx_tile_invalidate_acc_sources_on_output(
            dst_tile, &env->tile_acc_sources_valid,
            env->tile_acc_src0, env->tile_acc_src1);
        memset(env->tile_reg[dst_tile], 0, sizeof(env->tile_reg[dst_tile]));
        env->tile_reg_bytes[dst_tile] = bytes64;
        linx_tile_set_elem_bytes(env, dst_tile,
                                 linx_tile_dtype_elem_bytes(env->tile_dtype));
        linx_tile_set_dtype(env, dst_tile, env->tile_dtype);
        if (!linx_tile_set_block_shape(
                env, dst_tile, (uint32_t)bytes64,
                linx_tile_dtype_elem_bytes(env->tile_dtype))) {
            for (unsigned source = 0; source < 2; source++) {
                if ((src_valid & (1u << source)) != 0 &&
                    !src_was_pinned[source]) {
                    env->tile_pin_owner[src_phys[source]] &= ~owner;
                }
            }
            env->tile_hand_reserved[hand] &= ~(1u << depth);
            env->tile_iot_output_valid[index] = 0;
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }

    env->tile_iot_desc[env->tile_iot_count++] = packed;

}

void HELPER(linx_tile_commit)(CPULinxState *env)
{
    uint8_t live[LINX_TILE_HAND_COUNT];
    uint8_t reserved[LINX_TILE_HAND_COUNT];
    uint8_t order[LINX_TILE_HAND_COUNT][LINX_TILE_HAND_DEPTH];
    uint8_t count_by_hand[LINX_TILE_HAND_COUNT];
    uint8_t carrier_valid = env->tile_acc_carrier_valid;
    uint8_t carrier = env->tile_acc_carrier;
    uint8_t acc_sources_valid = env->tile_acc_sources_valid;
    uint8_t acc_src0 = env->tile_acc_src0;
    uint8_t acc_src1 = env->tile_acc_src1;

    if (env->tile_iot_count == 0 && env->tile_iot_valid == 0) {
        return;
    }
    memcpy(live, env->tile_hand_live, sizeof(live));
    memcpy(reserved, env->tile_hand_reserved, sizeof(reserved));
    memcpy(order, env->tile_hand_order, sizeof(order));
    memcpy(count_by_hand, env->tile_hand_count, sizeof(count_by_hand));

    if (env->blocktype == LINX_BLOCK_TMA) {
        uint8_t planned_live[LINX_TILE_HAND_COUNT];
        uint8_t planned_carrier_valid = carrier_valid;
        uint8_t planned_carrier = carrier;
        memcpy(planned_live, live, sizeof(planned_live));
        if (!linx_tile_preflight_tma(env, planned_live,
                                     &planned_carrier_valid,
                                     &planned_carrier)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        }
    }

    switch (env->blocktype) {
    case LINX_BLOCK_TEPL: {
        unsigned sources[LINX_TILE_MAX_IOT * 2];
        unsigned source_count = 0;
        unsigned output_index = 0;
        unsigned dst_tile = 0;
        unsigned size_code = 0;
        const uint32_t op = env->tile_func & 0x3ffu;

        if (op == 0x08au || op == 0x08bu) {
            unsigned interleave_sources[2];
            unsigned interleave_outputs[2];
            unsigned interleave_output_indices[2];

            if (!linx_tile_collect_interleave_bindings(
                    env, interleave_sources, interleave_outputs,
                    interleave_output_indices, &size_code) ||
                !linx_tile_interleave(env, op, interleave_outputs,
                                      interleave_sources, size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            for (unsigned i = 0; i < 2; i++) {
                linx_tile_invalidate_acc_sources_on_output(
                    interleave_outputs[i], &acc_sources_valid,
                    acc_src0, acc_src1);
            }
            for (unsigned i = 0; i < env->tile_iot_count; i++) {
                const LinxTileIOTDesc d =
                    linx_tile_decode_iot(env->tile_iot_desc[i]);
                linx_tile_consume_bound_sources(
                    env, live, i, &d, order, count_by_hand,
                    &carrier_valid, &carrier);
            }
            for (unsigned i = 0; i < 2; i++) {
                if (!linx_tile_complete_bound_output(
                        env, live, reserved, order, count_by_hand,
                        interleave_output_indices[i])) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
            }
            break;
        }

        if (!linx_tile_collect_tepl_bindings(
                env, op, sources, &source_count, &output_index,
                &dst_tile, &size_code)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        linx_tile_invalidate_acc_sources_on_output(
            dst_tile, &acc_sources_valid, acc_src0, acc_src1);
        linx_tile_tepl(env, dst_tile, sources, source_count, size_code, op);
        for (unsigned i = 0; i < env->tile_iot_count; i++) {
            const LinxTileIOTDesc d =
                linx_tile_decode_iot(env->tile_iot_desc[i]);
            linx_tile_consume_bound_sources(env, live, i, &d, order,
                                            count_by_hand, &carrier_valid,
                                            &carrier);
        }
        if (!linx_tile_complete_bound_output(
                env, live, reserved, order, count_by_hand, output_index)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        }
        break;
    }
    case LINX_BLOCK_TMA:
        switch (env->tile_func & 0x1f) {
        case LINX_TMA_TLOAD: {
            const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
            for (unsigned i = 0; i < count; i++) {
                LinxTileIOTDesc d;
                if (env->tile_iot_count) {
                    d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                } else {
                    d.src0 = env->tile_iot_src0 & 0x3f;
                    d.src1 = env->tile_iot_src1 & 0x3f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.last = env->tile_iot_grp & 0x1;
                    d.flags = env->tile_iot_flags & 0xf;
                    d.reg = env->tile_iot_reg & 0x1f;
                    d.size = env->tile_iot_size & 0x1f;
                    d.has_size = env->tile_iot_size != 0;
                }

                unsigned addr_reg = 0;
                if (!linx_tile_get_base_reg(env, &addr_reg)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const unsigned size_code = d.has_size ? (d.size & 0x1f)
                                                      : (env->tile_iot_size & 0x1f);
                if (!linx_tile_size_code_valid(size_code)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                }

                unsigned dst_tile = 0;
                if (!linx_tile_get_bound_output(env, i, &dst_tile)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_invalidate_acc_sources_on_output(
                    dst_tile, &acc_sources_valid, acc_src0, acc_src1);
                if (!acc_sources_valid) {
                    /* A fault may leave a non-atomic backing prefix. */
                    env->tile_acc_sources_valid = 0;
                }
                /*
                 * TLOAD may update a backing prefix before a data fault, as
                 * permitted for non-atomic tile-memory beats.  Retry selects
                 * the same still-unpublished slot and overwrites that prefix;
                 * only full success publishes hand liveness.
                 */
                linx_tile_load(env, dst_tile, addr_reg, size_code,
                               linx_tile_get_stride_bytes(env));
                if (!linx_tile_complete_bound_output(
                        env, live, reserved, order, count_by_hand, i)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                }
            }
            break;
        }
        case LINX_TMA_TMOV: {
            const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
            for (unsigned i = 0; i < count; i++) {
                LinxTileIOTDesc d;
                if (env->tile_iot_count) {
                    d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                } else {
                    d.src0 = env->tile_iot_src0 & 0x3f;
                    d.src1 = env->tile_iot_src1 & 0x3f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.last = env->tile_iot_grp & 0x1;
                    d.flags = env->tile_iot_flags & 0xf;
                    d.reg = env->tile_iot_reg & 0x1f;
                    d.size = env->tile_iot_size & 0x1f;
                    d.has_size = env->tile_iot_size != 0;
                }
                const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
                const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
                unsigned src_tile = 0;
                unsigned dst_tile = 0;
                if (src0_present) {
                    if (!linx_tile_get_bound_source(env, i, 0, &src_tile)) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                } else if (src1_present) {
                    if (!linx_tile_get_bound_source(env, i, 1, &src_tile)) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                } else {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                if (!linx_tile_get_bound_output(env, i, &dst_tile)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const unsigned size_code = d.has_size ? (d.size & 0x1f)
                                                      : (env->tile_iot_size & 0x1f);
                const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
                if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES ||
                    src_tile >= 32 || dst_tile >= 32 ||
                    env->tile_reg_bytes[src_tile] < bytes64) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_invalidate_acc_sources_on_output(
                    dst_tile, &acc_sources_valid, acc_src0, acc_src1);
                memcpy(env->tile_reg[dst_tile], env->tile_reg[src_tile],
                       (size_t)bytes64);
                memset((uint8_t *)env->tile_reg[dst_tile] + bytes64, 0,
                       LINX_TILE_MAX_BYTES - (size_t)bytes64);
                env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
                linx_tile_set_elem_bytes(env, dst_tile,
                                         env->tile_reg_elem_bytes[src_tile]);
                linx_tile_set_dtype(env, dst_tile,
                                    env->tile_reg_dtype[src_tile]);
                linx_tile_copy_shape(env, dst_tile, src_tile);
                linx_tile_consume_bound_sources(env, live, i, &d,
                                                order, count_by_hand,
                                                &carrier_valid, &carrier);
                if (!linx_tile_complete_bound_output(
                        env, live, reserved, order, count_by_hand, i)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                }
            }
            break;
        }
        case LINX_TMA_TPREFETCH: {
            const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
            for (unsigned i = 0; i < count; i++) {
                const LinxTileIOTDesc d = env->tile_iot_count
                                          ? linx_tile_decode_iot(env->tile_iot_desc[i])
                                          : linx_tile_get_iot_desc(env, 0);
                unsigned addr_reg = 0;
                if (!linx_tile_get_base_reg(env, &addr_reg)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const unsigned size_code = d.has_size ? (d.size & 0x1f)
                                                      : (env->tile_iot_size & 0x1f);
                linx_tile_prefetch(env, addr_reg, size_code);
            }
            break;
        }
        case LINX_TMA_TSTORE: {
            const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
            for (unsigned i = 0; i < count; i++) {
                LinxTileIOTDesc d;
                if (env->tile_iot_count) {
                    d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                } else {
                    d.src0 = env->tile_iot_src0 & 0x3f;
                    d.src1 = env->tile_iot_src1 & 0x3f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.last = env->tile_iot_grp & 0x1;
                    d.flags = env->tile_iot_flags & 0xf;
                    d.reg = env->tile_iot_reg & 0x1f;
                    d.size = env->tile_iot_size & 0x1f;
                    d.has_size = env->tile_iot_size != 0;
                }

                unsigned addr_reg = 0;
                if (!linx_tile_get_base_reg(env, &addr_reg)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
                const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
                unsigned src_tile = 0;
                if (src0_present) {
                    if (!linx_tile_get_bound_source(env, i, 0, &src_tile)) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                } else if (src1_present) {
                    if (!linx_tile_get_bound_source(env, i, 1, &src_tile)) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                } else {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const unsigned size_code = d.has_size ? (d.size & 0x1f)
                                                      : (env->tile_iot_size & 0x1f);
                if (!linx_tile_size_code_valid(size_code)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                }
                /*
                 * TMA's Normal-memory domain permits one TSTORE operation to
                 * decompose into externally visible, non-atomic beats.  A
                 * data fault may therefore leave a restartable prefix;
                 * allocator/source consumption publishes only after success.
                 */
                linx_tile_store(env, src_tile, addr_reg, size_code,
                                linx_tile_get_stride_bytes(env));
                linx_tile_consume_bound_sources(env, live, i, &d,
                                                order, count_by_hand,
                                                &carrier_valid, &carrier);
            }
            break;
        }
        case LINX_TMA_MGATHER:
        case LINX_TMA_MGATHER_MASK:
        case LINX_TMA_MSCATTER:
        case LINX_TMA_MSCATTER_MASK:
        case LINX_TMA_MGATHER_CAS: {
            unsigned sources[LINX_TILE_MAX_IOT * 2];
            unsigned source_count = 0;
            unsigned output_index = UINT_MAX;
            unsigned output_tile = 0;
            unsigned addr_reg = 0;
            unsigned size_code = env->tile_iot_size & 0x1f;
            const uint32_t func = env->tile_func & 0x1f;

            if (!linx_tile_get_base_reg(env, &addr_reg) ||
                !linx_tile_collect_sources(env, sources, &source_count)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            for (unsigned i = 0; i < env->tile_iot_count; i++) {
                LinxTileIOTDesc d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                if (d.has_size) {
                    size_code = d.size & 0x1f;
                }
                if (env->tile_iot_output_valid[i]) {
                    output_index = i;
                    output_tile = env->tile_iot_output_phys[i];
                }
            }

            switch (func) {
            case LINX_TMA_MGATHER:
                if (source_count < 1 || output_index == UINT_MAX) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_mgather_common(env, output_tile, sources[0], 0,
                                         false, addr_reg, size_code);
                break;
            case LINX_TMA_MGATHER_MASK:
                if (source_count < 2 || output_index == UINT_MAX) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_mgather_common(env, output_tile, sources[0],
                                         sources[1], true, addr_reg, size_code);
                break;
            case LINX_TMA_MSCATTER:
                if (source_count < 2 || output_index != UINT_MAX) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_mscatter_common(env, sources[0], sources[1], 0,
                                          false, addr_reg, size_code);
                break;
            case LINX_TMA_MSCATTER_MASK:
                if (source_count < 3 || output_index != UINT_MAX) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_mscatter_common(env, sources[0], sources[1],
                                          sources[2], true, addr_reg, size_code);
                break;
            case LINX_TMA_MGATHER_CAS:
                if (source_count < 3 || output_index == UINT_MAX) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_mgather_cas(env, output_tile, sources[0], sources[1],
                                      sources[2], addr_reg, size_code);
                break;
            default:
                g_assert_not_reached();
            }

            for (unsigned i = 0; i < env->tile_iot_count; i++) {
                LinxTileIOTDesc d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                linx_tile_consume_bound_sources(env, live, i, &d,
                                                order, count_by_hand,
                                                &carrier_valid, &carrier);
            }
            if (output_index != UINT_MAX) {
                linx_tile_invalidate_acc_sources_on_output(
                    output_tile, &acc_sources_valid, acc_src0, acc_src1);
                if (!linx_tile_complete_bound_output(
                        env, live, reserved, order, count_by_hand, output_index)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                }
            }
            break;
        }
        case LINX_TMA_TCVT_COMPAT: {
            const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
            for (unsigned i = 0; i < count; i++) {
                LinxTileIOTDesc d;
                if (env->tile_iot_count) {
                    d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                } else {
                    d.src0 = env->tile_iot_src0 & 0x3f;
                    d.src1 = env->tile_iot_src1 & 0x3f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.last = env->tile_iot_grp & 0x1;
                    d.flags = env->tile_iot_flags & 0xf;
                    d.reg = env->tile_iot_reg & 0x1f;
                    d.size = env->tile_iot_size & 0x1f;
                    d.has_size = env->tile_iot_size != 0;
                }

                const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
                const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;

                unsigned src_tile = 0;
                if (src0_present) {
                    if (!linx_tile_get_bound_source(env, i, 0, &src_tile)) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                } else if (src1_present) {
                    if (!linx_tile_get_bound_source(env, i, 1, &src_tile)) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                } else {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }

                unsigned dst_tile = 0;
                if (!linx_tile_get_bound_output(env, i, &dst_tile)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }

                const unsigned size_code = d.has_size ? (d.size & 0x1f)
                                                      : (env->tile_iot_size & 0x1f);
                const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
                if (bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES || (bytes64 & 3u) != 0) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const unsigned words = (unsigned)(bytes64 / 4u);
                if (src_tile >= 32 || dst_tile >= 32 ||
                    env->tile_reg_bytes[src_tile] < bytes64) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_invalidate_acc_sources_on_output(
                    dst_tile, &acc_sources_valid, acc_src0, acc_src1);
                for (unsigned w = 0; w < words && w < LINX_TILE_MAX_WORDS; w++) {
                    env->tile_reg[dst_tile][w] = env->tile_reg[src_tile][w];
                }
                for (unsigned w = words; w < LINX_TILE_MAX_WORDS; w++) {
                    env->tile_reg[dst_tile][w] = 0;
                }
                env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
                linx_tile_set_elem_bytes(env, dst_tile,
                                         env->tile_reg_elem_bytes[src_tile]);
                linx_tile_set_dtype(env, dst_tile,
                                    env->tile_reg_dtype[src_tile]);
                linx_tile_copy_shape(env, dst_tile, src_tile);
                linx_tile_consume_bound_sources(env, live, i, &d,
                                                order, count_by_hand,
                                                &carrier_valid, &carrier);
                if (!linx_tile_complete_bound_output(
                        env, live, reserved, order, count_by_hand, i)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                }
            }
            break;
        }
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    case LINX_BLOCK_CUBE:
        switch (env->tile_func & 0x1f) {
        case LINX_CUBE_MAMULB:
        case LINX_CUBE_TGEMV: {
            unsigned sources[LINX_TILE_MAX_IOT * 2];
            unsigned size_code = 0;
            if (!linx_tile_collect_cube_sources(env, 2u, sources,
                                                &size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            linx_tile_mamulb(env, sources[0], sources[1], size_code);
            for (unsigned i = 0; i < env->tile_iot_count; i++) {
                const LinxTileIOTDesc d =
                    linx_tile_decode_iot(env->tile_iot_desc[i]);
                linx_tile_consume_bound_sources(env, live, i, &d,
                                                order, count_by_hand,
                                                &carrier_valid, &carrier);
            }
            carrier_valid = 0;
            acc_sources_valid = 1;
            acc_src0 = sources[0];
            acc_src1 = sources[1];
            break;
        }
        case LINX_CUBE_MAMULB_BIAS: {
            unsigned sources[LINX_TILE_MAX_IOT * 2];
            unsigned size_code = 0;
            if (!linx_tile_collect_cube_sources(env, 3u, sources,
                                                &size_code) ||
                !linx_tile_mamulb_bias(env, sources[0], sources[1],
                                       sources[2], size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            for (unsigned i = 0; i < env->tile_iot_count; i++) {
                const LinxTileIOTDesc d =
                    linx_tile_decode_iot(env->tile_iot_desc[i]);
                linx_tile_consume_bound_sources(env, live, i, &d,
                                                order, count_by_hand,
                                                &carrier_valid, &carrier);
            }
            carrier_valid = 0;
            acc_sources_valid = 0;
            break;
        }
        case LINX_CUBE_MAMULB_ACC:
        case LINX_CUBE_TGEMV_ACC: {
            LinxTileIOTDesc d;
            if (env->tile_iot_count) {
                d = linx_tile_decode_iot(env->tile_iot_desc[0]);
            } else {
                d.src0 = env->tile_iot_src0 & 0x3f;
                d.src1 = env->tile_iot_src1 & 0x3f;
                d.flags = env->tile_iot_flags & 0xf;
                d.last = env->tile_iot_grp & 0x1;
                d.dst = env->tile_iot_dst & 0x7;
                d.reg = env->tile_iot_reg & 0x1f;
                d.size = env->tile_iot_size & 0x1f;
                d.has_size = env->tile_iot_size != 0;
            }
            if ((d.flags & (LINX_IOT_S0V | LINX_IOT_S1V)) != 0) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            unsigned bound_src0 = 0;
            unsigned bound_src1 = 0;
            if (!linx_tile_get_bound_source(env, 0, 0, &bound_src0) ||
                !linx_tile_get_bound_source(env, 0, 1, &bound_src1) ||
                !acc_sources_valid || bound_src0 != acc_src0 ||
                bound_src1 != acc_src1 ||
                acc_src0 >= LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH ||
                acc_src1 >= LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH ||
                env->tile_reg_bytes[acc_src0] == 0 ||
                env->tile_reg_bytes[acc_src1] == 0) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            const unsigned src0 = acc_src0;
            const unsigned src1 = acc_src1;
            const unsigned size_code = d.has_size ? (d.size & 0x1f)
                                                  : (env->tile_iot_size & 0x1f);
            if (!linx_tile_size_code_valid(size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            linx_tile_mamulb_acc(env, src0, src1, size_code);
            linx_tile_consume_bound_sources(env, live, 0, &d,
                                            order, count_by_hand,
                                            &carrier_valid, &carrier);
            if (carrier_valid) {
                linx_tile_release_source(live, order, count_by_hand,
                                         carrier, false,
                                         &carrier_valid, &carrier);
            }
            acc_sources_valid = 0;
            break;
        }
        case LINX_CUBE_ACCCVT: {
            LinxTileIOTDesc d;
            if (env->tile_iot_count) {
                d = linx_tile_decode_iot(env->tile_iot_desc[0]);
            } else {
                d.src0 = env->tile_iot_src0 & 0x3f;
                d.src1 = env->tile_iot_src1 & 0x3f;
                d.flags = env->tile_iot_flags & 0xf;
                d.dst = env->tile_iot_dst & 0x7;
                d.last = env->tile_iot_grp & 0x1;
                d.reg = env->tile_iot_reg & 0x1f;
                d.size = env->tile_iot_size & 0x1f;
                d.has_size = env->tile_iot_size != 0;
            }

            unsigned dst_tile = 0;
            if (!linx_tile_get_bound_output(env, 0, &dst_tile)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            linx_tile_invalidate_acc_sources_on_output(
                dst_tile, &acc_sources_valid, acc_src0, acc_src1);
            const unsigned size_code = d.has_size ? (d.size & 0x1f)
                                                  : (env->tile_iot_size & 0x1f);
            if (!linx_tile_size_code_valid(size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            linx_tile_acccvt(env, dst_tile, size_code);
            if (carrier_valid) {
                linx_tile_release_source(live, order, count_by_hand,
                                         carrier, false,
                                         &carrier_valid, &carrier);
            }
            if (!linx_tile_complete_bound_output(
                    env, live, reserved, order, count_by_hand, 0)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            }
            carrier_valid = 1;
            carrier = dst_tile;
            break;
        }
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    default:
        /* Non-tile blocks: nothing to do. */
        break;
    }

    memcpy(env->tile_hand_live, live, sizeof(live));
    memcpy(env->tile_hand_reserved, reserved, sizeof(reserved));
    memcpy(env->tile_hand_order, order, sizeof(order));
    memcpy(env->tile_hand_count, count_by_hand, sizeof(count_by_hand));
    env->tile_acc_carrier_valid = carrier_valid;
    env->tile_acc_carrier = carrier;
    env->tile_acc_sources_valid = acc_sources_valid;
    env->tile_acc_src0 = acc_src0;
    env->tile_acc_src1 = acc_src1;

    /* Consume the per-block descriptor. */
    env->tile_iot_valid = 0;
    env->tile_iot_size = 0;
    env->tile_iot_grp = 0;
    env->tile_arg_format = 0;
    env->tile_attr_pad = 0;
    env->tile_attr_dtype = 0;
    env->tile_ior_count = 0;
    linx_tile_unpin_bindings(env);
    env->tile_iot_count = 0;
    memset(env->tile_iot_desc, 0, sizeof(env->tile_iot_desc));
    memset(env->tile_iot_src_valid, 0, sizeof(env->tile_iot_src_valid));
    memset(env->tile_iot_src_phys, 0, sizeof(env->tile_iot_src_phys));
    memset(env->tile_iot_output_valid, 0,
           sizeof(env->tile_iot_output_valid));
    memset(env->tile_iot_output_phys, 0,
           sizeof(env->tile_iot_output_phys));
}

/* ------------------------------------------------------------------------- */
/* v0.3 SIMT/vector helpers (bring-up subset)                                */
/* ------------------------------------------------------------------------- */

enum {
    LINX_VEC_REGCLASS_RI = 1,
    LINX_VEC_REGCLASS_P = 2,
    LINX_VEC_REGCLASS_LC = 3,
    LINX_VEC_REGCLASS_VT = 4,
    LINX_VEC_REGCLASS_VU = 5,
    LINX_VEC_REGCLASS_VM = 6,
    LINX_VEC_REGCLASS_VN = 7,
    LINX_VEC_REGCLASS_TBASE = 8,
};

enum {
    LINX_VEC_P_REG_INDEX = 28,
};

static inline unsigned linx_vec_reg_class(uint32_t code)
{
    return (unsigned)((code >> 5) & 0x1fu);
}

static inline unsigned linx_vec_reg_index(uint32_t code)
{
    return (unsigned)(code & 0x1fu);
}

static inline uint32_t linx_vec_reg_code(unsigned cls, unsigned idx)
{
    return ((uint32_t)(cls & 0x1fu) << 5) | (uint32_t)(idx & 0x1fu);
}

static inline bool linx_vec_is_canonical_source_code(uint32_t code)
{
    const unsigned cls = linx_vec_reg_class(code);
    const unsigned idx = linx_vec_reg_index(code);

    if (code < 32u ||
        code == linx_vec_reg_code(LINX_VEC_REGCLASS_P,
                                  LINX_VEC_P_REG_INDEX)) {
        return true;
    }

    switch (cls) {
    case LINX_VEC_REGCLASS_RI:
        return true;
    case LINX_VEC_REGCLASS_LC:
        return idx < 3u;
    case LINX_VEC_REGCLASS_VT:
    case LINX_VEC_REGCLASS_VU:
    case LINX_VEC_REGCLASS_VM:
    case LINX_VEC_REGCLASS_VN:
        return true;
    case LINX_VEC_REGCLASS_TBASE:
        return idx < 6u;
    default:
        return false;
    }
}

static inline bool linx_vec_is_canonical_dst_code(uint32_t code)
{
    const unsigned cls = linx_vec_reg_class(code);
    if (code == linx_vec_reg_code(LINX_VEC_REGCLASS_P,
                                  LINX_VEC_P_REG_INDEX)) {
        return true;
    }
    return cls == LINX_VEC_REGCLASS_VT ||
           cls == LINX_VEC_REGCLASS_VU ||
           cls == LINX_VEC_REGCLASS_VM ||
           cls == LINX_VEC_REGCLASS_VN;
}

static inline uint32_t linx_vec_normalize_queue_source(uint32_t raw)
{
    const unsigned low = raw & 0x1fu;
    const unsigned bank = (low >> 3) & 0x3u;
    const unsigned idx = (low & 0x7u) + 1u;

    return linx_vec_reg_code(LINX_VEC_REGCLASS_VT + bank, idx);
}

/*
 * Current bring-up toolchain emits typed V.* operand codes that fold the
 * value width into the register field itself. The vector helpers operate on the
 * architectural namespace instead, so strip the transient type lane here.
 */
static uint32_t linx_vec_normalize_source_code(uint32_t raw)
{
    if (linx_vec_is_canonical_source_code(raw)) {
        return raw;
    }

    if (raw < 32u || raw == linx_vec_reg_code(LINX_VEC_REGCLASS_P,
                                              LINX_VEC_P_REG_INDEX)) {
        return raw;
    }

    if (raw <= 0x48u && (raw & 0x3u) == 0u) {
        return linx_vec_reg_code(LINX_VEC_REGCLASS_LC, (raw - 0x40u) >> 2);
    }

    if (raw >= 0x50u && raw <= 0x59u) {
        const unsigned low = raw & 0x0fu;
        if (low <= 3u) {
            return linx_vec_reg_code(LINX_VEC_REGCLASS_TBASE, low);
        }
        if (low == 8u || low == 9u) {
            return linx_vec_reg_code(LINX_VEC_REGCLASS_TBASE, low - 4u);
        }
    }

    if ((raw >= 0x80u && raw < 0xa0u) ||
        (raw >= 0x200u && raw < 0x220u) ||
        (raw >= 0x280u && raw < 0x2a0u)) {
        return linx_vec_normalize_queue_source(raw);
    }

    if ((raw >= 0xa0u && raw < 0xc0u) ||
        (raw >= 0x220u && raw < 0x240u) ||
        (raw >= 0x2a0u && raw < 0x2c0u)) {
        return linx_vec_reg_code(LINX_VEC_REGCLASS_RI, raw & 0x1fu);
    }

    if ((raw >= 0xc0u && raw <= 0xc8u) ||
        (raw >= 0x140u && raw <= 0x148u) ||
        (raw >= 0x2c0u && raw <= 0x2c8u)) {
        return linx_vec_reg_code(LINX_VEC_REGCLASS_LC,
                                 (raw & 0x1fu) >> 2);
    }

    if (raw == 0xdfu || raw == 0x25fu || raw == 0x2dfu) {
        return 0u;
    }

    return raw;
}

static bool linx_vec_has_bound_tile_inputs(const CPULinxState *env)
{
    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        if (env->tile_iot_src_valid[i] != 0) {
            return true;
        }
    }
    return false;
}

static uint32_t linx_vec_normalize_dst_code(const CPULinxState *env,
                                            uint32_t raw)
{
    if (linx_vec_is_canonical_dst_code(raw)) {
        const unsigned cls = linx_vec_reg_class(raw);

        if (cls == LINX_VEC_REGCLASS_P ||
            !linx_vec_has_bound_tile_inputs(env)) {
            return raw;
        }

        /*
         * Tile-backed VPAR/TEPL bodies publish their typed result as the new
         * queue head even when an older compiler encoded a physical vector
         * slot.  Ordinary MSEQ/MPAR bodies retain their existing fixed-slot
         * compatibility until compiler vector-queue lowering is converted as
         * one cross-layer change.
         */
        return linx_vec_reg_code(cls, 0u);
    }

    if (raw == linx_vec_reg_code(LINX_VEC_REGCLASS_P, LINX_VEC_P_REG_INDEX)) {
        return raw;
    }
    if (raw <= 3u) {
        return linx_vec_reg_code(LINX_VEC_REGCLASS_VT + raw, 0u);
    }
    if (raw >= 0x80u && raw <= 0x83u) {
        return linx_vec_reg_code(LINX_VEC_REGCLASS_VT + (raw - 0x80u), 0u);
    }
    return raw;
}

static uint32_t linx_vec_normalize_reduce_dst(uint32_t raw)
{
    if (raw < 32u) {
        return raw;
    }

    if ((raw & 0x3cu) == 0x3cu) {
        return 28u + (raw & 0x3u);
    }
    return raw;
}

static bool linx_vec_resolve_tile_base(const CPULinxState *env, unsigned base_idx,
                                       unsigned *tile_out)
{
    /*
     * Strict v0.3 bring-up mapping:
     * - TA..TD: up to four header-frozen input tiles
     * - TO/TS: the first two independently reserved output tiles
     *
     * TBASE indices 6+ are not part of the currently implemented assembler
     * ABI and remain illegal until compiler/parser encodings are defined.
     *
     * Inputs/outputs are derived from the active header's B.IOT descriptors.
     */
    unsigned inputs[4];
    unsigned outputs[2];
    unsigned input_count = 0;
    unsigned output_count = 0;

    const unsigned count = env->tile_iot_count ? env->tile_iot_count
                                               : (env->tile_iot_valid ? 1u : 0u);
    for (unsigned i = 0; i < count; i++) {
        LinxTileIOTDesc d;
        if (env->tile_iot_count) {
            d = linx_tile_decode_iot(env->tile_iot_desc[i]);
        } else {
            d.src0 = env->tile_iot_src0 & 0x3f;
            d.src1 = env->tile_iot_src1 & 0x3f;
            d.dst = env->tile_iot_dst & 0x7;
            d.last = env->tile_iot_grp & 0x1;
            d.flags = env->tile_iot_flags & 0xf;
            d.reg = env->tile_iot_reg & 0x1f;
            d.size = env->tile_iot_size & 0x1f;
            d.has_size = env->tile_iot_size != 0;
        }

        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local desc[%u]: src0=%u src1=%u dst=%u grp=%u"
                          " flags=0x%x reg=%u size=%u has_size=%u body_tpc=0x%" PRIx64 "\n",
                          i, d.src0, d.src1, d.dst, d.last, d.flags, d.reg,
                          d.size, d.has_size ? 1u : 0u, env->body_tpc);
        }

        if ((env->tile_iot_src_valid[i] & 1u) != 0 && input_count < 4) {
            inputs[input_count++] = env->tile_iot_src_phys[i][0];
        }
        if ((env->tile_iot_src_valid[i] & 2u) != 0 && input_count < 4) {
            inputs[input_count++] = env->tile_iot_src_phys[i][1];
        }

        if (env->tile_iot_output_valid[i] && output_count < 2) {
            outputs[output_count++] = env->tile_iot_output_phys[i];
        }
    }

    if (base_idx < 4) {
        if (base_idx < input_count) {
            *tile_out = inputs[base_idx] & 0x1f;
            return true;
        }
        return false;
    }
    if (base_idx >= 4 && base_idx < 6) { /* TO, TS/TO1 */
        const unsigned output = base_idx - 4u;
        if (output < output_count) {
            *tile_out = outputs[output] & 0x1f;
            return true;
        }
        return false;
    }
    return false;
}

static void linx_tile_commit_vector_bindings(CPULinxState *env)
{
    uint8_t live[LINX_TILE_HAND_COUNT];
    uint8_t reserved[LINX_TILE_HAND_COUNT];
    uint8_t order[LINX_TILE_HAND_COUNT][LINX_TILE_HAND_DEPTH];
    uint8_t count_by_hand[LINX_TILE_HAND_COUNT];

    memcpy(live, env->tile_hand_live, sizeof(live));
    memcpy(reserved, env->tile_hand_reserved, sizeof(reserved));
    memcpy(order, env->tile_hand_order, sizeof(order));
    memcpy(count_by_hand, env->tile_hand_count, sizeof(count_by_hand));

    /* Consume the header-frozen physical inputs before publishing outputs. */
    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        const LinxTileIOTDesc d = linx_tile_decode_iot(env->tile_iot_desc[i]);

        if ((env->tile_iot_src_valid[i] & 1u) != 0 &&
            (d.flags & LINX_IOT_S0R) == 0) {
            linx_tile_release_source(live, order, count_by_hand,
                                     env->tile_iot_src_phys[i][0], false,
                                     &env->tile_acc_carrier_valid,
                                     &env->tile_acc_carrier);
        }
        if ((env->tile_iot_src_valid[i] & 2u) != 0 &&
            (d.flags & LINX_IOT_S1R) == 0) {
            linx_tile_release_source(live, order, count_by_hand,
                                     env->tile_iot_src_phys[i][1], false,
                                     &env->tile_acc_carrier_valid,
                                     &env->tile_acc_carrier);
        }
    }

    /* Descriptor order is architectural: the last persistent output is #1. */
    for (unsigned i = 0; i < env->tile_iot_count; i++) {
        if (!env->tile_iot_output_valid[i]) {
            continue;
        }
        const unsigned tile = env->tile_iot_output_phys[i];
        const unsigned hand = tile / LINX_TILE_HAND_DEPTH;
        const unsigned depth = tile % LINX_TILE_HAND_DEPTH;

        reserved[hand] &= ~(1u << depth);

        /*
         * Source-less MSEQ/MPAR outputs back block-local LTAR scratch such as
         * TS.  They are addressable while the body runs, but are not tile
         * values that survive the block.  Publishing them would fill an
         * architectural hand after a few loop iterations and make the next
         * header retry forever.
         */
        if (env->tile_iot_src_valid[i] == 0) {
            live[hand] &= ~(1u << depth);
            env->tile_reg_bytes[tile] = 0;
            env->tile_reg_elem_bytes[tile] = 0;
            env->tile_reg_dtype[tile] = 0;
            env->tile_reg_valid_cols[tile] = 0;
            env->tile_reg_valid_rows[tile] = 0;
            env->tile_reg_cols[tile] = 0;
            env->tile_reg_rows[tile] = 0;
            continue;
        }

        linx_tile_publish_output(live, tile);
        if (!linx_tile_publish_order_state(order, count_by_hand, tile)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }

    memcpy(env->tile_hand_live, live, sizeof(live));
    memcpy(env->tile_hand_reserved, reserved, sizeof(reserved));
    memcpy(env->tile_hand_order, order, sizeof(order));
    memcpy(env->tile_hand_count, count_by_hand, sizeof(count_by_hand));
    linx_tile_unpin_bindings(env);
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
    memset(env->tile_iot_src_valid, 0, sizeof(env->tile_iot_src_valid));
    memset(env->tile_iot_src_phys, 0, sizeof(env->tile_iot_src_phys));
    memset(env->tile_iot_output_valid, 0,
           sizeof(env->tile_iot_output_valid));
    memset(env->tile_iot_output_phys, 0,
           sizeof(env->tile_iot_output_phys));
}

static uint64_t linx_vec_read_reg(CPULinxState *env, uint32_t code)
{
    code = linx_vec_normalize_source_code(code);

    if (env->pc == 0x1b536) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: vec read pc=0x%" PRIx64
                      " code=%u class=%u idx=%u ri_count=%u\n",
                      env->pc, code, linx_vec_reg_class(code),
                      linx_vec_reg_index(code), env->vec_ri_count);
    }
    /*
     * v0.3 bring-up: vector bodies may mix scalar and vector operands.
     * Scalar encodings keep the base scalar namespace:
     *   0      -> zero
     *   1..23  -> GPR
     *   24..27 -> TQ
     *   28..31 -> UQ
     */
    if (code < 32u) {
        if (code < LINX_GPR_COUNT) {
            return env->gpr[code];
        }
        if (code < 28u) {
            return env->tq[code - 24u];
        }
        return env->uq[code - 28u];
    }

    const unsigned cls = linx_vec_reg_class(code);
    const unsigned idx = linx_vec_reg_index(code);

    switch (cls) {
    case LINX_VEC_REGCLASS_RI: {
        if (idx < env->vec_ri_count) {
            return env->vec_ri_value[idx];
        }
        unsigned gpr = 0;
        if (!linx_tile_resolve_ior(env, idx, &gpr)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        return env->gpr[gpr];
    }
    case LINX_VEC_REGCLASS_P:
        if (idx == LINX_VEC_P_REG_INDEX) {
            return env->vec_p != 0 ? 1u : 0u;
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    case LINX_VEC_REGCLASS_LC:
        if (idx < 3) {
            return env->lc[idx];
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    case LINX_VEC_REGCLASS_VT:
        if (idx == 0) {
            return env->vtq[0];
        }
        if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
            return env->vtq[idx - 1u];
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    case LINX_VEC_REGCLASS_VU:
        if (idx == 0) {
            return env->vuq[0];
        }
        if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
            return env->vuq[idx - 1u];
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    case LINX_VEC_REGCLASS_VM:
        if (idx == 0) {
            return env->vmq[0];
        }
        if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
            return env->vmq[idx - 1u];
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    case LINX_VEC_REGCLASS_VN:
        if (idx == 0) {
            return env->vnq[0];
        }
        if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
            return env->vnq[idx - 1u];
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    case LINX_VEC_REGCLASS_TBASE: {
        unsigned tile = 0;
        if (!linx_vec_resolve_tile_base(env, idx, &tile)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return 0;
        }
        return (uint64_t)(tile & 0x1fu);
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }
}

static void linx_vec_write_vt(CPULinxState *env, unsigned idx, uint64_t value)
{
    if (idx == 0) {
        /* Push: VT#1 becomes the most recently produced value. */
        for (unsigned i = LINX_VEC_QUEUE_DEPTH - 1u; i > 0; i--) {
            env->vtq[i] = env->vtq[i - 1u];
        }
        env->vtq[0] = value;
        return;
    }
    if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
        env->vtq[idx - 1u] = value;
    }
}

static void linx_vec_write_vu(CPULinxState *env, unsigned idx, uint64_t value)
{
    if (idx == 0) {
        for (unsigned i = LINX_VEC_QUEUE_DEPTH - 1u; i > 0; i--) {
            env->vuq[i] = env->vuq[i - 1u];
        }
        env->vuq[0] = value;
        return;
    }
    if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
        env->vuq[idx - 1u] = value;
    }
}

static void linx_vec_write_vm(CPULinxState *env, unsigned idx, uint64_t value)
{
    if (idx == 0) {
        for (unsigned i = LINX_VEC_QUEUE_DEPTH - 1u; i > 0; i--) {
            env->vmq[i] = env->vmq[i - 1u];
        }
        env->vmq[0] = value;
        return;
    }
    if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
        env->vmq[idx - 1u] = value;
    }
}

static void linx_vec_write_vn(CPULinxState *env, unsigned idx, uint64_t value)
{
    if (idx == 0) {
        for (unsigned i = LINX_VEC_QUEUE_DEPTH - 1u; i > 0; i--) {
            env->vnq[i] = env->vnq[i - 1u];
        }
        env->vnq[0] = value;
        return;
    }
    if (idx - 1u < LINX_VEC_QUEUE_DEPTH) {
        env->vnq[idx - 1u] = value;
    }
}

void HELPER(linx_vec_body_begin)(CPULinxState *env)
{
    /* v0.3 bring-up: initialize loop counters and clear transient VT state. */
    env->lc[0] = 0;
    env->lc[1] = 0;
    env->lc[2] = 0;
    env->vec_p = 0;
    env->body_end = linx_lookup_body_end(env, env->body_tpc);
    linx_vec_capture_ri_values(env);
    for (unsigned i = 0; i < LINX_VEC_QUEUE_DEPTH; i++) {
        env->vtq[i] = 0;
        env->vuq[i] = 0;
        env->vmq[i] = 0;
        env->vnq[i] = 0;
    }
    if (linx_debug_body_replay_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body replay begin: tpc=0x%" PRIx64
                      " end=0x%" PRIx64
                      " lb=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]\n",
                      env->body_tpc, env->body_end,
                      env->lb[0], env->lb[1], env->lb[2]);
        for (unsigned i = 0; i < env->tile_ior_count; i++) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx body replay ior: tpc=0x%" PRIx64
                          " ior[%u]=0x%016" PRIx64 "\n",
                          env->body_tpc, i, env->tile_ior_desc[i]);
        }
        for (unsigned i = 0; i < env->vec_ri_count; i++) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx body replay ri: tpc=0x%" PRIx64
                          " ri%u=0x%" PRIx64 "\n",
                          env->body_tpc, i, env->vec_ri_value[i]);
        }
    }
}

uint32_t HELPER(linx_vec_body_next)(CPULinxState *env)
{
    const uint64_t lb0 = env->lb[0];
    const uint64_t lb1 = env->lb[1] ? env->lb[1] : 1;
    const uint64_t lb2 = env->lb[2] ? env->lb[2] : 1;
    const uint64_t prev_lc0 = env->lc[0];
    const uint64_t prev_lc1 = env->lc[1];
    const uint64_t prev_lc2 = env->lc[2];
    uint32_t cont;

    if (lb0 == 0) {
        linx_tile_commit_vector_bindings(env);
        return 0;
    }

    env->lc[0]++;
    if (env->lc[0] < lb0) {
        cont = 1;
        goto out;
    }
    env->lc[0] = 0;

    env->lc[1]++;
    if (env->lc[1] < lb1) {
        cont = 1;
        goto out;
    }
    env->lc[1] = 0;

    env->lc[2]++;
    if (env->lc[2] < lb2) {
        cont = 1;
        goto out;
    }
    env->lc[2] = 0;
    cont = 0;
    linx_tile_commit_vector_bindings(env);

out:
    if (linx_debug_body_replay_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body replay next: tpc=0x%" PRIx64
                      " prev_lc=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]"
                      " next_lc=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]"
                      " lb=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]"
                      " cont=%u\n",
                      env->body_tpc,
                      prev_lc0, prev_lc1, prev_lc2,
                      env->lc[0], env->lc[1], env->lc[2],
                      lb0, lb1, lb2,
                      cont);
    }
    return cont;
}

void HELPER(linx_debug_body_pred_branch)(CPULinxState *env, uint64_t current_pc,
                                         uint64_t target, uint64_t fallthrough,
                                         uint32_t take_on_zero)
{
    bool taken;

    if (!linx_debug_body_replay_enabled_p()) {
        return;
    }

    taken = take_on_zero ? (env->vec_p == 0) : (env->vec_p != 0);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx body replay branch: pc=0x%" PRIx64
                  " vec_p=0x%" PRIx64
                  " lc=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "]"
                  " mode=%s taken=%u target=0x%" PRIx64
                  " fallthrough=0x%" PRIx64 "\n",
                  current_pc, env->vec_p,
                  env->lc[0], env->lc[1], env->lc[2],
                  take_on_zero ? "b.z" : "b.nz",
                  taken ? 1u : 0u,
                  target, fallthrough);
}

static void linx_vec_write_dst(CPULinxState *env, uint32_t dst, uint64_t value)
{
    dst = linx_vec_normalize_dst_code(env, dst);

    const unsigned cls = linx_vec_reg_class(dst);
    const unsigned didx = linx_vec_reg_index(dst);

    switch (cls) {
    case LINX_VEC_REGCLASS_P:
        if (didx == LINX_VEC_P_REG_INDEX) {
            env->vec_p = value != 0 ? 1u : 0u;
            return;
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    case LINX_VEC_REGCLASS_VT:
        linx_vec_write_vt(env, didx, value);
        return;
    case LINX_VEC_REGCLASS_VU:
        linx_vec_write_vu(env, didx, value);
        return;
    case LINX_VEC_REGCLASS_VM:
        linx_vec_write_vm(env, didx, value);
        return;
    case LINX_VEC_REGCLASS_VN:
        linx_vec_write_vn(env, didx, value);
        return;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
}

static uint64_t linx_vec_read_reduce_dst(CPULinxState *env, uint32_t dst)
{
    dst = linx_vec_normalize_reduce_dst(dst);

    if (dst == 0) {
        return 0;
    }
    if (dst < LINX_GPR_COUNT) {
        return env->gpr[dst];
    }
    if (dst >= 24 && dst < 28) {
        return env->tq[dst - 24];
    }
    if (dst >= 28 && dst < 32) {
        return env->uq[dst - 28];
    }
    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
    return 0;
}

static void linx_vec_write_reduce_dst(CPULinxState *env, uint32_t dst, uint64_t value)
{
    dst = linx_vec_normalize_reduce_dst(dst);

    if (dst == 0) {
        return;
    }
    if (dst < LINX_GPR_COUNT) {
        env->gpr[dst] = value;
        return;
    }
    if (dst >= 24 && dst < 28) {
        env->tq[dst - 24] = value;
        return;
    }
    if (dst >= 28 && dst < 32) {
        env->uq[dst - 28] = value;
        return;
    }
    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
}

static uint64_t linx_vec_rhs_addsub(uint64_t rhs, uint32_t srctype, uint32_t shamt)
{
    switch (srctype & 0x3u) {
    case 0:
        rhs = (uint64_t)(int64_t)(int32_t)rhs;
        break;
    case 1:
        rhs = (uint64_t)(uint32_t)rhs;
        break;
    case 2:
        rhs = (uint64_t)(-(int64_t)rhs);
        break;
    default:
        break;
    }
    if (shamt) {
        rhs <<= (shamt & 0x3fu);
    }
    return rhs;
}

static uint64_t linx_vec_rhs_logic(uint64_t rhs, uint32_t srctype, uint32_t shamt)
{
    if ((srctype & 0x3u) == 2u) {
        rhs = ~rhs;
    }
    if (shamt) {
        rhs <<= (shamt & 0x3fu);
    }
    return rhs;
}

static inline uint32_t linx_vec_mask_low_n32(uint32_t n)
{
    return n >= 32u ? UINT32_MAX : ((1u << n) - 1u);
}

static inline uint32_t linx_vec_rol32(uint32_t x, uint32_t sh)
{
    sh &= 31u;
    return sh ? ((x << sh) | (x >> (32u - sh))) : x;
}

static inline uint32_t linx_vec_ror32(uint32_t x, uint32_t sh)
{
    sh &= 31u;
    return sh ? ((x >> sh) | (x << (32u - sh))) : x;
}

static uint32_t linx_vec_bitfield_wrap32(uint32_t x, uint32_t lsb, uint32_t width)
{
    return linx_vec_ror32(x, lsb) & linx_vec_mask_low_n32(width);
}

static uint32_t linx_vec_sign_extend32(uint32_t x, uint32_t width)
{
    if (width >= 32u) {
        return x;
    }
    if (width == 0u) {
        return 0u;
    }
    return (uint32_t)(((int32_t)(x << (32u - width))) >> (32u - width));
}

static uint32_t linx_vec_normalize_width(uint32_t nminus1)
{
    uint32_t width = (nminus1 & 0x3fu) + 1u;
    return width > 32u ? 32u : width;
}

static uint64_t linx_vec_cmp_bool(bool pred)
{
    return pred ? 1u : 0u;
}

static inline uint32_t linx_vec_mem_index_shift(uint32_t src_code,
                                                uint32_t shamt,
                                                uint32_t zero_base_shift,
                                                bool add_width_bias)
{
    src_code = linx_vec_normalize_source_code(src_code);
    shamt &= 0x3fu;

    /*
     * Current bring-up streams encode raw scalar `.sd` address operands
     * through the typed source lane, which reaches helpers as shamt=30.
     * Treat that spelling as the zero-shift base form for the element width
     * carried by the opcode so `[riX.sd, lc0<<2, riY.sd]` lands on the next
     * word rather than shifting by 30.
     */
    if (shamt == 30u &&
        (src_code == 0u ||
         linx_vec_reg_class(src_code) == LINX_VEC_REGCLASS_RI)) {
        return zero_base_shift;
    }

    return add_width_bias ? (zero_base_shift + shamt) : shamt;
}

static uint64_t linx_fp_unop_sqrt(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);
    switch (srctype & 0x3u) {
    case 0:
        a = (uint64_t)float64_sqrt((float64)a, &env->fp_status);
        break;
    case 1:
        a = (uint64_t)(uint32_t)float32_sqrt((float32)(uint32_t)a, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }
    linx_fp_sync_to_fcsr(env);
    return a;
}

static uint64_t linx_fp_unop_recip(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);
    switch (srctype & 0x3u) {
    case 0: {
        union {
            uint64_t u;
            double f;
        } cvt = { .u = a };
        cvt.f = 1.0 / cvt.f;
        a = cvt.u;
        break;
    }
    case 1: {
        union {
            uint32_t u;
            float f;
        } cvt = { .u = (uint32_t)a };
        cvt.f = 1.0f / cvt.f;
        a = (uint64_t)cvt.u;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }
    linx_fp_sync_to_fcsr(env);
    return a;
}

static uint64_t linx_fp_unop_exp(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    linx_fp_sync_from_fcsr(env);
    switch (srctype & 0x3u) {
    case 0: {
        union {
            uint64_t u;
            double d;
        } cvt = { .u = a };
        cvt.d = exp(cvt.d);
        a = cvt.u;
        break;
    }
    case 1: {
        union {
            uint32_t u;
            float f;
        } cvt = { .u = (uint32_t)a };
        cvt.f = expf(cvt.f);
        a = (uint64_t)cvt.u;
        break;
    }
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }
    linx_fp_sync_to_fcsr(env);
    return a;
}

static uint64_t linx_fp_unop_class(CPULinxState *env, uint64_t a, uint32_t srctype)
{
    uint64_t res = 0;

    switch (srctype & 0x3u) {
    case 1: {
        const uint32_t bits = (uint32_t)a;
        const bool sign = (bits >> 31) != 0;
        const uint32_t exp = (bits >> 23) & 0xffu;
        const uint32_t frac = bits & 0x7fffffu;

        if (exp == 0xffu) {
            if (frac == 0) {
                res = sign ? (1u << 0) : (1u << 7);
            } else {
                res = (frac & (1u << 22)) ? (1u << 9) : (1u << 8);
            }
        } else if (exp == 0) {
            if (frac == 0) {
                res = sign ? (1u << 3) : (1u << 4);
            } else {
                res = sign ? (1u << 2) : (1u << 5);
            }
        } else {
            res = sign ? (1u << 1) : (1u << 6);
        }
        break;
    }
    default:
        /* Keep current bring-up vector FP classification scoped to float32. */
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    return res;
}

static uint64_t linx_fp_binop_max(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    if (linx_fp_cmp_lt(env, a, b, srctype)) {
        return b;
    }
    return a;
}

static uint64_t linx_fp_binop_min(CPULinxState *env, uint64_t a, uint64_t b, uint32_t srctype)
{
    if (linx_fp_cmp_lt(env, b, a, srctype)) {
        return b;
    }
    return a;
}

static uint64_t linx_fp_ternop_muladd(CPULinxState *env, uint64_t a, uint64_t b,
                                      uint64_t c, uint32_t srctype, int flags)
{
    linx_fp_sync_from_fcsr(env);

    switch (srctype & 0x3u) {
    case 0:
        a = (uint64_t)float64_muladd((float64)a, (float64)b, (float64)c, flags, &env->fp_status);
        break;
    case 1:
        a = (uint64_t)(uint32_t)float32_muladd((float32)(uint32_t)a, (float32)(uint32_t)b,
                                               (float32)(uint32_t)c, flags, &env->fp_status);
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return 0;
    }

    linx_fp_sync_to_fcsr(env);
    return a;
}

void HELPER(linx_v_add)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR, uint32_t srctype, uint32_t shamt)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    uint64_t rhs = linx_vec_rhs_addsub(linx_vec_read_reg(env, srcR), srctype, shamt);

    const uint64_t res = lhs + rhs;
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_addi)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t imm)
{
    linx_vec_write_dst(env, dst, linx_vec_read_reg(env, srcL) + (uint64_t)imm);
}

void HELPER(linx_v_sub)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR, uint32_t srctype, uint32_t shamt)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    uint64_t rhs = linx_vec_rhs_addsub(linx_vec_read_reg(env, srcR), srctype, shamt);

    const uint64_t res = lhs - rhs;
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_subi)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t imm)
{
    linx_vec_write_dst(env, dst, linx_vec_read_reg(env, srcL) - (uint64_t)imm);
}

void HELPER(linx_v_and)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR, uint32_t srctype, uint32_t shamt)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_rhs_logic(linx_vec_read_reg(env, srcR), srctype, shamt);
    linx_vec_write_dst(env, dst, lhs & rhs);
}

void HELPER(linx_v_andi)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t imm)
{
    linx_vec_write_dst(env, dst, linx_vec_read_reg(env, srcL) & (uint64_t)(int32_t)imm);
}

void HELPER(linx_v_or)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                       uint32_t srcR, uint32_t srctype, uint32_t shamt)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_rhs_logic(linx_vec_read_reg(env, srcR), srctype, shamt);
    linx_vec_write_dst(env, dst, lhs | rhs);
}

void HELPER(linx_v_ori)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t imm)
{
    linx_vec_write_dst(env, dst, linx_vec_read_reg(env, srcL) | (uint64_t)(int32_t)imm);
}

void HELPER(linx_v_xor)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR, uint32_t srctype, uint32_t shamt)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_rhs_logic(linx_vec_read_reg(env, srcR), srctype, shamt);
    linx_vec_write_dst(env, dst, lhs ^ rhs);
}

void HELPER(linx_v_xori)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t imm)
{
    linx_vec_write_dst(env, dst, linx_vec_read_reg(env, srcL) ^ (uint64_t)(int32_t)imm);
}

void HELPER(linx_v_mul)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = lhs * rhs;
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_sll)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    linx_vec_write_dst(env, dst,
                       linx_vec_read_reg(env, srcL) << (linx_vec_read_reg(env, srcR) & 0x3fu));
}

void HELPER(linx_v_slli)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t shamt)
{
    linx_vec_write_dst(env, dst, linx_vec_read_reg(env, srcL) << (shamt & 0x3fu));
}

void HELPER(linx_v_srl)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    linx_vec_write_dst(env, dst,
                       linx_vec_read_reg(env, srcL) >> (linx_vec_read_reg(env, srcR) & 0x3fu));
}

void HELPER(linx_v_srli)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t shamt)
{
    linx_vec_write_dst(env, dst, linx_vec_read_reg(env, srcL) >> (shamt & 0x3fu));
}

void HELPER(linx_v_sra)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    const int64_t lhs = (int64_t)linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR) & 0x3fu;
    linx_vec_write_dst(env, dst, (uint64_t)(lhs >> rhs));
}

void HELPER(linx_v_srai)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t shamt)
{
    linx_vec_write_dst(env, dst, (uint64_t)((int64_t)linx_vec_read_reg(env, srcL) >> (shamt & 0x3fu)));
}

void HELPER(linx_v_max)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    const int64_t lhs = (int64_t)linx_vec_read_reg(env, srcL);
    const int64_t rhs = (int64_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, (uint64_t)(lhs > rhs ? lhs : rhs));
}

void HELPER(linx_v_min)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    const int64_t lhs = (int64_t)linx_vec_read_reg(env, srcL);
    const int64_t rhs = (int64_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, (uint64_t)(lhs < rhs ? lhs : rhs));
}

void HELPER(linx_v_madd)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR, uint32_t srcD)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t acc = linx_vec_read_reg(env, srcD);
    linx_vec_write_dst(env, dst, lhs * rhs + acc);
}

void HELPER(linx_v_div)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    const int64_t lhs = (int64_t)linx_vec_read_reg(env, srcL);
    const int64_t rhs = (int64_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, rhs == 0 ? UINT64_MAX : (uint64_t)(lhs / rhs));
}

void HELPER(linx_v_rem)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    const int64_t lhs = (int64_t)linx_vec_read_reg(env, srcL);
    const int64_t rhs = (int64_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, rhs == 0 ? (uint64_t)lhs : (uint64_t)(lhs % rhs));
}

void HELPER(linx_v_cmp_eq)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, lhs == rhs ? 1u : 0u);
}

void HELPER(linx_v_cmp_ne)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, lhs != rhs ? 1u : 0u);
}

void HELPER(linx_v_cmp_lt)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR)
{
    const int64_t lhs = (int64_t)linx_vec_read_reg(env, srcL);
    const int64_t rhs = (int64_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, lhs < rhs ? 1u : 0u);
}

void HELPER(linx_v_cmp_ltu)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, lhs < rhs ? 1u : 0u);
}

void HELPER(linx_v_cmp_ge)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR)
{
    const int64_t lhs = (int64_t)linx_vec_read_reg(env, srcL);
    const int64_t rhs = (int64_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, lhs >= rhs ? 1u : 0u);
}

void HELPER(linx_v_cmp_geu)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, lhs >= rhs ? 1u : 0u);
}

void HELPER(linx_v_cmp_and)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t srcR)
{
    const uint32_t lhs = (uint32_t)linx_vec_read_reg(env, srcL);
    const uint32_t rhs = (uint32_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs != 0u && rhs != 0u));
}

void HELPER(linx_v_cmp_or)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR)
{
    const uint32_t lhs = (uint32_t)linx_vec_read_reg(env, srcL);
    const uint32_t rhs = (uint32_t)linx_vec_read_reg(env, srcR);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs != 0u || rhs != 0u));
}

void HELPER(linx_v_cmp_andi)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                             uint32_t imm)
{
    const uint32_t lhs = (uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs != 0u && (int32_t)imm != 0));
}

void HELPER(linx_v_cmp_eqi)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t imm)
{
    const int32_t lhs = (int32_t)(uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs == (int32_t)imm));
}

void HELPER(linx_v_cmp_gei)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t imm)
{
    const int32_t lhs = (int32_t)(uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs >= (int32_t)imm));
}

void HELPER(linx_v_cmp_geui)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                             uint32_t imm)
{
    const uint32_t lhs = (uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs >= imm));
}

void HELPER(linx_v_cmp_lti)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t imm)
{
    const int32_t lhs = (int32_t)(uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs < (int32_t)imm));
}

void HELPER(linx_v_cmp_ltui)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                             uint32_t imm)
{
    const uint32_t lhs = (uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs < imm));
}

void HELPER(linx_v_cmp_nei)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t imm)
{
    const int32_t lhs = (int32_t)(uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs != (int32_t)imm));
}

void HELPER(linx_v_cmp_ori)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t imm)
{
    const uint32_t lhs = (uint32_t)linx_vec_read_reg(env, srcL);
    linx_vec_write_dst(env, dst, linx_vec_cmp_bool(lhs != 0u || (int32_t)imm != 0));
}

void HELPER(linx_v_bcnt)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t nminus1, uint32_t lsb)
{
    const uint32_t width = linx_vec_normalize_width(nminus1);
    const uint32_t field = linx_vec_bitfield_wrap32((uint32_t)linx_vec_read_reg(env, srcL),
                                                    lsb, width);
    linx_vec_write_dst(env, dst, __builtin_popcount(field));
}

void HELPER(linx_v_bic)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t nminus1, uint32_t lsb)
{
    const uint32_t width = linx_vec_normalize_width(nminus1);
    const uint32_t src = (uint32_t)linx_vec_read_reg(env, srcL);
    const uint32_t cleared = linx_vec_ror32(src, lsb) & ~linx_vec_mask_low_n32(width);
    linx_vec_write_dst(env, dst, linx_vec_rol32(cleared, lsb));
}

void HELPER(linx_v_bis)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t nminus1, uint32_t lsb)
{
    const uint32_t width = linx_vec_normalize_width(nminus1);
    const uint32_t src = (uint32_t)linx_vec_read_reg(env, srcL);
    const uint32_t setv = linx_vec_ror32(src, lsb) | linx_vec_mask_low_n32(width);
    linx_vec_write_dst(env, dst, linx_vec_rol32(setv, lsb));
}

void HELPER(linx_v_bxs)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t nminus1, uint32_t lsb)
{
    const uint32_t width = linx_vec_normalize_width(nminus1);
    const uint32_t field = linx_vec_bitfield_wrap32((uint32_t)linx_vec_read_reg(env, srcL),
                                                    lsb, width);
    linx_vec_write_dst(env, dst, linx_vec_sign_extend32(field, width));
}

void HELPER(linx_v_bxu)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t nminus1, uint32_t lsb)
{
    const uint32_t width = linx_vec_normalize_width(nminus1);
    const uint32_t field = linx_vec_bitfield_wrap32((uint32_t)linx_vec_read_reg(env, srcL),
                                                    lsb, width);
    linx_vec_write_dst(env, dst, field);
}

void HELPER(linx_v_clz)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t nminus1, uint32_t lsb)
{
    const uint32_t width = linx_vec_normalize_width(nminus1);
    const uint32_t field = linx_vec_bitfield_wrap32((uint32_t)linx_vec_read_reg(env, srcL),
                                                    lsb, width);
    const uint32_t count = field == 0u ? width
                                       : (uint32_t)__builtin_clz(field) - (32u - width);
    linx_vec_write_dst(env, dst, count);
}

void HELPER(linx_v_ctz)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t nminus1, uint32_t lsb)
{
    const uint32_t width = linx_vec_normalize_width(nminus1);
    const uint32_t field = linx_vec_bitfield_wrap32((uint32_t)linx_vec_read_reg(env, srcL),
                                                    lsb, width);
    const uint32_t count = field == 0u ? width : (uint32_t)__builtin_ctz(field);
    linx_vec_write_dst(env, dst, count);
}

void HELPER(linx_v_feq)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_cmp_eq(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res ? 1u : 0u);
}

void HELPER(linx_v_fne)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_cmp_eq(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res ? 0u : 1u);
}

void HELPER(linx_v_flt)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_cmp_lt(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res ? 1u : 0u);
}

void HELPER(linx_v_fge)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_cmp_ge(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res ? 1u : 0u);
}

void HELPER(linx_v_feqs)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    HELPER(linx_v_feq)(env, dst, srcL, srcR);
}

void HELPER(linx_v_fnes)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    HELPER(linx_v_fne)(env, dst, srcL, srcR);
}

void HELPER(linx_v_flts)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    HELPER(linx_v_flt)(env, dst, srcL, srcR);
}

void HELPER(linx_v_fges)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    HELPER(linx_v_fge)(env, dst, srcL, srcR);
}

void HELPER(linx_v_csel)(CPULinxState *env, uint32_t dst, uint32_t srcP,
                         uint32_t srcL, uint32_t srcR, uint32_t srctype)
{
    const uint64_t pred = linx_vec_read_reg(env, srcP);
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    uint64_t rhs = linx_vec_read_reg(env, srcR);

    if ((srctype & 0x3u) == 2u) {
        rhs = (uint64_t)(-(int64_t)rhs);
    }
    linx_vec_write_dst(env, dst, pred != 0 ? lhs : rhs);
}

void HELPER(linx_v_psel)(CPULinxState *env, uint32_t dst, uint32_t srcP,
                         uint32_t srcL, uint32_t srcR, uint32_t srctype)
{
    (void)srcR;
    HELPER(linx_v_csel)(env, dst, srcP, srcL, /*srcR=*/0, srctype);
}

void HELPER(linx_v_fadd)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_binop_add(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_fsub)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_binop_sub(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_fmul)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_binop_mul(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_fdiv)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = linx_fp_binop_div(env, lhs, rhs, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_fabs)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    const uint64_t res = linx_fp_unop_fabs(env, src, /*srctype=*/1);
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_fmadd)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                          uint32_t srcR, uint32_t srcA)
{
    linx_vec_write_dst(env, dst, linx_fp_ternop_muladd(env, linx_vec_read_reg(env, srcL),
                                                       linx_vec_read_reg(env, srcR),
                                                       linx_vec_read_reg(env, srcA), 1, 0));
}

void HELPER(linx_v_fmsub)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                          uint32_t srcR, uint32_t srcA)
{
    linx_vec_write_dst(env, dst, linx_fp_ternop_muladd(env, linx_vec_read_reg(env, srcL),
                                                       linx_vec_read_reg(env, srcR),
                                                       linx_vec_read_reg(env, srcA), 1,
                                                       float_muladd_negate_c));
}

void HELPER(linx_v_fnmadd)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR, uint32_t srcA)
{
    linx_vec_write_dst(env, dst, linx_fp_ternop_muladd(env, linx_vec_read_reg(env, srcL),
                                                       linx_vec_read_reg(env, srcR),
                                                       linx_vec_read_reg(env, srcA), 1,
                                                       float_muladd_negate_product));
}

void HELPER(linx_v_fnmsub)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR, uint32_t srcA)
{
    linx_vec_write_dst(env, dst, linx_fp_ternop_muladd(env, linx_vec_read_reg(env, srcL),
                                                       linx_vec_read_reg(env, srcR),
                                                       linx_vec_read_reg(env, srcA), 1,
                                                       float_muladd_negate_product |
                                                       float_muladd_negate_c));
}

void HELPER(linx_v_fsqrt)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    linx_vec_write_dst(env, dst, linx_fp_unop_sqrt(env, linx_vec_read_reg(env, srcL), 1));
}

void HELPER(linx_v_frecip)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    linx_vec_write_dst(env, dst, linx_fp_unop_recip(env, linx_vec_read_reg(env, srcL), 1));
}

void HELPER(linx_v_fexp)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    linx_vec_write_dst(env, dst, linx_fp_unop_exp(env, linx_vec_read_reg(env, srcL), 1));
}

void HELPER(linx_v_fclass)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    linx_vec_write_dst(env, dst, linx_fp_unop_class(env, linx_vec_read_reg(env, srcL), 1));
}

void HELPER(linx_v_fcvt)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t dsttype, uint32_t srctype)
{
    linx_vec_write_dst(env, dst,
                       linx_fp_fcvt(env, linx_vec_read_reg(env, srcL), dsttype, srctype));
}

void HELPER(linx_v_fcvti)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                          uint32_t dsttype, uint32_t srctype)
{
    linx_vec_write_dst(env, dst,
                       linx_fp_fcvti(env, linx_vec_read_reg(env, srcL), dsttype, srctype));
}

void HELPER(linx_v_fmax)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    linx_vec_write_dst(env, dst,
                       linx_fp_binop_max(env, linx_vec_read_reg(env, srcL),
                                         linx_vec_read_reg(env, srcR), 1));
}

void HELPER(linx_v_fmin)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t srcR)
{
    linx_vec_write_dst(env, dst,
                       linx_fp_binop_min(env, linx_vec_read_reg(env, srcL),
                                         linx_vec_read_reg(env, srcR), 1));
}

void HELPER(linx_v_icvt)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                         uint32_t dsttype, uint32_t srctype)
{
    linx_vec_write_dst(env, dst,
                       linx_int_icvt(env, linx_vec_read_reg(env, srcL), dsttype, srctype));
}

void HELPER(linx_v_icvtf)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                          uint32_t dsttype, uint32_t srctype)
{
    linx_vec_write_dst(env, dst,
                       linx_int_icvtf(env, linx_vec_read_reg(env, srcL), dsttype, srctype));
}

void HELPER(linx_v_rev)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t imml, uint32_t immr)
{
    (void)imml;
    (void)immr;
    /* Bring-up profile currently follows Sail's per-lane 32-bit byte reverse. */
    linx_vec_write_dst(env, dst, (uint64_t)bswap32((uint32_t)linx_vec_read_reg(env, srcL)));
}

void HELPER(linx_v_rdadd)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    /*
     * The current bring-up replay model presents one active scalar lane to the
     * V.RD* helpers per body iteration, so the helper input is already the
     * complete reduction result for that replay step. Do not accumulate across
     * iterations via the previous destination value.
     */
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdand)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdfadd)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdfmax)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdfmin)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdmax)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdmin)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdor)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_rdxor)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, src);
}

void HELPER(linx_v_lb_brg)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_lb_local)(env, dst, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 0u, false);
    const uint64_t addr = base + lane + (idx << eff_shamt);

    linx_lr_clear(env);
    const int8_t signed_value =
        (int8_t)cpu_ldb_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UB),
                            GETPC());
    const uint64_t value = (uint64_t)(int64_t)signed_value;

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem lb.resolved: tpc=0x%" PRIx64
                      " dst=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, dst, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }
    linx_vec_write_dst(env, dst, value);
}

void HELPER(linx_v_lh_brg)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_lh_local)(env, dst, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 1u, false);
    const uint64_t addr = base + (lane << 1) + (idx << eff_shamt);

    linx_lr_clear(env);
    const int16_t signed_value =
        (int16_t)cpu_ldw_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UW),
                             GETPC());
    const uint64_t value = (uint64_t)(int64_t)signed_value;

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem lh.resolved: tpc=0x%" PRIx64
                      " dst=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, dst, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }
    linx_vec_write_dst(env, dst, value);
}

void HELPER(linx_v_lbu_brg)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_lbu_local)(env, dst, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 0u, false);
    const uint64_t addr = base + lane + (idx << eff_shamt);

    linx_lr_clear(env);
    const uint32_t value =
        cpu_ldb_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UB), GETPC());

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem lbu.resolved: tpc=0x%" PRIx64
                      " dst=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, dst, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }
    linx_vec_write_dst(env, dst, value);
}

void HELPER(linx_v_lhu_brg)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                            uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_lhu_local)(env, dst, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 1u, false);
    const uint64_t addr = base + (lane << 1) + (idx << eff_shamt);

    linx_lr_clear(env);
    const uint32_t value =
        cpu_ldw_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UW), GETPC());

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem lhu.resolved: tpc=0x%" PRIx64
                      " dst=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, dst, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }
    linx_vec_write_dst(env, dst, value);
}

void HELPER(linx_v_sb_brg)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
                           uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_sb_local)(env, srcD, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 0u, true);
    const uint64_t addr = base + lane + (idx << eff_shamt);
    const uint8_t value = (uint8_t)linx_vec_read_reg(env, srcD);

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem sb.resolved: tpc=0x%" PRIx64
                      " srcD=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, srcD, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }

    linx_lr_clear(env);
    cpu_stb_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UB), GETPC());
}

void HELPER(linx_v_sh_brg)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
                           uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_sh_local)(env, srcD, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 1u, true);
    const uint64_t addr = base + (lane << 1) + (idx << eff_shamt);
    const uint16_t value = (uint16_t)linx_vec_read_reg(env, srcD);

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem sh.resolved: tpc=0x%" PRIx64
                      " srcD=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, srcD, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }

    linx_lr_clear(env);
    cpu_stw_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UW), GETPC());
}

void HELPER(linx_v_sw_brg)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
                           uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_sw_local)(env, srcD, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        /* Canonical v0.4: bridged/global accesses must use ri* base operands. */
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 2u, true);
    const uint64_t addr = base + (lane << 2) + (idx << eff_shamt);
    const uint32_t value = (uint32_t)linx_vec_read_reg(env, srcD);

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem sw.resolved: tpc=0x%" PRIx64
                      " srcD=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, srcD, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }

    linx_lr_clear(env);
    cpu_stl_mmu((CPUArchState *)env, addr, value, linx_oi_le(MO_UL), GETPC());
}

void HELPER(linx_v_lw_brg)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                           uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_lw_local)(env, dst, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(linx_vec_normalize_source_code(srcL)) !=
        LINX_VEC_REGCLASS_RI) {
        /* Canonical v0.4: bridged/global accesses must use ri* base operands. */
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 2u, false);
    const uint64_t addr = base + (lane << 2) + (idx << eff_shamt);

    linx_lr_clear(env);
    const uint32_t value =
        cpu_ldl_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UL), GETPC());

    if (linx_debug_body_replay_enabled_p() && env->body_tpc != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx body mem lw.resolved: tpc=0x%" PRIx64
                      " dst=0x%x srcL=0x%x srcR=0x%x"
                      " srcL_val=0x%" PRIx64 " srcR_val=0x%" PRIx64
                      " lc0=%" PRIu64 " shamt=%u resolved_addr=0x%" PRIx64
                      " value=0x%x lc1=%" PRIu64 "\n",
                      env->body_tpc, dst, srcL, srcR,
                      base, idx, lane, shamt, addr, value, env->lc[1]);
    }

    linx_vec_write_dst(env, dst, (uint64_t)value);
}

static bool linx_vec_resolve_local_tile(CPULinxState *env, uint32_t base_code,
                                        unsigned *tile_out)
{
    base_code = linx_vec_normalize_source_code(base_code);

    if (linx_vec_reg_class(base_code) != LINX_VEC_REGCLASS_TBASE) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local: base class mismatch code=0x%x class=%u idx=%u body_tpc=0x%" PRIx64 " lc0=%" PRIu64 " lc1=%" PRIu64 "\n",
                          base_code, linx_vec_reg_class(base_code),
                          linx_vec_reg_index(base_code), env->body_tpc,
                          env->lc[0], env->lc[1]);
        }
        return false;
    }
    unsigned idx = linx_vec_reg_index(base_code);
    unsigned tile = 0;
    if (!linx_vec_resolve_tile_base(env, idx, &tile)) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local: unresolved tile base code=0x%x idx=%u body_tpc=0x%" PRIx64 " iot_count=%u valid=%u lc0=%" PRIu64 " lc1=%" PRIu64 "\n",
                          base_code, idx, env->body_tpc, env->tile_iot_count,
                          env->tile_iot_valid, env->lc[0], env->lc[1]);
        }
        return false;
    }
    if (tile >= 32) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local: tile out of range code=0x%x idx=%u tile=%u body_tpc=0x%" PRIx64 "\n",
                          base_code, idx, tile, env->body_tpc);
        }
        return false;
    }
    *tile_out = tile;
    return true;
}

static bool linx_vec_local_ensure_store_bytes(CPULinxState *env, unsigned tile,
                                              uint64_t off, uint32_t size)
{
    if (tile >= 32 || size == 0 || off > UINT64_MAX - size) {
        return false;
    }

    const uint64_t required = off + size;
    if (required > LINX_TILE_MAX_BYTES) {
        return false;
    }
    if (env->tile_reg_bytes[tile] >= required) {
        linx_tile_set_elem_bytes(env, tile, size);
        return true;
    }

    const uint32_t old_bytes = env->tile_reg_bytes[tile];
    const unsigned old_words = (old_bytes + 3u) / 4u;
    const unsigned new_words = (unsigned)((required + 3u) / 4u);
    if (new_words > LINX_TILE_MAX_WORDS) {
        return false;
    }

    for (unsigned w = old_words; w < new_words; w++) {
        env->tile_reg[tile][w] = 0;
    }
    env->tile_reg_bytes[tile] = (uint32_t)required;
    linx_tile_set_elem_bytes(env, tile, size);
    return true;
}

void HELPER(linx_v_lbu_local)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                              uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 0u, false);
    const uint64_t off = lane + (idx << eff_shamt);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (bytes == 0 || off + 1u > bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    const unsigned bit = (unsigned)(off & 0x3u) * 8u;
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint32_t packed = env->tile_reg[tile][word];
    linx_vec_write_dst(env, dst, (packed >> bit) & 0xffu);
}

void HELPER(linx_v_lb_local)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                             uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 0u, false);
    const uint64_t off = lane + (idx << eff_shamt);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (bytes == 0 || off + 1u > bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    const unsigned bit = (unsigned)(off & 0x3u) * 8u;
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint32_t packed = env->tile_reg[tile][word];
    const int8_t signed_value = (int8_t)((packed >> bit) & 0xffu);
    linx_vec_write_dst(env, dst, (uint64_t)(int64_t)signed_value);
}

void HELPER(linx_v_lhu_local)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                              uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 1u, false);
    const uint64_t off = (lane << 1) + (idx << eff_shamt);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (bytes == 0 || off + 2u > bytes || (off & 1u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    const unsigned bit = (unsigned)(off & 0x2u) * 8u;
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint32_t packed = env->tile_reg[tile][word];
    linx_vec_write_dst(env, dst, (packed >> bit) & 0xffffu);
}

void HELPER(linx_v_lh_local)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                             uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 1u, false);
    const uint64_t off = (lane << 1) + (idx << eff_shamt);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (bytes == 0 || off + 2u > bytes || (off & 1u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    const unsigned bit = (unsigned)(off & 0x2u) * 8u;
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint32_t packed = env->tile_reg[tile][word];
    const int16_t signed_value = (int16_t)((packed >> bit) & 0xffffu);
    linx_vec_write_dst(env, dst, (uint64_t)(int64_t)signed_value);
}

void HELPER(linx_v_sb_local)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
                             uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sb: local bit clear srcD=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 "\n",
                          srcD, srcL, srcR, shamt, env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 0u, true);
    const uint64_t off = lane + (idx << eff_shamt);
    const uint8_t value = (uint8_t)linx_vec_read_reg(env, srcD);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (!linx_vec_local_ensure_store_bytes(env, tile, off, 1u)) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sb: byte range fail tile=%u bytes=%u off=0x%" PRIx64 " idx=0x%" PRIx64 " lane=%" PRIu64 " srcD=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 " lc1=%" PRIu64 "\n",
                          tile, bytes, off, idx, lane, srcD, srcL, srcR, shamt,
                          env->body_tpc, env->lc[1]);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    const unsigned bit = (unsigned)(off & 0x3u) * 8u;
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    uint32_t old = env->tile_reg[tile][word];
    old = (old & ~(0xffu << bit)) | ((uint32_t)value << bit);
    env->tile_reg[tile][word] = old;
}

void HELPER(linx_v_sh_local)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
                             uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sh: local bit clear srcD=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 "\n",
                          srcD, srcL, srcR, shamt, env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 1u, true);
    const uint64_t off = (lane << 1) + (idx << eff_shamt);
    const uint16_t value = (uint16_t)linx_vec_read_reg(env, srcD);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (!linx_vec_local_ensure_store_bytes(env, tile, off, 2u)) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sh: byte range fail tile=%u bytes=%u off=0x%" PRIx64 " idx=0x%" PRIx64 " lane=%" PRIu64 " srcD=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 " lc1=%" PRIu64 "\n",
                          tile, bytes, off, idx, lane, srcD, srcL, srcR, shamt,
                          env->body_tpc, env->lc[1]);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((off & 1u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    const unsigned bit = (unsigned)(off & 0x2u) * 8u;
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    uint32_t old = env->tile_reg[tile][word];
    old = (old & ~(0xffffu << bit)) | ((uint32_t)value << bit);
    env->tile_reg[tile][word] = old;
}

void HELPER(linx_v_sw_local)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
                             uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sw: local bit clear srcD=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 "\n",
                          srcD, srcL, srcR, shamt, env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 2u, true);
    const uint64_t off = (lane << 2) + (idx << eff_shamt);
    const uint32_t value = (uint32_t)linx_vec_read_reg(env, srcD);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (!linx_vec_local_ensure_store_bytes(env, tile, off, 4u)) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sw: byte range fail tile=%u bytes=%u off=0x%" PRIx64 " idx=0x%" PRIx64 " lane=%" PRIu64 " srcD=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 " lc1=%" PRIu64 "\n",
                          tile, bytes, off, idx, lane, srcD, srcL, srcR, shamt,
                          env->body_tpc, env->lc[1]);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((off & 3u) != 0) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sw: unaligned off tile=%u off=0x%" PRIx64 " idx=0x%" PRIx64 " lane=%" PRIu64 " srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 "\n",
                          tile, off, idx, lane, srcL, srcR, shamt,
                          env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    if (word >= LINX_TILE_MAX_WORDS) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local sw: word out of range tile=%u word=%u off=0x%" PRIx64 " bytes=%u body_tpc=0x%" PRIx64 "\n",
                          tile, word, off, bytes, env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    env->tile_reg[tile][word] = value;
}

void HELPER(linx_v_lw_local)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                             uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local == 0) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local lw: local bit clear dst=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 "\n",
                          dst, srcL, srcR, shamt, env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    unsigned tile = 0;
    if (!linx_vec_resolve_local_tile(env, srcL, &tile)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint32_t eff_shamt = linx_vec_mem_index_shift(srcR, shamt, 2u, false);
    const uint64_t off = (lane << 2) + (idx << eff_shamt);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (bytes == 0 || off + 4u > bytes) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local lw: byte range fail tile=%u bytes=%u off=0x%" PRIx64 " idx=0x%" PRIx64 " lane=%" PRIu64 " dst=0x%x srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 " lc1=%" PRIu64 "\n",
                          tile, bytes, off, idx, lane, dst, srcL, srcR, shamt,
                          env->body_tpc, env->lc[1]);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((off & 3u) != 0) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local lw: unaligned off tile=%u off=0x%" PRIx64 " idx=0x%" PRIx64 " lane=%" PRIu64 " srcL=0x%x srcR=0x%x shamt=%u body_tpc=0x%" PRIx64 "\n",
                          tile, off, idx, lane, srcL, srcR, shamt,
                          env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    if (word >= LINX_TILE_MAX_WORDS) {
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local lw: word out of range tile=%u word=%u off=0x%" PRIx64 " bytes=%u body_tpc=0x%" PRIx64 "\n",
                          tile, word, off, bytes, env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint32_t value = env->tile_reg[tile][word];

    const unsigned cls = linx_vec_reg_class(dst);
    const unsigned didx = linx_vec_reg_index(dst);
    switch (cls) {
    case LINX_VEC_REGCLASS_VT:
        linx_vec_write_vt(env, didx, (uint64_t)value);
        return;
    case LINX_VEC_REGCLASS_VU:
        linx_vec_write_vu(env, didx, (uint64_t)value);
        return;
    case LINX_VEC_REGCLASS_VM:
        linx_vec_write_vm(env, didx, (uint64_t)value);
        return;
    case LINX_VEC_REGCLASS_VN:
        linx_vec_write_vn(env, didx, (uint64_t)value);
        return;
    default:
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local lw: invalid dst class dst=0x%x class=%u idx=%u tile=%u word=%u value=0x%x body_tpc=0x%" PRIx64 "\n",
                          dst, cls, didx, tile, word, value, env->body_tpc);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
}


static unsigned linx_insn_len(uint16_t hw)
{
    if ((hw & 0x1) == 0) {
        return ((hw & 0xf) == 0xe) ? 6 : 2;
    }
    return ((hw & 0xf) == 0xf) ? 8 : 4;
}

static bool linx_read_code_bytes(CPULinxState *env, uint64_t pc,
                                 uint8_t *buf, size_t len)
{
    size_t done = 0;
    const int mmu_idx = linx_env_mmu_index(env);

    while (done < len) {
        const vaddr va = (vaddr)(pc + done);
        size_t page_left = TARGET_PAGE_SIZE -
            (size_t)(va & (TARGET_PAGE_SIZE - 1));
        const size_t n = MIN(len - done, page_left);
        void *host = NULL;
        const int flags =
            probe_access_flags(env, va, (int)n, MMU_INST_FETCH, mmu_idx,
                               true, &host, 0);

        if ((flags & (TLB_INVALID_MASK | TLB_MMIO)) || host == NULL) {
            return false;
        }

        memcpy(buf + done, host, n);
        done += n;
    }

    return true;
}

static bool linx_is_legacy_ret_j_wrapper_target(CPULinxState *env, uint64_t pc)
{
    uint8_t buf[8];
    uint32_t insn;

    if (pc < 2) {
        return false;
    }
    if (!linx_read_code_bytes(env, pc - 2, buf, sizeof(buf))) {
        return false;
    }

    insn = ldl_le_p(buf + 2);
    return lduw_le_p(buf) == 0x3800 &&
           (insn & 0x0000707fu) == 0x00000037u &&
           lduw_le_p(buf + 6) == 0x0000;
}

static bool linx_is_bstart_at_addr(CPULinxState *env, uint64_t pc)
{
    uint8_t buf[8];

    if (!linx_read_code_bytes(env, pc, buf, 2)) {
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
        if (!linx_read_code_bytes(env, pc, buf, 4)) {
            return false;
        }
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

        if (linx_is_legacy_ret_j_wrapper_target(env, pc)) {
            return true;
        }

        /* Generic BSTART split forms: low opcode 0x11/0x21 with simm25 target. */
        if ((insn & 0x7f) == 0x11 || (insn & 0x7f) == 0x21) {
            return true;
        }

        /* BSTART.*: bits[6:0]=0x01, branch kind in bits [14:12] is non-zero. */
        if ((insn & 0x7f) == 0x01 && ((insn >> 12) & 0x7) != 0) {
            return true;
        }

        /* Template blocks: frame templates (0x41) and memory templates (0x31). */
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
        if (!linx_read_code_bytes(env, pc, buf, 6)) {
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

    if (len == 8) {
        if (!linx_read_code_bytes(env, pc, buf, 8)) {
            return false;
        }

        /*
         * 64-bit L.BSTART.*: 16-bit trailer, 16 bits of padding, then the
         * 32-bit BSTART main word in bytes [4..7].
         */
        const uint32_t main32 = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                                ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);

        if ((main32 & 0x7f) == 0x01 && ((main32 >> 12) & 0x7) != 0) {
            return true;
        }

        return false;
    }

    return false;
}

static bool linx_is_call_fallthrough_target(CPULinxState *env, uint64_t pc,
                                            uint64_t target)
{
    uint8_t buf[8] = { 0 };

    if (!linx_read_code_bytes(env, pc, buf, 2)) {
        return false;
    }

    const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    const unsigned len = linx_insn_len(hw);
    if (target != pc + len) {
        return false;
    }

    if (len == 2) {
        if ((hw & 0xc7ff) == 0x0000 || (hw & 0xc7ff) == 0x0080) {
            const uint8_t brtype = (hw >> 11) & 0x7;
            return brtype == 4;
        }
        return false;
    }

    if (!linx_read_code_bytes(env, pc, buf, len)) {
        return false;
    }

    if (len == 4) {
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        return (insn & 0x7f) == 0x01 && ((insn >> 12) & 0x7) == 4;
    }

    if (len == 6) {
        const uint16_t prefix = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
        const uint32_t main32 = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8) |
                                ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24);
        return (prefix & 0xf) == 0xe && (main32 & 0xff) == 0x01 &&
               ((main32 >> 12) & 0x7) == 4;
    }

    if (len == 8) {
        const uint32_t main32 = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                                ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
        return (main32 & 0x7f) == 0x01 && ((main32 >> 12) & 0x7) == 4;
    }

    return false;
}

void HELPER(linx_check_bstart_target)(CPULinxState *env, uint64_t target)
{
    const size_t slot = linx_bstart_cache_slot(target);
    const uint8_t mmu_idx = (uint8_t)linx_env_mmu_index(env);
    const bool stats_enabled = unlikely(linx_bstart_cache_stats_enabled_p());

    if (stats_enabled) {
        linx_bstart_cache_stat_checks++;
    }

    if (linx_cfi_trace_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: indirect target check pc=0x%" PRIx64
                      " target=0x%" PRIx64 "\n",
                      env->pc, target);
    }

    /*
     * This helper is on the hot path for indirect control flow (RET/IND/ICALL
     * and template returns). Cache the most recently-validated targets to avoid
     * re-reading guest memory or re-scanning continuation metadata for tight
     * call/return loops.
     *
     * MMU programming and TLB invalidation reset this cache. ACR/CSTATE
     * switches keep entries because each line is tagged by the MMU index.
     * Self-modifying-code debug can opt back into old revalidate-on-hit
     * behavior with LINX_BSTART_CACHE_REVALIDATE=1.
     */
    if (env->bstart_cache_valid[slot] &&
        env->bstart_cache_tag[slot] == target &&
        env->bstart_cache_mmu_idx[slot] == mmu_idx) {
        if (!linx_bstart_cache_revalidate_enabled_p()) {
            if (stats_enabled) {
                linx_bstart_cache_stat_hits++;
                linx_bstart_cache_stats_emit_maybe(env);
            }
            return;
        }
        if (stats_enabled) {
            linx_bstart_cache_stat_revalidations++;
        }
        if (linx_is_bstart_at_addr(env, target)) {
            return;
        }
        env->bstart_cache_valid[slot] = 0;
    }

    if (linx_is_call_continuation(env, target)) {
        env->bstart_cache_tag[slot] = target;
        env->bstart_cache_mmu_idx[slot] = mmu_idx;
        env->bstart_cache_valid[slot] = 1;
        if (stats_enabled) {
            linx_bstart_cache_stat_continuations++;
            linx_bstart_cache_stat_inserts++;
            linx_bstart_cache_stats_emit_maybe(env);
        }
        return;
    }

    if (linx_is_call_fallthrough_target(env, env->pc, target)) {
        if (stats_enabled) {
            linx_bstart_cache_stat_fallthroughs++;
            linx_bstart_cache_stats_emit_maybe(env);
        }
        return;
    }

    if (linx_is_bstart_at_addr(env, target)) {
        env->bstart_cache_tag[slot] = target;
        env->bstart_cache_mmu_idx[slot] = mmu_idx;
        env->bstart_cache_valid[slot] = 1;
        if (stats_enabled) {
            linx_bstart_cache_stat_bstarts++;
            linx_bstart_cache_stat_inserts++;
            linx_bstart_cache_stats_emit_maybe(env);
        }
        return;
    }

    CPUState *cs = env_cpu(env);

    {
        uint8_t buf[8] = { 0 };
        if (linx_read_code_bytes(env, target, buf, sizeof(buf))) {
            bool all_zero = true;
            for (size_t i = 0; i < sizeof(buf); i++) {
                if (buf[i] != 0) {
                    all_zero = false;
                    break;
                }
            }
            if ((env->acr & 0xfu) == 2 && all_zero) {
                /*
                 * User text can be demand-paged.  cpu_memory_rw_debug() may
                 * observe an unpopulated executable page as zeros instead of
                 * raising the fetch fault that would page it in.  Defer to the
                 * real fetch path so Linux can service the instruction fault.
                 */
                if (stats_enabled) {
                    linx_bstart_cache_stat_defers++;
                    linx_bstart_cache_stats_emit_maybe(env);
                }
                return;
            }
            const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            const unsigned len = linx_insn_len(hw);
            trace_linx_cfi_bad_target(env->pc, target, (uint32_t)hw, len);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: target bytes @0x%" PRIx64 ": %02x %02x %02x %02x %02x %02x %02x %02x (hw=0x%04x len=%u)\n",
                          target, buf[0], buf[1], buf[2], buf[3],
                          buf[4], buf[5], buf[6], buf[7], hw, len);
        } else {
            /*
             * The target page may not be present yet (lazy demand paging). Defer
             * block-start validation to fetch-time instead of forcing a synthetic
             * BAD_BRANCH_TARGET trap here.
             */
            if (stats_enabled) {
                linx_bstart_cache_stat_defers++;
                linx_bstart_cache_stats_emit_maybe(env);
            }
            return;
        }
    }

    if (stats_enabled) {
        linx_bstart_cache_stat_bad++;
        linx_bstart_cache_stats_emit_maybe(env);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: invalid branch target 0x%" PRIx64 " (not a block start marker)\n",
                  target);
    /* v0.3: E_BLOCK(EC_CFI), TRAPARG0 is the source PC/TPC (not the target VA). */
    env->pending_trap_arg0 = env->pc;
    env->pending_trap_cause = linx_eblock_cfi_cause(LINX_EBLOCK_CFI_BAD_TARGET);
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

    if (linx_print_insn_count()) {
        fprintf(stderr, "LINX_INSN_COUNT=%" PRIu64 "\n", env->insn_count);
        fflush(stderr);
    }

    linx_cosim_init(env);
    if (env->cosim.active) {
        (void)linx_cosim_send_end(env, "guest_exit");
        linx_cosim_finish(env);
    }
    
    /* Request graceful shutdown of the VM */
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);

    /* Exit immediately from the execution loop. */
    cpu_loop_exit_noexc(cs);
}
