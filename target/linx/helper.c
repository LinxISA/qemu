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
#include "system/address-spaces.h"
#include "system/memory.h"
#include <inttypes.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Optional compatibility addend configured from $LINX_CALLFRAME_SIZE. */
extern uint64_t linx_callframe_size;

static inline void linx_bstart_cache_reset(CPULinxState *env)
{
    memset(env->bstart_cache_valid, 0, sizeof(env->bstart_cache_valid));
}

static inline void linx_bstart_cache_reset_page(CPULinxState *env, uint64_t addr)
{
    const uint64_t page = addr & TARGET_PAGE_MASK;

    for (size_t i = 0; i < LINX_BSTART_CACHE_SIZE; i++) {
        if (env->bstart_cache_valid[i] &&
            (env->bstart_cache_tag[i] & TARGET_PAGE_MASK) == page) {
            env->bstart_cache_valid[i] = 0;
        }
    }
}

static bool linx_is_bstart_at_addr(CPULinxState *env, uint64_t pc);
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
static bool linx_debug_pc_watch_match_gpr_enabled;
static unsigned linx_debug_pc_watch_match_gpr;
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
static bool linx_heartbeat_regs_enabled;
static unsigned linx_heartbeat_dump_code_bytes;
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

    linx_heartbeat_inited = true;
}

static uint64_t linx_heartbeat_next_count(uint64_t bucket)
{
    if (linx_heartbeat_interval == 0 ||
        bucket >= UINT64_MAX / linx_heartbeat_interval) {
        return UINT64_MAX;
    }
    return (bucket + 1) * linx_heartbeat_interval;
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
            " in_body=%u progress=%s same_site=%" PRIu64
            " sp=0x%" PRIx64
            " ra=0x%" PRIx64
            " tp=0x%" PRIx64
            " etemp1=0x%" PRIx64
            " etemp0_1=0x%" PRIx64
            " a0=0x%" PRIx64
            " a1=0x%" PRIx64
            "\n",
            qemu_clock_get_ms(QEMU_CLOCK_REALTIME),
            env->insn_count, delta, pc, env->bpc, env->body_tpc,
            env->pc, env->acr & 0xFu, env->ssr[0x20],
            env->brtype, env->tgt, env->in_body,
            progress, linx_heartbeat_same_site_repeats,
            env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->ssr[0x0000], env->ssr_acr[1][0xF05],
            env->ssr_acr[1][0xF06],
            env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1]);
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
    uint64_t lo = 0;
    uint64_t hi = 0;

    if (linx_mem_trace_inited) {
        return;
    }

    addr_s = getenv("LINX_MEM_TRACE_ADDR");
    if (addr_s && addr_s[0] && linx_parse_u64(addr_s, &value)) {
        linx_mem_trace_addr = value;
        linx_mem_trace_size = 8;
        linx_mem_trace_limit = 128;
        linx_mem_trace_enabled = true;
    }

    size_s = getenv("LINX_MEM_TRACE_SIZE");
    if (linx_mem_trace_enabled && size_s && size_s[0] &&
        strcmp(size_s, "0") != 0 && linx_parse_u64(size_s, &value)) {
        linx_mem_trace_size = value;
    }
    if (linx_mem_trace_size == 0) {
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
                                 uint64_t value)
{
    linx_mem_trace_init();
    if (!linx_mem_trace_enabled) {
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
    if (!linx_mem_trace_ranges_overlap(addr, size,
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
    if (linx_mem_trace_context_enabled) {
        const int mmu_idx = ((env->acr & 0xFu) == 2) ? 1 : 0;
        fprintf(stderr,
                " mmu_idx=%d ttbr0=0x%" PRIx64
                " ttbr1=0x%" PRIx64 " tcr=0x%" PRIx64,
                mmu_idx, env->ssr_acr[1][0xF10], env->ssr_acr[1][0xF11],
                env->ssr_acr[1][0xF12]);
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

void HELPER(linx_mem_trace_load)(CPULinxState *env, uint64_t pc, uint64_t addr,
                                 uint32_t size, uint64_t value)
{
    linx_mem_trace_probe(env, false, pc, addr, size, value);
}

void HELPER(linx_mem_trace_store)(CPULinxState *env, uint64_t pc, uint64_t addr,
                                  uint32_t size, uint64_t value)
{
    linx_mem_trace_probe(env, true, pc, addr, size, value);
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

static inline bool linx_call_trace_disabled_fast(void)
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

static inline void linx_call_trace_emit(CPULinxState *env, uint32_t event,
                                        uint64_t pc, uint64_t extra0,
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

static bool linx_fret_stk_trace_matches(CPULinxState *env, uint64_t pc,
                                        uint64_t restored_ra)
{
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

static bool linx_fentry_trace_matches(CPULinxState *env, uint64_t pc,
                                      uint64_t old_sp, uint64_t new_sp,
                                      uint64_t save_ra)
{
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
                                   uint32_t event, uint64_t extra0)
{
    linx_call_trace_emit(env, event, pc, extra0, 0);
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
            linx_debug_pc_watch_match_gpr_enabled = true;
            linx_debug_pc_watch_match_gpr = gpr;
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
        if (linx_debug_pc_watch_match_gpr_enabled) {
            const uint64_t actual = env->gpr[linx_debug_pc_watch_match_gpr];
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
                " return_pc=0x%" PRIx64 " call_ra_set=%u call_setret_pending=%u\n",
                pc, linx_debug_pc_watch_hits[i],
                linx_debug_pc_watch_printed[i], env->insn_count,
                env->gpr[LINX_REG_SP],
                env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1],
                env->gpr[LINX_REG_A2], env->gpr[LINX_REG_RA], tp, env->ssr[0x20],
                env->cond, env->carg, env->brtype, env->tgt, env->bpc,
                env->tq[0], env->tq[1], env->uq[0], env->uq[1],
                env->in_body, env->blocktype, env->body_tpc,
                env->return_pc, env->call_ra_set, env->call_setret_pending);
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

static inline bool linx_trace_capture_active(CPULinxState *env)
{
    if (!env->cosim.inited) {
        linx_cosim_init(env);
    }
    if (!env->commit_trace.inited) {
        linx_commit_trace_init(env);
    }
    if (!env->minst_trace.inited) {
        linx_minst_trace_init(env);
    }
    return (env->commit_trace.enabled && env->commit_trace.fp) ||
           (env->minst_trace.enabled && env->minst_trace.fp) ||
           env->cosim.active || qemu_loglevel_mask(LOG_LINX_MEM);
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

static inline void linx_trace_wb(CPULinxState *env, uint32_t rd, uint64_t data)
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

static inline void linx_trace_mem(CPULinxState *env, bool is_store,
                                  uint64_t addr, uint64_t wdata,
                                  uint64_t rdata, uint32_t size)
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

static inline G_NORETURN void linx_template_commit_and_exit(CPULinxState *env,
                                                            CPUState *cs,
                                                            uint64_t next_pc)
{
    if (linx_trace_capture_active(env)) {
        HELPER(linx_commit_trace)(env, next_pc);
    }
    cpu_loop_exit_noexc(cs);
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
    if (strstr(mnemonic, "bstart_tma")) {
        return "tma";
    }
    if (strstr(mnemonic, "bstart_cube")) {
        return "cube";
    }
    if (strstr(mnemonic, "bstart_fixp")) {
        return "fixp";
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
    if (strcmp(mnemonic, "bstart_tma") == 0) {
        return (LinxMinstCanonicalInfo){ "BSTART.TLOAD", "d0c18bb0ab15", "system", "tma" };
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

#define LINX_VIRT_EXIT_REG UINT64_C(0x10000004)

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
    terminal_store = mem_is_store && env->trace_mem_addr == LINX_VIRT_EXIT_REG;
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
            mem_size, 0ull, mem_rdata,
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
        linx_bstart_cache_reset(env);
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
    tlb_flush(env_cpu(env));
    linx_bstart_cache_reset(env);
}

void HELPER(linx_tlb_ia)(CPULinxState *env, uint64_t asid, uint64_t pc)
{
    linx_tlb_trace_emit(env, "ia", pc, asid, true);
    /*
     * QEMU's current Linx TLB model is not ASID-tagged independently from
     * TTBR/MMU-index state, so keep TLB.IA as a conservative local full flush.
     */
    tlb_flush(env_cpu(env));
    linx_bstart_cache_reset(env);
}

void HELPER(linx_tlb_iv)(CPULinxState *env, uint64_t addr, uint64_t pc)
{
    linx_tlb_trace_emit(env, "iv", pc, addr, true);
    tlb_flush_page(env_cpu(env), (vaddr)addr);
    linx_bstart_cache_reset_page(env, addr);
}

void HELPER(linx_tlb_iav)(CPULinxState *env, uint64_t packed, uint64_t pc)
{
    const uint64_t addr = packed & ((UINT64_C(1) << 44) - 1);

    linx_tlb_trace_emit(env, "iav", pc, packed, true);
    tlb_flush_page(env_cpu(env), (vaddr)addr);
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
    linx_bstart_cache_reset(env);
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
         * RRA_RESTORE: keep the full second-level architectural snapshot that
         * was restored by linx_acr_restore_block_state().
         *
         * Only continuation control-flow (BPC/TPC) is sourced from manager
         * EBARG below. This prevents handler-side EBARG pollution from
         * clobbering resumed queue/lane state.
         */
    } else {
        env->pending_trap_arg0 = (uint64_t)rra_type;
        env->pending_trap_cause = 0;
        helper_raise_exception(env, LINX_EXCP_EXEC_STATE_CHECK);
        return;
    }

    /* v0.2: always restore BPC from EBARG. */
    env->bpc = resume_bpc;

    env->acr = target;
    linx_bstart_cache_reset(env);
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
    LINX_BLOCK_FIXP = 8,
};

enum {
    LINX_TMA_MGATHER   = 0,
    LINX_TMA_MSCATTER  = 1,
    LINX_TMA_TLOAD     = 2,
    LINX_TMA_TPREFETCH = 3,
    LINX_TMA_TSTORE    = 4,
};

enum {
    LINX_CUBE_TGEMV   = 0,
    LINX_CUBE_TMATMUL = 1,
};

enum {
    LINX_FIXP_TDEQUANT = 0,
    LINX_FIXP_TEXTRACT = 1,
    LINX_FIXP_TINSERT  = 2,
    LINX_FIXP_TMOV     = 3,
    LINX_FIXP_TCONCAT  = 4,
    LINX_FIXP_TFILLPAD = 5,
    LINX_FIXP_TQUANT   = 6,
    LINX_FIXP_TTRANS   = 7,
};

typedef struct LinxTileITPDesc {
    uint32_t src0;
    uint32_t src1;
    uint32_t last;
    uint32_t src_pair;
    uint32_t s0r;
    uint32_t s1r;
} LinxTileITPDesc;

typedef struct LinxTileOTADesc {
    uint32_t dst;
    uint32_t cell_count_m1;
    uint32_t last;
    uint32_t dst_slot;
} LinxTileOTADesc;

static inline LinxTileITPDesc linx_tile_decode_itp(uint64_t packed)
{
    LinxTileITPDesc d;
    d.src0 = (packed >> 0) & 0xffu;
    d.src1 = (packed >> 8) & 0xffu;
    d.last = (packed >> 16) & 0x1u;
    d.src_pair = (packed >> 17) & 0x3u;
    d.s0r = (packed >> 19) & 0x1u;
    d.s1r = (packed >> 20) & 0x1u;
    return d;
}

static inline LinxTileOTADesc linx_tile_decode_ota(uint64_t packed)
{
    LinxTileOTADesc d;
    d.dst = (packed >> 0) & 0xffu;
    d.cell_count_m1 = (packed >> 8) & 0xffu;
    d.last = (packed >> 16) & 0x1u;
    d.dst_slot = (packed >> 17) & 0x3u;
    return d;
}

static inline uint64_t linx_tile_bytes_from_ota(const LinxTileOTADesc *desc)
{
    return ((uint64_t)desc->cell_count_m1 + 1u) * 128u;
}

static inline bool linx_tile_valid_arch_id(uint32_t tile)
{
    return tile != 0u && tile <= 32u;
}

static inline unsigned linx_tile_storage_index(uint32_t tile)
{
    return (unsigned)((tile - 1u) & 0x1fu);
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
    case 4u:  /* FP16/BF16-like encodings in bring-up streams */
        return 2u;
    case 1u:  /* FP32-like */
    case 0u:  /* I32-like */
    default:
        return 4u;
    }
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

static int linx_frame_restore_prepare(CPULinxState *env, uint64_t stacksize,
                                      uint64_t new_sp, uint64_t restore_base,
                                      int begin, int end,
                                      uint32_t regs[LINX_GPR_COUNT],
                                      uint64_t addrs[LINX_GPR_COUNT],
                                      uint64_t values[LINX_GPR_COUNT])
{
    const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
    const int mmu_idx = linx_env_mmu_index(env);
    int reg = begin;
    uint32_t step = 1;
    int n = 0;

    if (count <= 0) {
        return 0;
    }

    while (1) {
        const uint64_t addr = new_sp - restore_base - ((uint64_t)step * 8ull);

        if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
            regs[n] = (uint32_t)reg;
            addrs[n] = addr;
            values[n] = cpu_ldq_le_mmuidx_ra(env, (abi_ptr)addr, mmu_idx,
                                             GETPC());
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

void HELPER(linx_template_fentry)(CPULinxState *env, uint64_t cur_pc,
                                  uint64_t next_pc, uint32_t reg_begin,
                                  uint32_t reg_end, uint64_t stacksize)
{
    CPUState *cs = env_cpu(env);
    const uint64_t adj = stacksize + linx_callframe_size;
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
    const uint64_t old_sp = env->gpr[LINX_REG_SP];
    const uint64_t new_sp = old_sp - adj;
    const int mmu_idx = linx_env_mmu_index(env);
    void *save_hosts[LINX_GPR_COUNT] = { NULL };
    bool fentry_trace;

    linx_call_trace_emit(env, LINX_CALL_TRACE_FENTRY, cur_pc, new_sp, stacksize);

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
                save_hosts[reg] =
                    probe_write(env, (vaddr)addr, 8, mmu_idx, GETPC());
            }
            if (reg == end) {
                break;
            }
            reg = linx_next_fentry_reg(reg);
            step++;
        }
    }

    fentry_trace = linx_fentry_trace_matches(env, cur_pc, old_sp, new_sp,
                                             env->gpr[LINX_REG_RA]);

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
                cpu_stq_le_mmuidx_ra(env, (abi_ptr)addr, v, mmu_idx, GETPC());
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
    linx_template_commit_and_exit(env, cs, env->pc);
}

void HELPER(linx_template_fexit)(CPULinxState *env, uint64_t cur_pc,
                                 uint64_t next_pc, uint32_t reg_begin,
                                 uint32_t reg_end, uint64_t stacksize)
{
    CPUState *cs = env_cpu(env);
    const uint64_t adj = stacksize + linx_callframe_size;
    const uint64_t restore_base = linx_callframe_size;
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    const uint64_t old_sp = env->gpr[LINX_REG_SP];
    const uint64_t new_sp = old_sp + adj;
    uint32_t regs[LINX_GPR_COUNT];
    uint64_t addrs[LINX_GPR_COUNT];
    uint64_t values[LINX_GPR_COUNT];
    const int restore_count =
        linx_frame_restore_prepare(env, stacksize, new_sp, restore_base,
                                   begin, end, regs, addrs, values);

    if (adj) {
        env->gpr[LINX_REG_SP] = new_sp;
        linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
    }

    linx_frame_restore_commit(env, cur_pc, regs, addrs, values,
                              restore_count);

    linx_template_clear(env);
    env->pc = next_pc;
    linx_template_commit_and_exit(env, cs, env->pc);
}

void HELPER(linx_template_fret_stk)(CPULinxState *env, uint64_t cur_pc,
                                    uint64_t next_pc, uint32_t reg_begin,
                                    uint32_t reg_end, uint64_t stacksize)
{
    CPUState *cs = env_cpu(env);
    const uint64_t adj = stacksize + linx_callframe_size;
    const uint64_t restore_base = linx_callframe_size;
    const int begin = (int)reg_begin;
    const int end = (int)reg_end;
    const uint64_t old_sp = env->gpr[LINX_REG_SP];
    const uint64_t new_sp = old_sp + adj;
    uint32_t regs[LINX_GPR_COUNT];
    uint64_t addrs[LINX_GPR_COUNT];
    uint64_t values[LINX_GPR_COUNT];
    const int restore_count =
        linx_frame_restore_prepare(env, stacksize, new_sp, restore_base,
                                   begin, end, regs, addrs, values);

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
    HELPER(linx_check_bstart_target)(env, ra);
    linx_template_clear(env);
    env->pc = ra;
    linx_template_commit_and_exit(env, cs, env->pc);
}

void HELPER(linx_template_fret_ra)(CPULinxState *env, uint64_t cur_pc,
                                   uint64_t next_pc, uint32_t reg_begin,
                                   uint32_t reg_end, uint64_t stacksize)
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
    const int restore_count =
        linx_frame_restore_prepare(env, stacksize, new_sp, restore_base,
                                   begin, end, regs, addrs, values);

    if (adj) {
        env->gpr[LINX_REG_SP] = new_sp;
        linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
    }

    linx_frame_restore_commit(env, cur_pc, regs, addrs, values,
                              restore_count);

    HELPER(linx_check_bstart_target)(env, retRa);
    linx_template_clear(env);
    env->pc = retRa;
    linx_template_commit_and_exit(env, cs, env->pc);
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

static inline bool linx_tile_get_elem_from_words(const uint32_t *words,
                                                 uint32_t elem_idx,
                                                 unsigned elem_bytes,
                                                 uint32_t *value_out)
{
    const uint8_t *buf = (const uint8_t *)words;
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

static inline void linx_tile_record_shape(CPULinxState *env, unsigned tile,
                                          uint32_t rows, uint32_t cols)
{
    if (tile >= 32) {
        return;
    }
    env->tile_reg_rows[tile] = rows;
    env->tile_reg_cols[tile] = cols;
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
                           size_t bytes)
{
    if (dst_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (bytes == 0 || bytes > LINX_TILE_MAX_BYTES) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
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
    if ((fmt.dst == LINX_TILE_LAYOUT_NZ || fmt.dst == LINX_TILE_LAYOUT_ZN) &&
        (((uint64_t)tr_inner * elem_bytes) % 32u != 0u || (tr_outer % 16u) != 0u)) {
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
                value = linx_tile_mem_read(env, base + (uint64_t)src_idx * elem_bytes, elem_bytes);
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
    linx_tile_record_shape(env, dst_tile,
                           (uint32_t)(env->lb[0] & 0xffffffffu),
                           (uint32_t)(env->lb[1] & 0xffffffffu));
}

static void linx_tile_store(CPULinxState *env, unsigned src_tile, unsigned addr_reg,
                            size_t bytes)
{
    if (src_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (bytes == 0 || bytes > LINX_TILE_MAX_BYTES) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
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
    if ((fmt.src == LINX_TILE_LAYOUT_NZ || fmt.src == LINX_TILE_LAYOUT_ZN) &&
        (((uint64_t)tr_inner * elem_bytes) % 32u != 0u || (tr_outer % 16u) != 0u)) {
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
            linx_tile_mem_write(env, base + (uint64_t)dst_idx * elem_bytes, elem_bytes, value);
        }
    }
}

static void linx_tile_mamulb(CPULinxState *env, unsigned src_a, unsigned src_b,
                             unsigned acc_tile, unsigned dst_tile, size_t bytes,
                             bool accumulate)
{
    if (src_a >= 32 || src_b >= 32 || dst_tile >= 32 ||
        (accumulate && acc_tile >= 32)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned m = env->lb[0] ? MIN((unsigned)env->lb[0], 8u) : 8u;
    const unsigned n = env->lb[1] ? MIN((unsigned)env->lb[1], 8u) : 8u;
    const unsigned kdim = env->lb[2] ? MIN((unsigned)env->lb[2], 8u) : 8u;

    if (bytes == 0 || bytes > LINX_TILE_MAX_BYTES || (bytes & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const unsigned words = (unsigned)(bytes / 4u);
    const unsigned lhs_required_words = (m - 1u) * 8u + kdim;
    const unsigned rhs_required_words = (kdim - 1u) * 8u + n;
    const unsigned out_required_words = (m - 1u) * 8u + n;
    if (words < out_required_words ||
        env->tile_reg_bytes[src_a] < lhs_required_words * 4u ||
        env->tile_reg_bytes[src_b] < rhs_required_words * 4u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (accumulate) {
        if (env->tile_reg_bytes[acc_tile] < bytes) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }

    uint32_t lhs[64];
    uint32_t rhs[64];
    uint32_t acc_seed[64];
    for (unsigned i = 0; i < 64; i++) {
        lhs[i] = env->tile_reg[src_a][i];
        rhs[i] = env->tile_reg[src_b][i];
        acc_seed[i] = accumulate ? env->tile_reg[acc_tile][i] : 0u;
    }

    env->tile_acc_bytes = (uint32_t)bytes;
    env->tile_reg_bytes[dst_tile] = (uint32_t)bytes;
    linx_tile_record_shape(env, dst_tile, m, n);

    if (accumulate) {
        for (unsigned i = 0; i < words && i < LINX_TILE_MAX_WORDS; i++) {
            env->tile_acc[i] = env->tile_reg[acc_tile][i];
            env->tile_reg[dst_tile][i] = env->tile_reg[acc_tile][i];
        }
    }

    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            int64_t acc = accumulate
                              ? (int64_t)(int32_t)acc_seed[i * 8u + j]
                              : 0;
            for (unsigned k = 0; k < kdim; k++) {
                const int32_t a = (int32_t)lhs[i * 8u + k];
                const int32_t b = (int32_t)rhs[k * 8u + j];
                acc += (int64_t)a * (int64_t)b;
            }
            env->tile_acc[i * 8u + j] = (uint32_t)(int32_t)acc;
            env->tile_reg[dst_tile][i * 8u + j] = (uint32_t)(int32_t)acc;
        }
    }

    if (!accumulate) {
        /* Zero the rest of the accumulator for determinism. */
        for (unsigned i = 0; i < 8; i++) {
            for (unsigned j = 0; j < 8; j++) {
                if (i < m && j < n) {
                    continue;
                }
                env->tile_acc[i * 8u + j] = 0;
                env->tile_reg[dst_tile][i * 8u + j] = 0;
            }
        }
        for (unsigned i = 64; i < words && i < LINX_TILE_MAX_WORDS; i++) {
            env->tile_acc[i] = 0;
            env->tile_reg[dst_tile][i] = 0;
        }
    }
}

static inline bool linx_tile_region_fits(uint32_t rows, uint32_t cols,
                                         unsigned elem_bytes, size_t bytes)
{
    if (rows == 0u || cols == 0u || elem_bytes == 0u) {
        return false;
    }
    return (uint64_t)rows * (uint64_t)cols * (uint64_t)elem_bytes <= bytes;
}

static void linx_tile_fixp_tmov(CPULinxState *env)
{
    if (env->tile_itp_count == 0 || env->tile_ota_count == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const LinxTileITPDesc src = linx_tile_decode_itp(env->tile_itp_desc[0]);
    const LinxTileOTADesc dst = linx_tile_decode_ota(env->tile_ota_desc[0]);
    if (!linx_tile_valid_arch_id(src.src0) || !linx_tile_valid_arch_id(dst.dst)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned src_tile = linx_tile_storage_index(src.src0);
    const unsigned dst_tile = linx_tile_storage_index(dst.dst);
    const size_t bytes = (size_t)linx_tile_bytes_from_ota(&dst);
    if (bytes == 0 || bytes > LINX_TILE_MAX_BYTES ||
        env->tile_reg_bytes[src_tile] < bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    memcpy(env->tile_reg[dst_tile], env->tile_reg[src_tile], bytes);
    if (bytes < LINX_TILE_MAX_BYTES) {
        memset(((uint8_t *)env->tile_reg[dst_tile]) + bytes, 0,
               LINX_TILE_MAX_BYTES - bytes);
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)bytes;
    linx_tile_record_shape(env, dst_tile, env->tile_reg_rows[src_tile],
                           env->tile_reg_cols[src_tile]);
}

static void linx_tile_fixp_tinsert(CPULinxState *env)
{
    if (env->tile_itp_count == 0 || env->tile_ota_count == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const LinxTileITPDesc src = linx_tile_decode_itp(env->tile_itp_desc[0]);
    const LinxTileOTADesc dst = linx_tile_decode_ota(env->tile_ota_desc[0]);
    if (!linx_tile_valid_arch_id(src.src0) ||
        !linx_tile_valid_arch_id(src.src1) ||
        !linx_tile_valid_arch_id(dst.dst)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned base_tile = linx_tile_storage_index(src.src0);
    const unsigned insert_tile = linx_tile_storage_index(src.src1);
    const unsigned dst_tile = linx_tile_storage_index(dst.dst);

    const uint32_t dst_rows = (uint32_t)(env->lb[0] & 0xffffffffu);
    const uint32_t dst_cols = (uint32_t)(env->lb[1] & 0xffffffffu);
    uint32_t src_rows = (uint32_t)((env->lb[2] >> 16) & 0xffffu);
    uint32_t src_cols = (uint32_t)(env->lb[2] & 0xffffu);
    if (src_rows == 0u && env->tile_reg_rows[insert_tile] != 0u) {
        src_rows = env->tile_reg_rows[insert_tile];
    }
    if (src_cols == 0u && env->tile_reg_cols[insert_tile] != 0u) {
        src_cols = env->tile_reg_cols[insert_tile];
    }
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(env->tile_dtype);
    const size_t dst_bytes = (size_t)linx_tile_bytes_from_ota(&dst);
    const size_t src_bytes =
        (size_t)src_rows * (size_t)src_cols * (size_t)elem_bytes;
    const uint32_t row = (uint32_t)(env->tile_meta_value >> 32);
    const uint32_t col = (uint32_t)(env->tile_meta_value & 0xffffffffu);

    if (!env->tile_meta_valid || env->tile_meta_mode != 0u ||
        !linx_tile_region_fits(dst_rows, dst_cols, elem_bytes, dst_bytes) ||
        !linx_tile_region_fits(src_rows, src_cols, elem_bytes, src_bytes) ||
        row > dst_rows || col > dst_cols ||
        src_rows > dst_rows - row || src_cols > dst_cols - col) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (env->tile_reg_bytes[base_tile] < dst_bytes ||
        env->tile_reg_bytes[insert_tile] < src_bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    memcpy(env->tile_reg[dst_tile], env->tile_reg[base_tile], dst_bytes);
    if (dst_bytes < LINX_TILE_MAX_BYTES) {
        memset(((uint8_t *)env->tile_reg[dst_tile]) + dst_bytes, 0,
               LINX_TILE_MAX_BYTES - dst_bytes);
    }

    for (uint32_t r = 0; r < src_rows; r++) {
        for (uint32_t c = 0; c < src_cols; c++) {
            uint32_t value = 0;
            const uint32_t src_idx = r * src_cols + c;
            const uint32_t dst_idx = (row + r) * dst_cols + (col + c);
            if (!linx_tile_get_elem(env, insert_tile, src_idx, elem_bytes, &value) ||
                !linx_tile_set_elem(env, dst_tile, dst_idx, elem_bytes, value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
        }
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)dst_bytes;
    linx_tile_record_shape(env, dst_tile, dst_rows, dst_cols);
}

static void linx_tile_fixp_ttrans(CPULinxState *env)
{
    if (env->tile_itp_count == 0 || env->tile_ota_count == 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const LinxTileITPDesc src = linx_tile_decode_itp(env->tile_itp_desc[0]);
    const LinxTileOTADesc dst = linx_tile_decode_ota(env->tile_ota_desc[0]);
    if (!linx_tile_valid_arch_id(src.src0) || !linx_tile_valid_arch_id(dst.dst)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned src_tile = linx_tile_storage_index(src.src0);
    const unsigned dst_tile = linx_tile_storage_index(dst.dst);

    uint32_t src_rows = (uint32_t)(env->lb[0] & 0xffffffffu);
    uint32_t src_cols = (uint32_t)(env->lb[1] & 0xffffffffu);
    uint32_t dst_rows = (uint32_t)((env->lb[2] >> 16) & 0xffffu);
    uint32_t dst_cols = (uint32_t)(env->lb[2] & 0xffffu);
    if (src_rows == 0u && env->tile_reg_rows[src_tile] != 0u) {
        src_rows = env->tile_reg_rows[src_tile];
    }
    if (src_cols == 0u && env->tile_reg_cols[src_tile] != 0u) {
        src_cols = env->tile_reg_cols[src_tile];
    }
    if (dst_cols == 0u) {
        dst_cols = src_rows;
    }
    if (dst_rows == 0u) {
        dst_rows = src_cols;
    }
    const unsigned elem_bytes = linx_tile_dtype_elem_bytes(env->tile_dtype);
    const size_t dst_bytes = (size_t)linx_tile_bytes_from_ota(&dst);
    const size_t src_bytes =
        (size_t)src_rows * (size_t)src_cols * (size_t)elem_bytes;

    if (!linx_tile_region_fits(src_rows, src_cols, elem_bytes, src_bytes) ||
        !linx_tile_region_fits(dst_rows, dst_cols, elem_bytes, dst_bytes) ||
        dst_rows < src_cols || dst_cols < src_rows) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (env->tile_reg_bytes[src_tile] < src_bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    uint32_t src_shadow[LINX_TILE_MAX_WORDS];
    const uint32_t *src_words = env->tile_reg[src_tile];
    if (src_tile == dst_tile) {
        memcpy(src_shadow, env->tile_reg[src_tile], LINX_TILE_MAX_BYTES);
        src_words = src_shadow;
    }

    memset(env->tile_reg[dst_tile], 0, LINX_TILE_MAX_BYTES);
    for (uint32_t r = 0; r < src_rows; r++) {
        for (uint32_t c = 0; c < src_cols; c++) {
            uint32_t value = 0;
            const uint32_t src_idx = r * src_cols + c;
            const uint32_t dst_idx = c * dst_cols + r;
            if (!linx_tile_get_elem_from_words(src_words, src_idx, elem_bytes, &value) ||
                !linx_tile_set_elem(env, dst_tile, dst_idx, elem_bytes, value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
        }
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)dst_bytes;
    linx_tile_record_shape(env, dst_tile, dst_rows, dst_cols);
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
        const unsigned src0 = (desc >> 5) & 0x1fu;  /* RegSrc0 */
        const unsigned src1 = (desc >> 10) & 0x1fu; /* RegSrc1 */
        const unsigned src2 = (desc >> 15) & 0x1fu; /* RegSrc2 */
        const unsigned dst = desc & 0x1fu;          /* RegDst */

        /*
         * Bring-up launcher streams treat RI bindings as the authored B.IOR
         * operand-list order across descriptors.
         */
        const unsigned srcs[4] = { src0, src1, src2, dst };
        for (unsigned s = 0; s < 4; s++) {
            const unsigned reg = srcs[s];
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
        const unsigned src0 = (desc >> 5) & 0x1fu;  /* RegSrc0 */
        const unsigned src1 = (desc >> 10) & 0x1fu; /* RegSrc1 */
        const unsigned src2 = (desc >> 15) & 0x1fu; /* RegSrc2 */
        const unsigned dst = desc & 0x1fu;          /* RegDst */
        const unsigned srcs[4] = { src0, src1, src2, dst };

        for (unsigned s = 0; s < 4; s++) {
            const unsigned reg = srcs[s];
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

void HELPER(linx_tile_reset_block)(CPULinxState *env)
{
    env->tile_arg_format = 0;
    env->tile_attr_pad = 0;
    env->tile_attr_dtype = 0;
    env->tile_ior_count = 0;
    env->vec_ri_count = 0;
    env->tile_itp_count = 0;
    env->tile_ota_count = 0;
    env->tile_meta_valid = 0;
    env->tile_meta_mode = 0;
    env->tile_meta_value = 0;
    env->tile_desc_valid = 0;
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

void HELPER(linx_tile_append_itp)(CPULinxState *env, uint64_t packed)
{
    const LinxTileITPDesc desc = linx_tile_decode_itp(packed);

    if ((desc.src0 == 0u && desc.s0r) || (desc.src1 == 0u && desc.s1r)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((desc.src0 != 0u && !linx_tile_valid_arch_id(desc.src0)) ||
        (desc.src1 != 0u && !linx_tile_valid_arch_id(desc.src1))) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (env->tile_itp_count >= LINX_TILE_MAX_ITP) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    env->tile_desc_valid = 1;
    env->tile_itp_desc[env->tile_itp_count++] = packed;
}

void HELPER(linx_tile_append_ota)(CPULinxState *env, uint64_t packed)
{
    const LinxTileOTADesc desc = linx_tile_decode_ota(packed);
    const uint64_t bytes64 = linx_tile_bytes_from_ota(&desc);

    if (!linx_tile_valid_arch_id(desc.dst) ||
        bytes64 == 0 || bytes64 > LINX_TILE_MAX_BYTES ||
        (bytes64 & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (env->tile_ota_count >= LINX_TILE_MAX_OTA) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    env->tile_desc_valid = 1;
    env->tile_ota_desc[env->tile_ota_count++] = packed;
}

void HELPER(linx_tile_set_meta)(CPULinxState *env, uint64_t value, uint32_t mode)
{
    if ((mode & ~1u) != 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    env->tile_desc_valid = 1;
    env->tile_meta_valid = 1;
    env->tile_meta_mode = mode & 1u;
    env->tile_meta_value = value;
}

void HELPER(linx_tile_commit)(CPULinxState *env)
{
    if (env->tile_itp_count == 0 && env->tile_ota_count == 0 &&
        env->tile_desc_valid == 0) {
        return;
    }

    switch (env->blocktype) {
    case LINX_BLOCK_TMA:
        switch (env->tile_func) {
        case LINX_TMA_TLOAD: {
            if (env->tile_ota_count == 0) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            unsigned addr_reg = 0;
            if (!linx_tile_get_base_reg(env, &addr_reg)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            for (unsigned i = 0; i < env->tile_ota_count; i++) {
                const LinxTileOTADesc d =
                    linx_tile_decode_ota(env->tile_ota_desc[i]);
                if (!linx_tile_valid_arch_id(d.dst)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_load(env, linx_tile_storage_index(d.dst), addr_reg,
                               (size_t)linx_tile_bytes_from_ota(&d));
            }
            break;
        }
        case LINX_TMA_TSTORE: {
            if (env->tile_itp_count == 0) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            unsigned addr_reg = 0;
            if (!linx_tile_get_base_reg(env, &addr_reg)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            for (unsigned i = 0; i < env->tile_itp_count; i++) {
                const LinxTileITPDesc d =
                    linx_tile_decode_itp(env->tile_itp_desc[i]);
                const uint32_t srcs[2] = { d.src0, d.src1 };
                for (unsigned s = 0; s < 2; s++) {
                    if (srcs[s] == 0u) {
                        continue;
                    }
                    if (!linx_tile_valid_arch_id(srcs[s])) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                    const unsigned src_tile = linx_tile_storage_index(srcs[s]);
                    const size_t bytes = env->tile_reg_bytes[src_tile];
                    linx_tile_store(env, src_tile, addr_reg, bytes);
                }
            }
            break;
        }
        case LINX_TMA_TPREFETCH:
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    case LINX_BLOCK_CUBE:
        switch (env->tile_func) {
        case LINX_CUBE_TMATMUL: {
            if (env->tile_itp_count == 0 || env->tile_ota_count == 0) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            const bool accumulate = (env->tile_arg_format & 0x1fu) == 1u;
            if ((env->tile_arg_format & 0x1fu) > 1u) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            if (accumulate && env->tile_itp_count < 2) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            const LinxTileITPDesc src =
                linx_tile_decode_itp(env->tile_itp_desc[0]);
            const LinxTileITPDesc acc =
                accumulate ? linx_tile_decode_itp(env->tile_itp_desc[1])
                           : (LinxTileITPDesc){ 0 };
            const LinxTileOTADesc dst =
                linx_tile_decode_ota(env->tile_ota_desc[0]);
            if (!linx_tile_valid_arch_id(src.src0) ||
                !linx_tile_valid_arch_id(src.src1) ||
                (accumulate && !linx_tile_valid_arch_id(acc.src0)) ||
                !linx_tile_valid_arch_id(dst.dst)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            linx_tile_mamulb(env, linx_tile_storage_index(src.src0),
                             linx_tile_storage_index(src.src1),
                             accumulate ? linx_tile_storage_index(acc.src0) : 0u,
                             linx_tile_storage_index(dst.dst),
                             (size_t)linx_tile_bytes_from_ota(&dst),
                             accumulate);
            break;
        }
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    case LINX_BLOCK_FIXP:
        switch (env->tile_func) {
        case LINX_FIXP_TMOV:
            linx_tile_fixp_tmov(env);
            break;
        case LINX_FIXP_TINSERT:
            linx_tile_fixp_tinsert(env);
            break;
        case LINX_FIXP_TTRANS:
            linx_tile_fixp_ttrans(env);
            break;
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    default:
        /* Non-tile blocks: nothing to do. */
        break;
    }

    /* Consume the per-block descriptor. */
    env->tile_desc_valid = 0;
    env->tile_arg_format = 0;
    env->tile_attr_pad = 0;
    env->tile_attr_dtype = 0;
    env->tile_ior_count = 0;
    env->tile_itp_count = 0;
    env->tile_ota_count = 0;
    env->tile_meta_valid = 0;
    env->tile_meta_mode = 0;
    env->tile_meta_value = 0;
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

static uint32_t linx_vec_normalize_dst_code(uint32_t raw)
{
    if (linx_vec_is_canonical_dst_code(raw)) {
        return raw;
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
     * v0.57 mapping:
     * - TA/TB/TC/TD: first four non-TZERO B.ITP source tiles in header order.
     * - TO/TS: first two B.OTA destination tiles in header order.
     */
    unsigned inputs[4];
    unsigned outputs[2];
    unsigned input_count = 0;
    unsigned output_count = 0;

    for (unsigned i = 0; i < env->tile_itp_count; i++) {
        const LinxTileITPDesc d =
            linx_tile_decode_itp(env->tile_itp_desc[i]);
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local B.ITP[%u]: src0=%u src1=%u last=%u"
                          " src_pair=%u reuse=%u/%u body_tpc=0x%" PRIx64 "\n",
                          i, d.src0, d.src1, d.last, d.src_pair,
                          d.s0r, d.s1r, env->body_tpc);
        }
        if (d.src0 != 0u && linx_tile_valid_arch_id(d.src0) &&
            input_count < 4) {
            inputs[input_count++] = linx_tile_storage_index(d.src0);
        }
        if (d.src1 != 0u && linx_tile_valid_arch_id(d.src1) &&
            input_count < 4) {
            inputs[input_count++] = linx_tile_storage_index(d.src1);
        }
    }

    for (unsigned i = 0; i < env->tile_ota_count; i++) {
        const LinxTileOTADesc d =
            linx_tile_decode_ota(env->tile_ota_desc[i]);
        if (linx_debug_local_enabled_p()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx local B.OTA[%u]: dst=%u cell_count_m1=%u"
                          " last=%u dst_slot=%u body_tpc=0x%" PRIx64 "\n",
                          i, d.dst, d.cell_count_m1, d.last, d.dst_slot,
                          env->body_tpc);
        }
        if (linx_tile_valid_arch_id(d.dst) && output_count < 2) {
            outputs[output_count++] = linx_tile_storage_index(d.dst);
        }
    }

    if (base_idx < 4) {
        if (base_idx < input_count) {
            *tile_out = inputs[base_idx];
            return true;
        }
        return false;
    }
    if (base_idx == 4) { /* TO */
        if (output_count >= 1) {
            *tile_out = outputs[0];
            return true;
        }
        return false;
    }
    if (base_idx == 5) { /* TS */
        if (output_count >= 2) {
            *tile_out = outputs[1];
            return true;
        }
        return false;
    }
    return false;
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
    dst = linx_vec_normalize_dst_code(dst);

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
                          "Linx local: unresolved tile base code=0x%x idx=%u body_tpc=0x%" PRIx64 " itp_count=%u ota_count=%u valid=%u lc0=%" PRIu64 " lc1=%" PRIu64 "\n",
                          base_code, idx, env->body_tpc, env->tile_itp_count,
                          env->tile_ota_count, env->tile_desc_valid,
                          env->lc[0], env->lc[1]);
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
    const size_t slot = (size_t)((target >> 1) % LINX_BSTART_CACHE_SIZE);

    if (linx_cfi_trace_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: indirect target check pc=0x%" PRIx64
                      " target=0x%" PRIx64 "\n",
                      env->pc, target);
    }

    if (linx_is_call_continuation(env, target)) {
        return;
    }

    /*
     * This helper is on the hot path for indirect control flow (RET/IND/ICALL
     * and template returns). Cache the most recently-validated targets to avoid
     * re-reading guest memory for tight call/return loops.
     *
     * MMU programming, TLB invalidation, CSTATE/ACR switches, and ACRE/trap
     * transitions reset this cache. Self-modifying-code debug can opt back into
     * old revalidate-on-hit behavior with LINX_BSTART_CACHE_REVALIDATE=1.
     */
    if (env->bstart_cache_valid[slot] && env->bstart_cache_tag[slot] == target) {
        if (!linx_bstart_cache_revalidate_enabled_p()) {
            return;
        }
        if (linx_is_bstart_at_addr(env, target)) {
            return;
        }
        env->bstart_cache_valid[slot] = 0;
    }

    if (linx_is_call_fallthrough_target(env, env->pc, target)) {
        return;
    }

    if (linx_is_bstart_at_addr(env, target)) {
        env->bstart_cache_tag[slot] = target;
        env->bstart_cache_valid[slot] = 1;
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
            return;
        }
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
