/*
 * LinxISA translation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translator.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "exec/log.h"
#include "trace.h"
#include "opcode_meta.h"
#include "tile_isa_058.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

typedef struct DisasContext {
    DisasContextBase base;
    CPULinxState *env;

    uint8_t brtype;
    vaddr brtarget;
    uint32_t cur_insn_len;
    uint64_t cur_insn_raw;
    bool in_body;
    bool decoupled_header;
    bool tgt_modified;
    bool ra_set;
    vaddr call_ra_target;
    uint32_t block_insn_index;
    int mem_idx;
} DisasContext;

enum {
    LINX_BR_FALL   = 1,
    LINX_BR_DIRECT = 2,
    LINX_BR_COND   = 3,
    LINX_BR_CALL   = 4,
    LINX_BR_IND    = 5,
    LINX_BR_ICALL  = 6,
    LINX_BR_RET    = 7,
};

static TCGv_i64 cpu_gpr[LINX_GPR_COUNT];
static TCGv_i64 cpu_tq[4];
static TCGv_i64 cpu_uq[4];
static TCGv_i64 cpu_bpc;
static TCGv_i64 cpu_tgt;
static TCGv_i32 cpu_cond;
static TCGv_i64 cpu_vec_p;
static TCGv_i32 cpu_carg;  /* Commit argument flag */
static TCGv_i32 cpu_brtype;
static TCGv_i32 cpu_blocktype;
static TCGv_i64 cpu_body_tpc;
static TCGv_i64 cpu_body_end;
static TCGv_i64 cpu_return_pc;
static TCGv_i32 cpu_in_body;
static TCGv_i32 cpu_tile_func;
static TCGv_i32 cpu_tile_dtype;
static TCGv_i32 cpu_tile_iot_valid;
static TCGv_i32 cpu_tile_iot_flags;
static TCGv_i32 cpu_tile_iot_dst;
static TCGv_i32 cpu_tile_iot_grp;
static TCGv_i32 cpu_tile_iot_src0;
static TCGv_i32 cpu_tile_iot_src1;
static TCGv_i32 cpu_tile_iot_reg;
static TCGv_i32 cpu_tile_iot_size;
static TCGv_i32 cpu_tile_attr_raw;
static TCGv_i32 cpu_tile_attr_pad;
static TCGv_i32 cpu_tile_attr_dtype;
static TCGv_i64 cpu_lb[3];
static TCGv_i64 cpu_pc;
static TCGv_i64 cpu_insn_pc_next;
static TCGv_i64 cpu_insn_count;
static TCGv_i64 cpu_heartbeat_next_count;
static TCGv_i64 cpu_pending_trap_arg0;
static TCGv_i32 cpu_pending_trap_cause;

/* Commit-trace scratch (JSONL). */
static TCGv_i64 cpu_trace_pc;
static TCGv_i64 cpu_trace_insn;
static TCGv_i32 cpu_trace_len;
static TCGv_i32 cpu_trace_wb_valid;
static TCGv_i32 cpu_trace_wb_rd;
static TCGv_i64 cpu_trace_wb_data;
static TCGv_i32 cpu_trace_mem_valid;
static TCGv_i32 cpu_trace_mem_is_store;
static TCGv_i64 cpu_trace_mem_addr;
static TCGv_i64 cpu_trace_mem_wdata;
static TCGv_i64 cpu_trace_mem_rdata;
static TCGv_i32 cpu_trace_mem_size;
static TCGv_i32 cpu_trace_trap_valid;
static TCGv_i32 cpu_trace_trap_cause;
static TCGv_i64 cpu_trace_traparg0;

static bool linx_commit_trace_enabled;
static bool linx_opcode_meta_strict = true;
static bool linx_pc_sample_enabled;
static bool linx_heartbeat_enabled;
static bool linx_call_trace_translate_enabled;
static bool linx_mem_trace_translate_enabled;
static bool linx_mem_trace_translate_loads = true;
static bool linx_mem_trace_translate_stores = true;
static bool linx_mem_trace_translate_fast_enabled;
static bool linx_mem_trace_translate_addr_filter_enabled;
static uint64_t linx_mem_trace_translate_addr;
static uint64_t linx_mem_trace_translate_end;
static bool linx_mem_trace_translate_pc_filter_enabled;
static uint64_t linx_mem_trace_translate_pc_lo;
static uint64_t linx_mem_trace_translate_pc_hi;
static bool linx_mem_trace_translate_pre_enabled;
static bool linx_debug_local_inited;
static bool linx_debug_local_enabled;
static bool linx_bstart_inline_cache_inited;
static bool linx_bstart_inline_cache_enabled;
static bool linx_template_chain_enabled;
static bool linx_host_insn_hook_inited;
static bool linx_host_insn_hook_global_enabled;
static bool linx_host_insn_hook_pc_watch_requested;
static bool linx_host_insn_hook_queue_trace_requested;
static bool linx_translate_queue_trace_inited;
static bool linx_translate_queue_trace_enabled;
static bool linx_translate_queue_trace_pc_filter_enabled;
static uint64_t linx_translate_queue_trace_pc_lo;
static uint64_t linx_translate_queue_trace_pc_hi = UINT64_MAX;
#define LINX_TRANSLATE_PC_WATCH_MAX 16
static bool linx_translate_pc_watch_inited;
static unsigned linx_translate_pc_watch_count;
static uint64_t linx_translate_pc_watch[LINX_TRANSLATE_PC_WATCH_MAX];

enum {
    LINX_CALL_TRACE_SETRET = 1,
    LINX_CALL_TRACE_CALL_COMMIT = 2,
};

static inline bool linx_debug_local_enabled_p(void)
{
    if (!linx_debug_local_inited) {
        const char *v = getenv("LINX_DEBUG_LOCAL");
        linx_debug_local_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_debug_local_inited = true;
    }
    return linx_debug_local_enabled;
}

static inline bool linx_translate_env_enabled(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

static inline bool linx_bstart_inline_cache_enabled_p(void)
{
    if (!linx_bstart_inline_cache_inited) {
        const char *v = getenv("LINX_BSTART_INLINE_CACHE");
        const bool explicitly_disabled = v && v[0] && strcmp(v, "0") == 0;

        linx_bstart_inline_cache_enabled =
            !explicitly_disabled &&
            !linx_translate_env_enabled("LINX_CFI_TRACE") &&
            !linx_translate_env_enabled("LINX_BSTART_CACHE_REVALIDATE") &&
            !linx_translate_env_enabled("LINX_BSTART_CACHE_STATS");
        linx_bstart_inline_cache_inited = true;
    }
    return linx_bstart_inline_cache_enabled;
}

static bool linx_translate_parse_u64(const char *s, uint64_t *out)
{
    char *endp = NULL;

    if (!s || !s[0]) {
        return false;
    }
    errno = 0;
    *out = strtoull(s, &endp, 0);
    return errno == 0 && endp && *endp == '\0';
}

static void linx_translate_pc_watch_init(void)
{
    char *copy;
    char *saveptr = NULL;
    char *tok;
    const char *watch;

    if (linx_translate_pc_watch_inited) {
        return;
    }
    linx_translate_pc_watch_inited = true;

    watch = getenv("LINX_DEBUG_PC_WATCH");
    if (!watch || !watch[0] || strcmp(watch, "0") == 0) {
        return;
    }

    copy = g_strdup(watch);
    for (tok = strtok_r(copy, ",", &saveptr);
         tok && linx_translate_pc_watch_count < LINX_TRANSLATE_PC_WATCH_MAX;
         tok = strtok_r(NULL, ",", &saveptr)) {
        uint64_t pc;
        char *trimmed = g_strstrip(tok);
        if (linx_translate_parse_u64(trimmed, &pc)) {
            linx_translate_pc_watch[linx_translate_pc_watch_count++] = pc;
        }
    }
    g_free(copy);
}

static bool linx_translate_pc_watch_matches(vaddr pc)
{
    linx_translate_pc_watch_init();
    for (unsigned i = 0; i < linx_translate_pc_watch_count; i++) {
        if (linx_translate_pc_watch[i] == pc) {
            return true;
        }
    }
    return false;
}

static void linx_translate_queue_trace_init(void)
{
    const char *enabled_s;
    const char *lo_s;
    const char *hi_s;
    const char *value_s;
    uint64_t lo = 0;
    uint64_t hi = UINT64_MAX;

    if (linx_translate_queue_trace_inited) {
        return;
    }
    linx_translate_queue_trace_inited = true;

    enabled_s = getenv("LINX_QUEUE_TRACE");
    if (!enabled_s || !enabled_s[0] || strcmp(enabled_s, "0") == 0) {
        enabled_s = getenv("LINX_QEMU_QUEUE_TRACE");
    }
    linx_translate_queue_trace_enabled =
        enabled_s && enabled_s[0] && strcmp(enabled_s, "0") != 0;
    if (!linx_translate_queue_trace_enabled) {
        return;
    }

    lo_s = getenv("LINX_QUEUE_TRACE_PC_LO");
    if (!lo_s || !lo_s[0]) {
        lo_s = getenv("LINX_QEMU_QUEUE_TRACE_PC_LO");
    }
    hi_s = getenv("LINX_QUEUE_TRACE_PC_HI");
    if (!hi_s || !hi_s[0]) {
        hi_s = getenv("LINX_QEMU_QUEUE_TRACE_PC_HI");
    }
    const bool have_pc_lo = lo_s && linx_translate_parse_u64(lo_s, &lo);
    const bool have_pc_hi = hi_s && linx_translate_parse_u64(hi_s, &hi);
    if (have_pc_lo || have_pc_hi) {
        linx_translate_queue_trace_pc_lo = MIN(lo, hi);
        linx_translate_queue_trace_pc_hi = MAX(lo, hi);
        linx_translate_queue_trace_pc_filter_enabled = true;
    }

    value_s = getenv("LINX_QUEUE_TRACE_PC");
    if (!value_s || !value_s[0]) {
        value_s = getenv("LINX_QEMU_QUEUE_TRACE_PC");
    }
    if (value_s && linx_translate_parse_u64(value_s, &lo)) {
        linx_translate_queue_trace_pc_lo = lo;
        linx_translate_queue_trace_pc_hi = lo;
        linx_translate_queue_trace_pc_filter_enabled = true;
    }
}

static bool linx_translate_queue_trace_matches(vaddr pc)
{
    linx_translate_queue_trace_init();
    if (!linx_translate_queue_trace_enabled) {
        return false;
    }
    if (!linx_translate_queue_trace_pc_filter_enabled) {
        return true;
    }
    return pc >= linx_translate_queue_trace_pc_lo &&
           pc <= linx_translate_queue_trace_pc_hi;
}

static inline bool linx_host_insn_hook_enabled_p(vaddr pc)
{
    if (!linx_host_insn_hook_inited) {
        const char *cosim = getenv("LINX_COSIM_ENABLE");
        const char *pc_watch = getenv("LINX_DEBUG_PC_WATCH");
        const char *work_grab = getenv("LINX_DEBUG_WORK_GRAB");
        const char *queue_trace = getenv("LINX_QUEUE_TRACE");
        if (!queue_trace || !queue_trace[0] ||
            strcmp(queue_trace, "0") == 0) {
            queue_trace = getenv("LINX_QEMU_QUEUE_TRACE");
        }

        linx_host_insn_hook_global_enabled =
            (cosim && cosim[0] && strcmp(cosim, "0") != 0) ||
            (work_grab && work_grab[0] && strcmp(work_grab, "0") != 0);
        linx_host_insn_hook_pc_watch_requested =
            pc_watch && pc_watch[0] && strcmp(pc_watch, "0") != 0;
        linx_host_insn_hook_queue_trace_requested =
            queue_trace && queue_trace[0] && strcmp(queue_trace, "0") != 0;
        linx_host_insn_hook_inited = true;
    }
    return linx_host_insn_hook_global_enabled ||
           (linx_host_insn_hook_pc_watch_requested &&
            linx_translate_pc_watch_matches(pc)) ||
           (linx_host_insn_hook_queue_trace_requested &&
            linx_translate_queue_trace_matches(pc));
}

static void linx_gen_mem_trace_probe(bool is_store, bool pre_access, vaddr pc,
                                     TCGv_i64 addr, uint32_t size,
                                     TCGv_i64 value)
{
    if (linx_mem_trace_translate_pc_filter_enabled &&
        (pc < linx_mem_trace_translate_pc_lo ||
         pc > linx_mem_trace_translate_pc_hi)) {
        return;
    }

    if (linx_mem_trace_translate_fast_enabled) {
        TCGLabel *skip = gen_new_label();

        if (size > 1) {
            TCGv_i64 end = tcg_temp_new_i64();

            tcg_gen_addi_i64(end, addr, size - 1);
            tcg_gen_brcondi_i64(TCG_COND_LTU, end,
                                (int64_t)linx_mem_trace_translate_addr, skip);
        } else {
            tcg_gen_brcondi_i64(TCG_COND_LTU, addr,
                                (int64_t)linx_mem_trace_translate_addr, skip);
        }
        tcg_gen_brcondi_i64(TCG_COND_GTU, addr,
                            (int64_t)linx_mem_trace_translate_end, skip);
        if (is_store) {
            gen_helper_linx_mem_trace_store(tcg_env, tcg_constant_i64(pc),
                                            addr, tcg_constant_i32(size),
                                            value);
        } else if (pre_access) {
            gen_helper_linx_mem_trace_load_pre(tcg_env, tcg_constant_i64(pc),
                                               addr, tcg_constant_i32(size),
                                               value);
        } else {
            gen_helper_linx_mem_trace_load(tcg_env, tcg_constant_i64(pc),
                                           addr, tcg_constant_i32(size),
                                           value);
        }
        gen_set_label(skip);
        return;
    }

    if (is_store) {
        gen_helper_linx_mem_trace_store(tcg_env, tcg_constant_i64(pc), addr,
                                        tcg_constant_i32(size), value);
    } else if (pre_access) {
        gen_helper_linx_mem_trace_load_pre(tcg_env, tcg_constant_i64(pc), addr,
                                           tcg_constant_i32(size), value);
    } else {
        gen_helper_linx_mem_trace_load(tcg_env, tcg_constant_i64(pc), addr,
                                       tcg_constant_i32(size), value);
    }
}

static unsigned linx_insn_len(uint16_t hw);
static bool linx_illegal(DisasContext *ctx);
static bool linx_block_fault(DisasContext *ctx, uint32_t legacy_cause, uint64_t arg0);
static bool linx_setret_common(DisasContext *ctx, int64_t imm_hw);

/*
 * The autogenerated decodetree field for 64-bit L.BSTART immediates currently
 * does not invert the R_LINX_L_BSTART42_PCREL packing correctly. Decode
 * the signed halfword displacement directly from the raw instruction so the
 * existing PC-relative target helper keeps the final byte offset correct.
 */
static inline int64_t linx_decode_l_bstart_simm42(uint64_t insn)
{
    uint64_t high17_raw = (insn >> 47) & 0x1ffffULL;
    uint64_t low25 = (insn >> 7) & 0x1ffffffULL;
    int64_t high17 = (int64_t)high17_raw;

    if (high17_raw & (1ULL << 16)) {
        high17 -= INT64_C(1) << 17;
    }

    return high17 * (INT64_C(1) << 25) + (int64_t)low25;
}

static void linx_emit_tile_iot_desc(DisasContext *ctx, uint32_t flags,
                                    uint32_t dst, uint32_t grp,
                                    uint32_t src0, uint32_t src1,
                                    uint32_t reg, uint32_t size,
                                    bool has_size)
{
    const uint64_t desc =
        ((uint64_t)(src0 & 0x3f) << 0) |
        ((uint64_t)(src1 & 0x3f) << 6) |
        ((uint64_t)(dst & 0x7) << 12) |
        /* `grp` is the B.IOT L/last bit, not either source's hand bit. */
        ((uint64_t)(grp & 0x1) << 15) |
        ((uint64_t)(flags & 0xf) << 16) |
        ((uint64_t)(reg & 0x1f) << 20) |
        ((uint64_t)(size & 0x1f) << 25) |
        ((uint64_t)(has_size ? 1u : 0u) << 30);

    tcg_gen_movi_i32(cpu_tile_iot_valid, 1);
    tcg_gen_movi_i32(cpu_tile_iot_flags, flags);
    tcg_gen_movi_i32(cpu_tile_iot_dst, dst & 0x7);
    tcg_gen_movi_i32(cpu_tile_iot_grp, grp & 0x1);
    tcg_gen_movi_i32(cpu_tile_iot_src0, src0 & 0x3f);
    tcg_gen_movi_i32(cpu_tile_iot_src1, src1 & 0x3f);
    tcg_gen_movi_i32(cpu_tile_iot_reg, reg & 0x1f);
    tcg_gen_movi_i32(cpu_tile_iot_size, has_size ? (size & 0x1f) : 0);

    gen_helper_linx_tile_append_iot(tcg_env, tcg_constant_i64(desc));
}

/*
 * Frame-template semantics use only the encoded stack size.  Keep this value
 * fixed so external environment state cannot alter guest-visible SP or slot
 * addresses.
 */
uint64_t linx_callframe_size = 0;

static inline MemOp linx_mo_endian(void)
{
    return MO_LE;
}

static bool linx_validate_opcode_meta(DisasContext *ctx, vaddr pc, uint64_t insn_raw, unsigned len)
{
    const LinxOpcodeMeta *meta = linx_opcode_meta_lookup(insn_raw, len);
    if (meta) {
        return true;
    }
    if (!linx_opcode_meta_strict) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: opcode metadata missing @ PC=0x%" VADDR_PRIx " len=%u insn=0x%016" PRIx64 "\n",
                  pc, len, insn_raw);
    linx_illegal(ctx);
    return false;
}

static inline void linx_lr_invalidate(void)
{
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(CPULinxState, lr_valid));
}

static inline void linx_tile_set_attr_const(uint32_t packed)
{
    tcg_gen_st_i32(tcg_constant_i32((int32_t)packed), tcg_env,
                   offsetof(CPULinxState, tile_attr_raw));
    tcg_gen_movi_i32(cpu_tile_attr_dtype, (packed >> 7) & 0x1fu);
    tcg_gen_movi_i32(cpu_tile_attr_pad, (packed >> 12) & 0x1fu);
}

static inline void linx_tile_reset_block_inline(void)
{
    gen_helper_linx_tile_reset_block(tcg_env);
}

static void linx_gen_check_bstart_target(DisasContext *ctx, TCGv_i64 target)
{
    if (!linx_bstart_inline_cache_enabled_p()) {
        gen_helper_linx_check_bstart_target(tcg_env, target);
        return;
    }

    TCGLabel *miss = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i64 key = tcg_temp_new_i64();
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 off = tcg_temp_new_i64();
    TCGv_i64 tag = tcg_temp_new_i64();
    TCGv_i32 valid = tcg_temp_new_i32();
    TCGv_i32 mmu_idx = tcg_temp_new_i32();
    TCGv_ptr ptr = tcg_temp_new_ptr();

    tcg_gen_shri_i64(key, target, 1);
    tcg_gen_shri_i64(tmp, key, 12);
    tcg_gen_xor_i64(key, key, tmp);
    tcg_gen_shri_i64(tmp, key, 24);
    tcg_gen_xor_i64(key, key, tmp);
    tcg_gen_andi_i64(key, key, LINX_BSTART_CACHE_SIZE - 1u);

    tcg_gen_addi_i64(off, key, offsetof(CPULinxState, bstart_cache_valid));
    tcg_gen_trunc_i64_ptr(ptr, off);
    tcg_gen_add_ptr(ptr, ptr, tcg_env);
    tcg_gen_ld8u_i32(valid, ptr, 0);
    tcg_gen_brcondi_i32(TCG_COND_EQ, valid, 0, miss);

    tcg_gen_shli_i64(off, key, 3);
    tcg_gen_addi_i64(off, off, offsetof(CPULinxState, bstart_cache_tag));
    tcg_gen_trunc_i64_ptr(ptr, off);
    tcg_gen_add_ptr(ptr, ptr, tcg_env);
    tcg_gen_ld_i64(tag, ptr, 0);
    tcg_gen_brcond_i64(TCG_COND_NE, tag, target, miss);

    tcg_gen_addi_i64(off, key, offsetof(CPULinxState, bstart_cache_mmu_idx));
    tcg_gen_trunc_i64_ptr(ptr, off);
    tcg_gen_add_ptr(ptr, ptr, tcg_env);
    tcg_gen_ld8u_i32(mmu_idx, ptr, 0);
    tcg_gen_brcondi_i32(TCG_COND_NE, mmu_idx, ctx->mem_idx, miss);

    tcg_gen_br(done);
    gen_set_label(miss);
    gen_helper_linx_check_bstart_target(tcg_env, target);
    gen_set_label(done);
}

static TCGv_i64 linx_get_reg(unsigned code)
{
    if (code == LINX_REG_ZERO) {
        return tcg_constant_i64(0);
    }
    if (code < LINX_GPR_COUNT) {
        return cpu_gpr[code];
    }
    if (!linx_debug_local_enabled_p()) {
        if (code < 28u) {
            return cpu_tq[code - 24u];
        }
        if (code < 32u) {
            return cpu_uq[code - 28u];
        }
    }

    TCGv_i64 tmp = tcg_temp_new_i64();
    gen_helper_linx_scalar_read_reg(tmp, tcg_env, tcg_constant_i32((int32_t)code));
    return tmp;
}

static void linx_push_t(TCGv_i64 v)
{
    if (linx_debug_local_enabled_p()) {
        gen_helper_linx_tq_push(tcg_env, v);
        return;
    }

    TCGv_i64 value = tcg_temp_new_i64();
    tcg_gen_mov_i64(value, v);
    tcg_gen_mov_i64(cpu_tq[3], cpu_tq[2]);
    tcg_gen_mov_i64(cpu_tq[2], cpu_tq[1]);
    tcg_gen_mov_i64(cpu_tq[1], cpu_tq[0]);
    tcg_gen_mov_i64(cpu_tq[0], value);
}

static void linx_push_u(TCGv_i64 v)
{
    if (linx_debug_local_enabled_p()) {
        gen_helper_linx_uq_push(tcg_env, v);
        return;
    }

    TCGv_i64 value = tcg_temp_new_i64();
    tcg_gen_mov_i64(value, v);
    tcg_gen_mov_i64(cpu_uq[3], cpu_uq[2]);
    tcg_gen_mov_i64(cpu_uq[2], cpu_uq[1]);
    tcg_gen_mov_i64(cpu_uq[1], cpu_uq[0]);
    tcg_gen_mov_i64(cpu_uq[0], value);
}

static void linx_trace_wb_dest(unsigned dst, TCGv_i64 v)
{
    if (linx_commit_trace_enabled) {
        tcg_gen_movi_i32(cpu_trace_wb_valid, 1);
        tcg_gen_movi_i32(cpu_trace_wb_rd, (int32_t)dst);
        tcg_gen_mov_i64(cpu_trace_wb_data, v);
    }
}

static void linx_set_dest(unsigned dst, TCGv_i64 v)
{
    if (dst == 0) {
        return;
    }
    if (dst == 31) {
        linx_push_t(v);
        linx_trace_wb_dest(dst, v);
        return;
    }
    if (dst == 30) {
        linx_push_u(v);
        linx_trace_wb_dest(dst, v);
        return;
    }
    if (dst < LINX_GPR_COUNT) {
        tcg_gen_mov_i64(cpu_gpr[dst], v);
        linx_trace_wb_dest(dst, v);
        return;
    }
    if (linx_debug_local_enabled_p()) {
        gen_helper_linx_scalar_write_reg(tcg_env, tcg_constant_i32((int32_t)dst), v);
        linx_trace_wb_dest(dst, v);
        return;
    }
    if (dst < 28u) {
        tcg_gen_mov_i64(cpu_tq[dst - 24u], v);
        linx_trace_wb_dest(dst, v);
        return;
    }
    if (dst < 30u) {
        tcg_gen_mov_i64(cpu_uq[dst - 28u], v);
        linx_trace_wb_dest(dst, v);
    }
}

static inline void linx_trace_begin(vaddr pc, uint64_t insn_raw, unsigned len)
{
    if (!linx_commit_trace_enabled) {
        return;
    }

    tcg_gen_movi_i64(cpu_trace_pc, pc);
    tcg_gen_movi_i64(cpu_trace_insn, insn_raw);
    tcg_gen_movi_i32(cpu_trace_len, (int32_t)len);

    tcg_gen_movi_i32(cpu_trace_wb_valid, 0);
    tcg_gen_movi_i32(cpu_trace_wb_rd, 0);
    tcg_gen_movi_i64(cpu_trace_wb_data, 0);

    tcg_gen_movi_i32(cpu_trace_mem_valid, 0);
    tcg_gen_movi_i32(cpu_trace_mem_is_store, 0);
    tcg_gen_movi_i64(cpu_trace_mem_addr, 0);
    tcg_gen_movi_i64(cpu_trace_mem_wdata, 0);
    tcg_gen_movi_i64(cpu_trace_mem_rdata, 0);
    tcg_gen_movi_i32(cpu_trace_mem_size, 0);

    tcg_gen_movi_i32(cpu_trace_trap_valid, 0);
    tcg_gen_movi_i32(cpu_trace_trap_cause, 0);
    tcg_gen_movi_i64(cpu_trace_traparg0, 0);

    gen_helper_linx_trace_operands_begin(tcg_env, tcg_constant_i64(insn_raw), tcg_constant_i32((int32_t)len));
}

static void linx_block_begin_common(DisasContext *ctx, uint8_t brtype,
                                    vaddr block_pc,
                                    vaddr initial_target,
                                    bool preserve_scalar_queues)
{
    int i;
    tcg_gen_movi_i64(cpu_bpc, block_pc);
    if (!preserve_scalar_queues) {
        for (i = 0; i < 4; i++) {
            tcg_gen_movi_i64(cpu_tq[i], 0);
            tcg_gen_movi_i64(cpu_uq[i], 0);
        }
    }
    tcg_gen_movi_i32(cpu_cond, 0);
    tcg_gen_movi_i32(cpu_carg, 0);
    tcg_gen_movi_i32(cpu_brtype, brtype);
    tcg_gen_movi_i32(cpu_blocktype, 0);
    tcg_gen_movi_i64(cpu_body_tpc, 0);
    tcg_gen_movi_i64(cpu_body_end, 0);
    tcg_gen_movi_i64(cpu_return_pc, 0);
    tcg_gen_movi_i32(cpu_in_body, 0);
    tcg_gen_movi_i32(cpu_tile_func, 0);
    tcg_gen_movi_i32(cpu_tile_dtype, 17); /* INT32 default in v0.3 DataType */
    tcg_gen_movi_i32(cpu_tile_iot_valid, 0);
    tcg_gen_movi_i32(cpu_tile_iot_flags, 0);
    tcg_gen_movi_i32(cpu_tile_iot_dst, 0);
    tcg_gen_movi_i32(cpu_tile_iot_grp, 0);
    tcg_gen_movi_i32(cpu_tile_iot_src0, 0);
    tcg_gen_movi_i32(cpu_tile_iot_src1, 0);
    tcg_gen_movi_i32(cpu_tile_iot_reg, 0);
    tcg_gen_movi_i32(cpu_tile_iot_size, 0);
    linx_tile_set_attr_const(0);
    linx_tile_reset_block_inline();
    tcg_gen_movi_i64(cpu_lb[0], 0);
    tcg_gen_movi_i64(cpu_lb[1], 0);
    tcg_gen_movi_i64(cpu_lb[2], 0);
    ctx->tgt_modified = brtype == LINX_BR_ICALL;
    ctx->decoupled_header = false;
    ctx->ra_set = false;
    ctx->call_ra_target = 0;
    ctx->block_insn_index = 0;
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(CPULinxState, call_ra_set));
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(CPULinxState, call_setret_pending));
    tcg_gen_st_i64(tcg_constant_i64(0), tcg_env,
                   offsetof(CPULinxState, fused_icall_target));
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(CPULinxState, fused_icall_target_valid));
    
    /* For COND blocks: set diverted target in bpc (cpu_tgt) */
    /* For DIRECT/CALL blocks: set target in bpc (cpu_tgt) */
    /* For FALL blocks: cpu_tgt remains 0 (unused) */
    if (brtype == LINX_BR_COND || brtype == LINX_BR_DIRECT || brtype == LINX_BR_CALL) {
        tcg_gen_movi_i64(cpu_tgt, initial_target);
    } else if (brtype != LINX_BR_ICALL) {
        tcg_gen_movi_i64(cpu_tgt, 0);
    }

    if (brtype == LINX_BR_ICALL) {
        /* Snapshot the existing BARG.BPCN target across the fused boundary. */
        ctx->tgt_modified = true;
    }

    if (brtype == LINX_BR_CALL || brtype == LINX_BR_ICALL) {
        tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                       offsetof(CPULinxState, call_setret_pending));
    }

    ctx->brtype = brtype;
    ctx->brtarget = initial_target; /* Keep for fallback/fallthrough calculation */
}

static void linx_block_begin(DisasContext *ctx, uint8_t brtype,
                             vaddr initial_target)
{
    linx_block_begin_common(ctx, brtype, ctx->base.pc_first,
                            initial_target, false);
}

static void linx_block_begin_preserve_scalar_queues(DisasContext *ctx,
                                                    uint8_t brtype,
                                                    vaddr initial_target)
{
    linx_block_begin_common(ctx, brtype, ctx->base.pc_first,
                            initial_target, true);
}

static void linx_block_begin_preserve_scalar_queues_at(DisasContext *ctx,
                                                       vaddr block_pc,
                                                       uint8_t brtype,
                                                       vaddr initial_target)
{
    linx_block_begin_common(ctx, brtype, block_pc, initial_target, true);
}

static bool linx_can_translate_fetch_span(DisasContext *ctx, vaddr pc,
                                          unsigned len)
{
    vaddr first_page = ctx->base.pc_first & TARGET_PAGE_MASK;
    vaddr second_page = first_page + TARGET_PAGE_SIZE;
    vaddr last;

    if (len == 0) {
        return false;
    }

    last = pc + len - 1;
    if (last < pc) {
        return false;
    }

    if (((first_page ^ pc) & TARGET_PAGE_MASK) == 0 &&
        ((first_page ^ last) & TARGET_PAGE_MASK) == 0) {
        return true;
    }

    /*
     * translator_ld* can also fetch from the immediately adjacent second page
     * of the current TB. It must not be used for arbitrary far targets.
     */
    return ((second_page ^ pc) & TARGET_PAGE_MASK) == 0 &&
           ((second_page ^ last) & TARGET_PAGE_MASK) == 0;
}

static bool linx_try_fetch_code_u16(DisasContext *ctx, vaddr pc,
                                    uint16_t *out)
{
    if (!linx_can_translate_fetch_span(ctx, pc, 2)) {
        return false;
    }
    *out = translator_lduw_end(ctx->env, &ctx->base, pc, MO_LE);
    return true;
}

static bool linx_try_fetch_code_u32(DisasContext *ctx, vaddr pc,
                                    uint32_t *out)
{
    uint16_t lo;
    uint16_t hi;

    if (!linx_try_fetch_code_u16(ctx, pc, &lo) ||
        !linx_try_fetch_code_u16(ctx, pc + 2, &hi)) {
        return false;
    }
    *out = (uint32_t)lo | ((uint32_t)hi << 16);
    return true;
}

typedef enum LinxFusedCallProbe {
    LINX_FUSED_CALL_NO_MATCH,
    LINX_FUSED_CALL_MATCH,
    LINX_FUSED_CALL_UNAVAILABLE,
} LinxFusedCallProbe;

static bool linx_is_hl_fused_call_raw(uint64_t raw)
{
    return (raw & UINT64_C(0xf83f0000007f)) ==
           UINT64_C(0x501600000011);
}

static LinxFusedCallProbe linx_probe_hl_fused_call(DisasContext *ctx,
                                                    vaddr pc, uint16_t hw,
                                                    uint64_t *raw_out)
{
    uint16_t hw2;
    uint16_t hw3;
    uint64_t raw;

    if ((hw & 0x007fu) != 0x0011u) {
        return LINX_FUSED_CALL_NO_MATCH;
    }
    if (!linx_try_fetch_code_u16(ctx, pc + 2, &hw2) ||
        !linx_try_fetch_code_u16(ctx, pc + 4, &hw3)) {
        return LINX_FUSED_CALL_UNAVAILABLE;
    }

    raw = (uint64_t)hw | ((uint64_t)hw2 << 16) | ((uint64_t)hw3 << 32);
    *raw_out = raw;
    return linx_is_hl_fused_call_raw(raw) ? LINX_FUSED_CALL_MATCH
                                           : LINX_FUSED_CALL_NO_MATCH;
}

static bool linx_try_fetch_code_insn(DisasContext *ctx, vaddr pc,
                                     uint64_t *raw_out, unsigned *len_out)
{
    uint16_t hw;
    unsigned len;
    uint64_t raw;

    if (!linx_try_fetch_code_u16(ctx, pc, &hw)) {
        return false;
    }

    len = linx_insn_len(hw);
    raw = hw;

    if (len == 4 && (hw & 0x007fu) == 0x0011u) {
        LinxFusedCallProbe probe =
            linx_probe_hl_fused_call(ctx, pc, hw, &raw);

        if (probe == LINX_FUSED_CALL_UNAVAILABLE) {
            return false;
        }
        if (probe == LINX_FUSED_CALL_MATCH) {
            *raw_out = raw;
            *len_out = 6;
            return true;
        }
        raw &= UINT32_MAX;
    }

    if (len >= 4) {
        uint16_t hw2;
        if (!linx_try_fetch_code_u16(ctx, pc + 2, &hw2)) {
            return false;
        }
        raw |= (uint64_t)hw2 << 16;
    }
    if (len >= 6) {
        uint16_t hw3;
        if (!linx_try_fetch_code_u16(ctx, pc + 4, &hw3)) {
            return false;
        }
        raw |= (uint64_t)hw3 << 32;
    }
    if (len >= 8) {
        uint16_t hw4;
        if (!linx_try_fetch_code_u16(ctx, pc + 6, &hw4)) {
            return false;
        }
        raw |= (uint64_t)hw4 << 48;
    }

    *raw_out = raw;
    *len_out = len;
    return true;
}

/* Helper to check if an address points to a block-start instruction.
 *
 * In addition to explicit BSTART encodings, LinxISA uses certain macro
 * instructions (FENTRY/FEXIT/FRET.*) as standalone blocks in the bring-up
 * toolchain.
 */
static bool linx_is_bstart_at_pc(DisasContext *ctx, vaddr pc)
{
    unsigned len;
    uint64_t raw;
    uint16_t hw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }
    hw = raw & 0xffffu;

    if (len == 2) {
        /* C.BSTART.STD / C.BSTART.FP: mask=0xc7ff, BrType in bits [13:11]. */
        if ((hw & 0xc7ff) == 0x0000 || (hw & 0xc7ff) == 0x0080) {
            const uint8_t brtype = (hw >> 11) & 0x7;
            if (brtype != 0) {
                return true;
            }
        }

        /* C.BSTART DIRECT/COND: check low nibble. */
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
        const uint32_t insn = raw & UINT32_MAX;

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
        if (linx_is_hl_fused_call_raw(raw)) {
            return true;
        }
        const uint16_t prefix = raw & 0xffffu;
        const uint32_t main32 = (raw >> 16) & UINT32_MAX;
        if ((prefix & 0xf) != 0xe) {
            return false;
        }

        /* HL.BSTART.*: encoded as 16-bit prefix + 32-bit BSTART main part. */
        if ((main32 & 0xff) == 0x01 && ((main32 >> 12) & 0x7) != 0) {
            return true;
        }
        return false;
    }

    if (len == 8) {
        /*
         * 64-bit L.BSTART.*: 16-bit trailer, 16 bits of padding, then the
         * 32-bit BSTART main word in bytes [4..7].
         */
        const uint32_t main32 = raw >> 32;

        if ((main32 & 0x7f) == 0x01 && ((main32 >> 12) & 0x7) != 0) {
            return true;
        }
        return false;
    }

    return false;
}

static bool linx_is_b_catr_at_pc(DisasContext *ctx, vaddr pc)
{
    unsigned len;
    uint64_t raw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }

    if (len != 4) {
        return false;
    }
    return (((uint32_t)raw) & 0x707fu) == 0x23u;
}

static bool linx_is_scalar_mem_at_pc(DisasContext *ctx, vaddr pc)
{
    unsigned len;
    uint64_t insn_raw;
    const LinxOpcodeMeta *meta;

    if (!linx_try_fetch_code_insn(ctx, pc, &insn_raw, &len)) {
        return false;
    }

    meta = linx_opcode_meta_lookup(insn_raw, len);
    return meta && (meta->major_cat == LINX_CAT_LOAD ||
                    meta->major_cat == LINX_CAT_STORE);
}

static bool linx_is_setret_at_pc(DisasContext *ctx, vaddr pc)
{
    unsigned len;
    uint64_t raw;
    uint16_t hw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }
    hw = raw & 0xffffu;

    if (len == 2) {
        return (hw & 0xf83f) == 0x5016;
    }

    if (len == 4) {
        return (((uint32_t)raw) & 0xfffu) == 0x507u;
    }

    if (len == 6) {
        return (raw & UINT64_C(0x00000fff000f)) ==
               UINT64_C(0x00000507000e);
    }

    return false;
}

/*
 * Current LLVM can lower a predicated forward edge as:
 *
 *   FALL block with SETC
 *   BSTART DIRECT, cold_trampoline
 *   BSTART COND, hot_target
 *
 * When the FALL block commits with CARG set and COND true, the immediate
 * direct trampoline is not the semantic successor.  When the following marker
 * is a fixed-target COND header, skip directly to its target: entering the COND
 * header as a fresh block would reset cond/carg and lose the predicate that
 * selected this edge.
 */
static bool linx_cond_bstart_target_at_pc(DisasContext *ctx, vaddr pc,
                                          vaddr *target_out)
{
    unsigned len;
    uint64_t raw;
    uint16_t hw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }
    hw = raw & 0xffffu;

    if (len == 2) {
        if ((hw & 0x000f) != 0x0004) {
            return false;
        }
        *target_out = pc + (((vaddr)sextract32(hw, 4, 12)) << 1);
        return true;
    }

    if (len == 4) {
        uint32_t insn = raw & UINT32_MAX;
        if ((insn & 0x7fu) == 0x21u) {
            *target_out = pc + (((vaddr)sextract32(insn, 7, 25)) << 1);
            return true;
        }
        if ((insn & 0x7fu) == 0x01u &&
            ((insn >> 12) & 0x7u) == LINX_BR_COND) {
            *target_out = pc + (((vaddr)sextract32(insn, 15, 17)) << 1);
            return true;
        }
    }

    return false;
}

static bool linx_predicated_fall_accept_skip(DisasContext *ctx, vaddr pc,
                                             unsigned len, vaddr *skip_pc)
{
    vaddr next_pc = pc + len;
    vaddr cond_target = 0;

    /*
     * Only skip marker-only direct trampolines.  A direct BSTART followed by a
     * normal instruction owns a real body; skipping just the header would execute
     * that body with stale block metadata.
     */
    if (!linx_is_bstart_at_pc(ctx, next_pc)) {
        return false;
    }

    if (linx_cond_bstart_target_at_pc(ctx, next_pc, &cond_target)) {
        *skip_pc = cond_target;
        return true;
    }

    *skip_pc = next_pc;
    return true;
}

static bool linx_setret_info_at_pc(DisasContext *ctx, vaddr pc,
                                   unsigned *len_out, vaddr *target_out)
{
    unsigned len;
    uint64_t raw;
    uint16_t hw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }
    hw = raw & 0xffffu;

    if (len == 2) {
        if ((hw & 0xf83f) != 0x5016) {
            return false;
        }
        *len_out = len;
        *target_out = pc + (((vaddr)((hw >> 6) & 0x1f)) << 1);
        return true;
    }

    if (len == 4) {
        uint32_t insn = raw & UINT32_MAX;
        if ((insn & 0xfffu) != 0x507u) {
            return false;
        }
        *len_out = len;
        *target_out = pc + (((vaddr)(insn >> 12)) << 1);
        return true;
    }

    if (len == 6) {
        uint64_t imm;

        if ((hw & 0xf) != 0xe) {
            return false;
        }
        if ((raw & UINT64_C(0x00000fff000f)) !=
            UINT64_C(0x00000507000e)) {
            return false;
        }
        *len_out = len;
        imm = ((raw >> 4) & UINT64_C(0xfff)) |
              (((raw >> 28) & UINT64_C(0xfffff)) << 12);
        *target_out = pc + ((vaddr)imm << 1);
        return true;
    }

    return false;
}

static bool linx_direct_bstart_target_at_pc(DisasContext *ctx, vaddr pc,
                                            vaddr *target_out)
{
    unsigned len;
    uint64_t raw;
    uint16_t hw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }
    hw = raw & 0xffffu;

    if (len == 2) {
        if ((hw & 0x000f) != 0x0002) {
            return false;
        }
        *target_out = pc + (((vaddr)sextract32(hw, 4, 12)) << 1);
        return true;
    }

    if (len == 4) {
        uint32_t insn = raw & UINT32_MAX;
        if ((insn & 0x7fu) != 0x11u) {
            return false;
        }
        *target_out = pc + (((vaddr)sextract32(insn, 7, 25)) << 1);
        return true;
    }

    return false;
}

static bool linx_is_call_like_bstart_at_pc(DisasContext *ctx, vaddr pc,
                                           unsigned *len_out)
{
    unsigned len;
    uint64_t raw;
    uint16_t hw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }
    hw = raw & 0xffffu;

    if (len == 2) {
        if (hw == 0x2000 || hw == 0x3000) {
            *len_out = len;
            return true;
        }
        return false;
    }

    if (len == 4) {
        uint32_t insn = raw & UINT32_MAX;
        uint32_t brtype;

        brtype = (insn >> 12) & 0x7u;
        if ((insn & 0xffu) == 0x01u &&
            (brtype == LINX_BR_CALL || brtype == LINX_BR_ICALL)) {
            *len_out = len;
            return true;
        }
        return false;
    }

    if (len == 6) {
        uint32_t main32 = (raw >> 16) & UINT32_MAX;
        uint32_t brtype;

        if (linx_is_hl_fused_call_raw(raw)) {
            *len_out = len;
            return true;
        }

        if ((hw & 0xf) != 0xe) {
            return false;
        }
        brtype = (main32 >> 12) & 0x7u;
        if ((main32 & 0xffu) == 0x01u &&
            (brtype == LINX_BR_CALL || brtype == LINX_BR_ICALL)) {
            *len_out = len;
            return true;
        }
        return false;
    }

    if (len == 8) {
        uint32_t main32 = raw >> 32;
        uint32_t brtype;

        brtype = (main32 >> 12) & 0x7u;
        if ((main32 & 0x7fu) == 0x01u &&
            (brtype == LINX_BR_CALL || brtype == LINX_BR_ICALL)) {
            *len_out = len;
            return true;
        }
    }

    return false;
}

static bool linx_predicated_fall_accept_call_skip(DisasContext *ctx, vaddr pc,
                                                  unsigned len, vaddr *skip_pc)
{
    vaddr next_pc = pc + len;
    unsigned setret_len = 0;
    vaddr setret_target = 0;
    vaddr direct_target = 0;

    /*
     * A skipped CALL/ICALL must not enter either the callee or the call-return
     * body.  SETRET gives the return continuation; LLVM commonly places a
     * direct block there to jump to the join block after a conditional call
     * body.  In that shape, the direct target is the semantic skip target.
     */
    if (linx_setret_info_at_pc(ctx, next_pc, &setret_len, &setret_target)) {
        if (linx_can_translate_fetch_span(ctx, setret_target, 8) &&
            linx_direct_bstart_target_at_pc(ctx, setret_target,
                                           &direct_target)) {
            *skip_pc = direct_target;
            return true;
        }
        *skip_pc = setret_target;
        return true;
    }

    *skip_pc = next_pc;
    return true;
}

static bool linx_predicated_fall_skip_target(DisasContext *ctx, vaddr pc,
                                             vaddr *skip_pc)
{
    unsigned len;
    uint64_t raw;
    uint16_t hw;

    if (!linx_try_fetch_code_insn(ctx, pc, &raw, &len)) {
        return false;
    }
    hw = raw & 0xffffu;

    {
        unsigned call_len = 0;
        if (linx_is_call_like_bstart_at_pc(ctx, pc, &call_len)) {
            return linx_predicated_fall_accept_call_skip(ctx, pc, call_len,
                                                        skip_pc);
        }
    }

    if (len == 2) {
        if ((hw & 0x000f) == 0x0002 ||
            ((hw & 0xc7ff) == 0x0000 &&
             ((hw >> 11) & 0x7) == LINX_BR_DIRECT)) {
            return linx_predicated_fall_accept_skip(ctx, pc, len, skip_pc);
        }
        return false;
    }

    if (len == 4) {
        uint32_t insn = raw & UINT32_MAX;
        if ((insn & 0x7f) == 0x11 ||
            ((insn & 0xff) == 0x01 && ((insn >> 12) & 0x7) == LINX_BR_DIRECT)) {
            return linx_predicated_fall_accept_skip(ctx, pc, len, skip_pc);
        }
        return false;
    }

    if (len == 6) {
        uint32_t main32 = (raw >> 16) & UINT32_MAX;
        if ((hw & 0xf) != 0xe) {
            return false;
        }
        if ((main32 & 0xff) == 0x01 &&
            ((main32 >> 12) & 0x7) == LINX_BR_DIRECT) {
            return linx_predicated_fall_accept_skip(ctx, pc, len, skip_pc);
        }
        return false;
    }

    if (len == 8) {
        uint32_t main32 = raw >> 32;
        if ((main32 & 0x7f) == 0x01 &&
            ((main32 >> 12) & 0x7) == LINX_BR_DIRECT) {
            return linx_predicated_fall_accept_skip(ctx, pc, len, skip_pc);
        }
        return false;
    }

    return false;
}

static bool linx_is_c_bstop_at_pc(DisasContext *ctx, vaddr pc)
{
    uint16_t hw;
    return linx_try_fetch_code_u16(ctx, pc, &hw) && hw == 0x0000;
}

static bool linx_is_bstop32_at_pc(DisasContext *ctx, vaddr pc)
{
    uint32_t insn;
    return linx_try_fetch_code_u32(ctx, pc, &insn) && insn == 0x00000001u;
}

static bool linx_is_j_bstop_trailer_at_pc(DisasContext *ctx, vaddr pc)
{
    uint32_t insn;
    uint16_t trailer;

    if (!linx_try_fetch_code_u32(ctx, pc, &insn) ||
        !linx_try_fetch_code_u16(ctx, pc + 4, &trailer)) {
        return false;
    }

    return (insn & 0x0000707fu) == 0x00000037u &&
           trailer == 0x0000;
}

static bool linx_is_ret_wrapper_j_trailer_at_pc(DisasContext *ctx, vaddr pc)
{
    uint16_t hw;
    return linx_try_fetch_code_u16(ctx, pc, &hw) &&
           hw == 0x3800 &&
           linx_is_j_bstop_trailer_at_pc(ctx, pc + 2);
}

static void linx_gen_ret_to_ra(DisasContext *ctx)
{
    TCGv_i64 ret_tgt = cpu_gpr[LINX_REG_RA];

    linx_gen_check_bstart_target(ctx, ret_tgt);
    if (linx_commit_trace_enabled) {
        gen_helper_linx_commit_trace(tcg_env, ret_tgt);
    }
    tcg_gen_mov_i64(cpu_pc, ret_tgt);
    tcg_gen_lookup_and_goto_ptr();
    ctx->base.is_jmp = DISAS_NORETURN;
}


static void linx_gen_goto_tb(DisasContext *ctx, int slot, vaddr dest,
                             bool validate_target)
{
    if (validate_target) {
        /*
         * Validate branch targets at runtime so demand-paged text can fault-in
         * naturally. Fallthrough paths do not require explicit BSTART markers.
         */
        linx_gen_check_bstart_target(ctx, tcg_constant_i64(dest));
    }

    if (linx_commit_trace_enabled) {
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(dest));
    }

    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(slot);
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, slot);
    } else {
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void linx_gen_goto_tb_after_committed_helper(DisasContext *ctx,
                                                    int slot, vaddr dest)
{
    if (translator_use_goto_tb(&ctx->base, dest)) {
        tcg_gen_goto_tb(slot);
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_exit_tb(ctx->base.tb, slot);
    } else {
        tcg_gen_movi_i64(cpu_pc, dest);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->base.is_jmp = DISAS_NORETURN;
}

static void linx_gen_block_end(DisasContext *ctx, vaddr fallthrough)
{
    const vaddr source_pc = ctx->base.pc_next - ctx->cur_insn_len;

    if (ctx->in_body) {
        /*
         * Decoupled body terminator:
         * - For SIMT/vector decoupled blocks, replay the body until the LB/LC
         *   loop nest completes, then return to the header continuation.
         * - For other decoupled bodies, return to the header continuation.
         */
        TCGLabel *done = gen_new_label();
        TCGv_i32 cont = tcg_temp_new_i32();
        gen_helper_linx_vec_body_next(cont, tcg_env);
        tcg_gen_brcondi_i32(TCG_COND_EQ, cont, 0, done);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_body_tpc);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_body_tpc);
        tcg_gen_exit_tb(NULL, 0);
        ctx->base.is_jmp = DISAS_NORETURN;
        gen_set_label(done);

        tcg_gen_movi_i32(cpu_in_body, 0);
        tcg_gen_movi_i64(cpu_body_end, 0);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_return_pc);
        }
        linx_gen_check_bstart_target(ctx, cpu_return_pc);
        tcg_gen_mov_i64(cpu_pc, cpu_return_pc);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }

    if (ctx->decoupled_header) {
        /*
         * Decoupled header terminator: jump to the out-of-line body specified
         * by B.TEXT, then resume at the header continuation (fallthrough).
         */
        TCGLabel *have_body = gen_new_label();
        TCGLabel *body_aligned = gen_new_label();
        const uint32_t missing_next_cause =
            linx_eblock_cfi_cause(LINX_EBLOCK_CFI_MISSING_NEXT_MARKER);
        const uint32_t bfetch_cause = linx_eblock_bfetch_cause();

        tcg_gen_brcondi_i64(TCG_COND_NE, cpu_body_tpc, 0, have_body);
        tcg_gen_movi_i64(cpu_pending_trap_arg0, source_pc);
        tcg_gen_movi_i32(cpu_pending_trap_cause, missing_next_cause);
        if (linx_commit_trace_enabled) {
            tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
            tcg_gen_movi_i32(cpu_trace_trap_cause,
                             (int32_t)(((missing_next_cause & 0xffffu) << 8) | 5));
            tcg_gen_movi_i64(cpu_trace_traparg0, source_pc);
            gen_helper_linx_commit_trace(tcg_env, cpu_bpc);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_bpc);
        gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_BLOCK_FAULT));
        tcg_gen_exit_tb(NULL, 0);
        gen_set_label(have_body);

        /*
         * v0.3: non-MMU body entry misalignment is E_BLOCK(EC_BFETCH), BI=1.
         * (MMU translation/access faults continue through the normal fetch path
         * and are delivered as E_DATA with BI=1.)
         */
        {
            TCGv_i64 lsb = tcg_temp_new_i64();
            tcg_gen_andi_i64(lsb, cpu_body_tpc, 1);
            tcg_gen_brcondi_i64(TCG_COND_EQ, lsb, 0, body_aligned);
        }
        tcg_gen_mov_i64(cpu_pending_trap_arg0, cpu_body_tpc);
        tcg_gen_movi_i32(cpu_pending_trap_cause, bfetch_cause);
        if (linx_commit_trace_enabled) {
            tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
            tcg_gen_movi_i32(cpu_trace_trap_cause,
                             (int32_t)(((bfetch_cause & 0xffffu) << 8) | 5));
            tcg_gen_mov_i64(cpu_trace_traparg0, cpu_body_tpc);
            gen_helper_linx_commit_trace(tcg_env, cpu_bpc);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_bpc);
        gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_BLOCK_FAULT));
        tcg_gen_exit_tb(NULL, 0);
        gen_set_label(body_aligned);

        tcg_gen_movi_i64(cpu_return_pc, fallthrough);
        tcg_gen_movi_i32(cpu_in_body, 1);
        gen_helper_linx_vec_body_begin(tcg_env);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_body_tpc);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_body_tpc);
        tcg_gen_exit_tb(NULL, 0);
        ctx->base.is_jmp = DISAS_NORETURN;
        return;
    }

    /*
     * Coupled tile blocks commit side effects before control-flow commit.
     * Scalar SPEC blocks reach this path very frequently with no tile
     * descriptor pending; branch around the helper in that common case while
     * preserving dynamic descriptor state that may have been decoded in an
     * earlier TB.
     */
    {
        TCGLabel *no_tile_commit = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_tile_iot_valid, 0,
                            no_tile_commit);
        gen_helper_linx_tile_commit(tcg_env, tcg_constant_i64(fallthrough));
        gen_set_label(no_tile_commit);
    }

    switch (ctx->brtype & 0x7) {
    case LINX_BR_FALL: {
        vaddr skip_pc = 0;
        if (linx_predicated_fall_skip_target(ctx, fallthrough, &skip_pc)) {
            TCGLabel *normal = gen_new_label();
            TCGLabel *skip = gen_new_label();
            tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_carg, 0, normal);
            tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, skip);
            gen_set_label(normal);
            linx_gen_goto_tb(ctx, 0, fallthrough, false);
            gen_set_label(skip);
            linx_gen_goto_tb(ctx, 1, skip_pc, false);
        } else {
            linx_gen_goto_tb(ctx, 0, fallthrough, false);
        }
        break;
    }
    case LINX_BR_DIRECT:
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            /*
             * LLVM still emits some fixed-target direct branches to mid-block
             * continuation PCs. Allow those to enter fetch/translation
             * naturally instead of pre-validating them as BSTART markers.
             */
            linx_gen_goto_tb(ctx, 0, ctx->brtarget, false);
        } else {
            /* Jump to cpu_tgt (diverted target from BSTART, or set target from SETC.TGT). */
            linx_gen_check_bstart_target(ctx, cpu_tgt);
            if (linx_commit_trace_enabled) {
                gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
            }
            tcg_gen_mov_i64(cpu_pc, cpu_tgt);
            tcg_gen_lookup_and_goto_ptr();
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        break;
    case LINX_BR_CALL:
        /*
         * v0.3: for direct CALL transitions, the return address defaults to the
         * next block-start marker in the linear stream (the fall-through). A
         * following SETRET/C.SETRET may override RA, but is not required.
         */
        if (!ctx->ra_set) {
            linx_set_dest(LINX_REG_RA, tcg_constant_i64(fallthrough));
        }
        if (linx_call_trace_translate_enabled) {
            gen_helper_linx_call_trace_event(tcg_env, tcg_constant_i64(source_pc),
                                             tcg_constant_i32(LINX_CALL_TRACE_CALL_COMMIT),
                                             tcg_constant_i64(fallthrough),
                                             tcg_constant_i64(ctx->brtarget));
        }
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            linx_gen_goto_tb(ctx, 0, ctx->brtarget, true);
        } else {
            linx_gen_check_bstart_target(ctx, cpu_tgt);
            if (linx_commit_trace_enabled) {
                gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
            }
            tcg_gen_mov_i64(cpu_pc, cpu_tgt);
            tcg_gen_lookup_and_goto_ptr();
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        break;
    case LINX_BR_COND: {
        /* Conditional jump: 
         * - If cpu_cond is set (and CARG), jump to diverted target (cpu_tgt)
         * - Otherwise fall through
         * Note: cpu_tgt may have been updated by SETC.TGT to override diverted target
         */
        TCGLabel *taken = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
        /* Condition not set: fall through */
        linx_gen_goto_tb(ctx, 1, fallthrough, false);
        gen_set_label(taken);
        /* Condition set: jump to diverted/set target */
        if (!ctx->tgt_modified && ctx->brtarget != 0) {
            /*
             * Fixed-target conditional branches can legally land on mid-block
             * continuation PCs in current LLVM output. Let fetch/translation
             * consume those targets instead of enforcing a marker here.
             */
            linx_gen_goto_tb(ctx, 0, ctx->brtarget, false);
        } else {
            linx_gen_check_bstart_target(ctx, cpu_tgt);
            if (linx_commit_trace_enabled) {
                gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
            }
            tcg_gen_mov_i64(cpu_pc, cpu_tgt);
            tcg_gen_lookup_and_goto_ptr();
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        break;
    }
    case LINX_BR_RET: {
        /*
         * Preserve legacy RET block semantics for normal translated code:
         * conditional target transfer via cpu_cond/cpu_tgt, otherwise
         * fallthrough. The only special-case we accept is the empty wrapper
         * form emitted by glibc's Linx syscall stubs:
         *
         *   C.BSTART.STD RET
         *
         * That block has no explicit target and no body instructions, so it
         * must return to `ra`.
         */
        if (!ctx->tgt_modified && ctx->brtarget == 0 &&
            (linx_is_c_bstop_at_pc(ctx, ctx->base.pc_first + 2) ||
             linx_is_ret_wrapper_j_trailer_at_pc(ctx, ctx->base.pc_first))) {
            linx_gen_ret_to_ra(ctx);
            break;
        }

        if (!ctx->tgt_modified && ctx->brtarget == 0) {
            (void)linx_block_fault(ctx, LINX_EBLOCK_LEGACY_RET_MISSING_SETCTGT, 0);
            return;
        }

        {
            TCGLabel *taken = gen_new_label();

            tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
            linx_gen_goto_tb(ctx, 1, fallthrough, false);
            gen_set_label(taken);
        }
        linx_gen_check_bstart_target(ctx, cpu_tgt);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    }
    case LINX_BR_IND:
        /* Indirect jump: jump to cpu_tgt (must be set by SETC.TGT) */
        if (!ctx->tgt_modified && ctx->brtarget == 0) {
            (void)linx_block_fault(ctx, LINX_EBLOCK_LEGACY_RET_MISSING_SETCTGT, 0);
            return;
        }
        linx_gen_check_bstart_target(ctx, cpu_tgt);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, cpu_tgt);
        }
        tcg_gen_mov_i64(cpu_pc, cpu_tgt);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    case LINX_BR_ICALL:
        /*
         * Indirect call: like IND, but set RA to the fall-through block start
         * marker for return.
         */
        if (!ctx->ra_set) {
            linx_set_dest(LINX_REG_RA, tcg_constant_i64(fallthrough));
        }
        if (linx_call_trace_translate_enabled) {
            gen_helper_linx_call_trace_event(tcg_env, tcg_constant_i64(source_pc),
                                             tcg_constant_i32(LINX_CALL_TRACE_CALL_COMMIT),
                                             tcg_constant_i64(fallthrough),
                                             tcg_constant_i64(ctx->brtarget));
        }
        if (!ctx->tgt_modified && ctx->brtarget == 0) {
            (void)linx_block_fault(ctx, LINX_EBLOCK_LEGACY_RET_MISSING_SETCTGT, 0);
            return;
        }
        /*
         * A fused 0.58.1 ICALL owns a durable snapshot of the retiring
         * BARG.BPCN. Later SETC.TGT instructions and TB splits must not
        * redirect that call. Legacy ICALL continues to consume cpu_tgt.
         */
        TCGv_i64 icall_target = tcg_temp_new_i64();
        TCGv_i32 fused_valid = tcg_temp_new_i32();
        TCGLabel *legacy_target = gen_new_label();
        TCGLabel *target_selected = gen_new_label();
        tcg_gen_ld_i32(fused_valid, tcg_env,
                       offsetof(CPULinxState, fused_icall_target_valid));
        tcg_gen_brcondi_i32(TCG_COND_EQ, fused_valid, 0, legacy_target);
        tcg_gen_ld_i64(icall_target, tcg_env,
                       offsetof(CPULinxState, fused_icall_target));
        tcg_gen_st_i64(tcg_constant_i64(0), tcg_env,
                       offsetof(CPULinxState, fused_icall_target));
        tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                       offsetof(CPULinxState, fused_icall_target_valid));
        tcg_gen_br(target_selected);
        gen_set_label(legacy_target);
        tcg_gen_mov_i64(icall_target, cpu_tgt);
        gen_set_label(target_selected);
        linx_gen_check_bstart_target(ctx, icall_target);
        if (linx_commit_trace_enabled) {
            gen_helper_linx_commit_trace(tcg_env, icall_target);
        }
        tcg_gen_mov_i64(cpu_pc, icall_target);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
        break;
    default:
        /* Unhandled block kind: fall through. */
        linx_gen_goto_tb(ctx, 0, fallthrough, false);
        break;
    }
}

static inline uint64_t G_GNUC_UNUSED linx_zext32(uint64_t x)
{
    return (uint32_t)x;
}

static inline uint64_t G_GNUC_UNUSED linx_sext32(uint64_t x)
{
    return (int64_t)(int32_t)x;
}

static TCGv_i64 linx_srcR_addsub(DisasContext *ctx, unsigned srcR,
                                 unsigned srcRType, unsigned shamt)
{
    TCGv_i64 r = linx_get_reg(srcR);
    TCGv_i64 tmp = tcg_temp_new_i64();

    switch (srcRType & 0x3) {
    case 0: /* .sw */
        tcg_gen_ext32s_i64(tmp, r);
        break;
    case 1: /* .uw */
        tcg_gen_ext32u_i64(tmp, r);
        break;
    case 2: /* .neg */
        tcg_gen_neg_i64(tmp, r);
        break;
    default: /* no modifier */
        tcg_gen_mov_i64(tmp, r);
        break;
    }
    if (shamt) {
        tcg_gen_shli_i64(tmp, tmp, shamt & 0x3f);
    }
    return tmp;
}

static TCGv_i64 linx_srcR_logic(DisasContext *ctx, unsigned srcR,
                                unsigned srcRType, unsigned shamt)
{
    TCGv_i64 r = linx_get_reg(srcR);
    TCGv_i64 tmp = tcg_temp_new_i64();

    switch (srcRType & 0x3) {
    case 0: /* .sw */
        tcg_gen_ext32s_i64(tmp, r);
        break;
    case 1: /* .uw */
        tcg_gen_ext32u_i64(tmp, r);
        break;
    case 2: /* .not */
        tcg_gen_not_i64(tmp, r);
        break;
    default: /* no modifier */
        tcg_gen_mov_i64(tmp, r);
        break;
    }
    if (shamt) {
        tcg_gen_shli_i64(tmp, tmp, shamt & 0x3f);
    }
    return tmp;
}

static vaddr linx_pcrel_target(vaddr pc, int64_t simm_hw)
{
    return pc + ((vaddr)simm_hw << 1);
}

static bool linx_illegal(DisasContext *ctx)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: illegal instruction @ 0x%" VADDR_PRIx
                  " (insn_len=%u in_body=%u tb_flags=0x%x env_in_body=%u)\n",
                  pc, ctx->cur_insn_len, ctx->in_body ? 1u : 0u,
                  ctx->base.tb ? ctx->base.tb->flags : 0u,
                  ctx->env && ctx->env->in_body ? 1u : 0u);
    trace_linx_insn_decode_fail(ctx->base.pc_next, 0, ctx->cur_insn_len);
    tcg_gen_movi_i64(cpu_pending_trap_arg0, 0);
    tcg_gen_movi_i32(cpu_pending_trap_cause, 0);
    if (linx_commit_trace_enabled) {
        /* Trapnum=ILLEGAL_INST(4), cause=0. */
        tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
        tcg_gen_movi_i32(cpu_trace_trap_cause, 4);
        tcg_gen_movi_i64(cpu_trace_traparg0, 0);
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(pc));
    }
    tcg_gen_movi_i64(cpu_pc, pc);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_ILLEGAL_INST));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool linx_block_fault(DisasContext *ctx, uint32_t legacy_cause, uint64_t arg0)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;

    /* v0.3 mapping from legacy v0.2 internal causes. */
    uint32_t cause = 0;
    uint64_t traparg0 = arg0;

    switch (legacy_cause) {
    case LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY:
    case LINX_EBLOCK_LEGACY_ILLEGAL_IN_HEADER:
        /* v0.3: illegal instruction stream -> E_INST(EC_ILLEGAL), BI determined by ctx->in_body. */
        return linx_illegal(ctx);

    case LINX_EBLOCK_LEGACY_BAD_BRANCH_TARGET:
        cause = linx_eblock_cfi_cause(LINX_EBLOCK_CFI_BAD_TARGET);
        traparg0 = pc;
        break;

    case LINX_EBLOCK_LEGACY_MISSING_BODY_TPC:
        cause = linx_eblock_cfi_cause(LINX_EBLOCK_CFI_MISSING_NEXT_MARKER);
        traparg0 = pc;
        break;

    case LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK:
        /* v0.3: block-format illegal combination (descriptor outside block). */
        cause = linx_eblock_blockfmt_cause();
        if ((arg0 >> 8) != 0) {
            traparg0 = arg0;
        } else {
            uint8_t family = (uint8_t)(arg0 & 0xffu);
            if (family == 0) {
                family = LINX_BLOCKFMT_FAMILY_NONE;
            }
            traparg0 = linx_blockfmt_traparg_make(
                family, LINX_BLOCKFMT_DETAIL_ILLEGAL_COMBO);
        }
        break;

    case LINX_EBLOCK_LEGACY_ACRC_MISSING_BSTOP:
        /* v0.3: block-format illegal combination (ACRC without terminating BSTOP marker). */
        cause = linx_eblock_blockfmt_cause();
        traparg0 = linx_blockfmt_traparg_make(
            LINX_BLOCKFMT_FAMILY_NONE, LINX_BLOCKFMT_DETAIL_ILLEGAL_COMBO);
        break;

    case LINX_EBLOCK_LEGACY_CALL_MISSING_SETRET:
    case LINX_EBLOCK_LEGACY_CALL_INVALID_SEQUENCE:
    case LINX_EBLOCK_LEGACY_RET_MISSING_SETCTGT:
        /* v0.3: treat toolchain call/ret pairing violations as block-format illegal combinations. */
        cause = linx_eblock_blockfmt_cause();
        traparg0 = linx_blockfmt_traparg_make(
            LINX_BLOCKFMT_FAMILY_ARG, LINX_BLOCKFMT_DETAIL_ILLEGAL_COMBO);
        break;

    default:
        /* Default to generic block-format fault. */
        cause = linx_eblock_blockfmt_cause();
        if (traparg0 == 0) {
            traparg0 = linx_blockfmt_traparg_make(
                LINX_BLOCKFMT_FAMILY_NONE, LINX_BLOCKFMT_DETAIL_INVALID);
        }
        break;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "Linx: block fault @ 0x%" VADDR_PRIx " legacy=%u ec=0x%x\n",
                  pc, legacy_cause, (unsigned)((cause >> 8) & 0xffu));
    if (linx_debug_local_enabled_p()) {
        uint8_t dbg_bytes[8] = {0};
        CPUState *cs = env_cpu(ctx->env);
        int dbg_rc = cpu_memory_rw_debug(cs, pc, dbg_bytes, sizeof(dbg_bytes), 0);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: block fault detail pc_first=0x%" VADDR_PRIx
                      " pc_next=0x%" VADDR_PRIx " len=%u raw=0x%016" PRIx64
                      " brtype=%u brtarget=0x%" VADDR_PRIx
                      " ra_set=%u in_body=%u block_idx=%u tb_flags=0x%x\n",
                      ctx->base.pc_first, ctx->base.pc_next, ctx->cur_insn_len,
                      ctx->cur_insn_raw, ctx->brtype, ctx->brtarget,
                      ctx->ra_set ? 1u : 0u, ctx->in_body ? 1u : 0u,
                      ctx->block_insn_index,
                      ctx->base.tb ? ctx->base.tb->flags : 0u);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: block fault bytes rc=%d data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                      dbg_rc,
                      dbg_bytes[0], dbg_bytes[1], dbg_bytes[2], dbg_bytes[3],
                      dbg_bytes[4], dbg_bytes[5], dbg_bytes[6], dbg_bytes[7]);
    }

    tcg_gen_movi_i64(cpu_pending_trap_arg0, traparg0);
    tcg_gen_movi_i32(cpu_pending_trap_cause, cause);

    if (linx_commit_trace_enabled) {
        /* Trapnum=BLOCK_TRAP(5), cause=v0.3 packed cause. */
        tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
        tcg_gen_movi_i32(cpu_trace_trap_cause,
                         (int32_t)(((cause & 0xffffu) << 8) | 5));
        tcg_gen_movi_i64(cpu_trace_traparg0, traparg0);
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(pc));
    }

    tcg_gen_movi_i64(cpu_pc, pc);
    gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_BLOCK_FAULT));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* Include the auto-generated decoders. */
#include "decode-insn16.c.inc"
#include "decode-insn32.c.inc"
#include "decode-insn48.c.inc"
#include "decode-insn64.c.inc"

static bool trans_bstart_tile_common(DisasContext *ctx, uint32_t dtype, uint32_t op);
static bool trans_bstart_tile_func_common(DisasContext *ctx, uint32_t dtype,
                                          uint32_t blocktype, uint32_t func);

static bool linx_begin_header_target(DisasContext *ctx, uint8_t brtype, vaddr target)
{
    /* pc_next has already been advanced past the current insn, so we need to
     * check if the CURRENT instruction (pc_next - cur_insn_len) is at pc_first */
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    /*
     * Linux bitops can emit an in-body BSTART.STD immediately followed by
     * B.CATR to annotate scalar memory operations.  Treat that descriptor as
     * part of the current block so the pending branch does not commit before
     * the memory side effect.
     */
    if (current_pc != ctx->base.pc_first &&
        brtype == LINX_BR_FALL &&
        ctx->brtype != 0 &&
        linx_is_b_catr_at_pc(ctx, ctx->base.pc_next)) {
        return true;
    }
    /*
     * Linux uaccess emits an internal FALL header with a fixup target directly
     * before the protected scalar load/store. Start that block at the internal
     * header while preserving scalar queues already prepared by the prologue,
     * so exception delivery reports the fixup header as BPC.
     */
    if (current_pc != ctx->base.pc_first &&
        brtype == LINX_BR_FALL &&
        ctx->brtype != 0 &&
        linx_is_scalar_mem_at_pc(ctx, ctx->base.pc_next)) {
        linx_block_begin_preserve_scalar_queues_at(ctx, current_pc,
                                                   LINX_BR_FALL, target);
        return true;
    }
    if (current_pc != ctx->base.pc_first &&
        (ctx->brtype == LINX_BR_CALL || ctx->brtype == LINX_BR_ICALL) &&
        brtype != LINX_BR_CALL &&
        brtype != LINX_BR_ICALL &&
        ctx->ra_set &&
        ctx->call_ra_target != 0 &&
        ctx->call_ra_target > ctx->base.pc_first &&
        current_pc > ctx->base.pc_first &&
        current_pc < ctx->call_ra_target) {
        /*
         * Current Linx LLVM lowering can emit extra headers inside an open
         * CALL/ICALL block before the recorded SETRET continuation. Keep
         * translating through those internal markers so the call prelude
         * executes before control transfers to the callee. This applies only
         * to forward continuations; self-returning noreturn-call encodings
         * must commit at the next header.
         */
        return true;
    }
    if (current_pc != ctx->base.pc_first) {
        /* BSTART in the middle of a translation block - end the previous block */
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, brtype, target);
    return true;
}

static bool linx_begin_sys_header_target(DisasContext *ctx, vaddr target)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin_preserve_scalar_queues(ctx, LINX_BR_FALL, target);
    return true;
}

/* C.BSTART.STD helper - also used by explicit 16-bit overlap handling. */
static bool linx_trans_c_bstart_std(DisasContext *ctx, uint8_t brtype)
{
    return linx_begin_header_target(ctx, brtype, 0);
}

static bool trans_c_bstart_std_fall(DisasContext *ctx, arg_c_bstart_std_fall *a)
{
    (void)a;
    return linx_trans_c_bstart_std(ctx, LINX_BR_FALL);
}

static bool trans_c_bstart_std_ind(DisasContext *ctx, arg_c_bstart_std_ind *a)
{
    (void)a;
    return linx_trans_c_bstart_std(ctx, LINX_BR_IND);
}

static bool trans_c_bstart_std_ret(DisasContext *ctx, arg_c_bstart_std_ret *a)
{
    (void)a;
    return linx_trans_c_bstart_std(ctx, LINX_BR_RET);
}

static bool trans_c_bstart_fp(DisasContext *ctx, arg_c_bstart_fp *a)
{
    const uint8_t brtype = (uint8_t)((ctx->cur_insn_raw >> 11) & 0x7u);

    (void)a;
    return linx_begin_header_target(ctx, brtype, 0);
}

static bool trans_c_setret(DisasContext *ctx, arg_c_setret *a)
{
    return linx_setret_common(ctx, (int64_t)a->uimm);
}

static bool trans_c_bstart_direct(DisasContext *ctx, arg_c_bstart_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_DIRECT,
                                    linx_pcrel_target(current_pc, a->simm12));
}

static bool trans_c_bstart_cond(DisasContext *ctx, arg_c_bstart_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_COND,
                                    linx_pcrel_target(current_pc, a->simm12));
}

static bool trans_c_bstart_sys(DisasContext *ctx, arg_c_bstart_sys *a)
{
    (void)a;
    if (ctx->base.pc_next - ctx->cur_insn_len != ctx->base.pc_first &&
        ctx->brtype != 0) {
        return true;
    }
    return linx_begin_sys_header_target(ctx, 0);
}

static bool trans_bstart_split_direct(DisasContext *ctx,
                                      arg_bstart_split_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    uint8_t brtype = linx_is_setret_at_pc(ctx, ctx->base.pc_next)
                         ? LINX_BR_CALL
                         : LINX_BR_DIRECT;

    return linx_begin_header_target(ctx, brtype,
                                    linx_pcrel_target(current_pc, a->simm25));
}

static bool trans_bstart_split_cond(DisasContext *ctx,
                                    arg_bstart_split_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_COND,
                                    linx_pcrel_target(current_pc, a->simm25));
}

static bool trans_c_bstart_mpar(DisasContext *ctx, arg_c_bstart_mpar *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 0);
    ctx->decoupled_header = true;
    return true;
}

static bool trans_c_bstart_mseq(DisasContext *ctx, arg_c_bstart_mseq *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 1);
    ctx->decoupled_header = true;
    return true;
}

static bool trans_c_bstart_vpar(DisasContext *ctx, arg_c_bstart_vpar *a)
{
    return trans_c_bstart_mpar(ctx, (arg_c_bstart_mpar *)a);
}

static bool trans_c_bstart_vseq(DisasContext *ctx, arg_c_bstart_vseq *a)
{
    return trans_c_bstart_mseq(ctx, (arg_c_bstart_mseq *)a);
}

static bool trans_c_bstop(DisasContext *ctx, arg_c_bstop *a)
{
    /* pc_next has already been advanced, so fallthrough is just pc_next */
    linx_gen_block_end(ctx, ctx->base.pc_next);
    return true;
}

static bool trans_bstop(DisasContext *ctx, arg_bstop *a)
{
    /* pc_next has already been advanced, so fallthrough is just pc_next */
    linx_gen_block_end(ctx, ctx->base.pc_next);
    return true;
}

static bool trans_l_bstop(DisasContext *ctx, arg_l_bstop *a)
{
    return trans_bstop(ctx, (arg_bstop *)a);
}

static bool trans_bstart_call(DisasContext *ctx, arg_bstart_call *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_CALL, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_fall(DisasContext *ctx, arg_bstart_fall *a)
{
    (void)a;
    return linx_begin_header_target(ctx, LINX_BR_FALL, 0);
}

static bool trans_bstart_direct(DisasContext *ctx, arg_bstart_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_DIRECT, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_cond(DisasContext *ctx, arg_bstart_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_COND, linx_pcrel_target(current_pc, a->simm17));
    return true;
}

static bool trans_bstart_ind(DisasContext *ctx, arg_bstart_ind *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_IND, 0);
    return true;
}

static bool trans_bstart_icall(DisasContext *ctx, arg_bstart_icall *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_ICALL, 0);
    return true;
}

static bool trans_bstart_ret(DisasContext *ctx, arg_bstart_ret *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_RET, 0);
    return true;
}

static bool trans_bstart_fp_fall(DisasContext *ctx, arg_bstart_fp_fall *a)
{
    (void)a;
    return linx_begin_header_target(ctx, LINX_BR_FALL, 0);
}

static bool trans_bstart_fp_direct(DisasContext *ctx, arg_bstart_fp_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_DIRECT,
                                    linx_pcrel_target(current_pc, a->simm17));
}

static bool trans_bstart_fp_cond(DisasContext *ctx, arg_bstart_fp_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_COND,
                                    linx_pcrel_target(current_pc, a->simm17));
}

static bool trans_bstart_fp_call(DisasContext *ctx, arg_bstart_fp_call *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_CALL,
                                    linx_pcrel_target(current_pc, a->simm17));
}

static bool trans_bstart_fp_ind(DisasContext *ctx, arg_bstart_fp_ind *a)
{
    (void)a;
    return linx_begin_header_target(ctx, LINX_BR_IND, 0);
}

static bool trans_bstart_fp_icall(DisasContext *ctx, arg_bstart_fp_icall *a)
{
    (void)a;
    return linx_begin_header_target(ctx, LINX_BR_ICALL, 0);
}

static bool trans_bstart_fp_ret(DisasContext *ctx, arg_bstart_fp_ret *a)
{
    (void)a;
    return linx_begin_header_target(ctx, LINX_BR_RET, 0);
}

static bool trans_bstart_sys(DisasContext *ctx, arg_bstart_sys *a)
{
    (void)a;
    if (ctx->base.pc_next - ctx->cur_insn_len != ctx->base.pc_first &&
        ctx->brtype != 0) {
        return true;
    }
    return linx_begin_sys_header_target(ctx, 0);
}

#define LINX_TRANS_TILE_OPERATION_DIRECT(name, selector) \
    static bool trans_bstart_##name(DisasContext *ctx, \
                                    arg_bstart_##name *a) \
    { \
        return trans_bstart_tile_common(ctx, a->dtype, selector); \
    }

LINX_TRANS_TILE_OPERATION_DIRECT(tadd, 0x000u)
LINX_TRANS_TILE_OPERATION_DIRECT(tsub, 0x001u)
LINX_TRANS_TILE_OPERATION_DIRECT(tmul, 0x002u)
LINX_TRANS_TILE_OPERATION_DIRECT(tmax, 0x00bu)
LINX_TRANS_TILE_OPERATION_DIRECT(tmin, 0x00cu)
LINX_TRANS_TILE_OPERATION_DIRECT(tand, 0x006u)
LINX_TRANS_TILE_OPERATION_DIRECT(tor, 0x007u)
LINX_TRANS_TILE_OPERATION_DIRECT(txor, 0x008u)
LINX_TRANS_TILE_OPERATION_DIRECT(tshl, 0x009u)
LINX_TRANS_TILE_OPERATION_DIRECT(tshr, 0x00au)
LINX_TRANS_TILE_OPERATION_DIRECT(tcmp, 0x00du)
LINX_TRANS_TILE_OPERATION_DIRECT(tsel, 0x01au)
LINX_TRANS_TILE_OPERATION_DIRECT(tabs, 0x00fu)
LINX_TRANS_TILE_OPERATION_DIRECT(tnot, 0x010u)
LINX_TRANS_TILE_OPERATION_DIRECT(tneg, 0x011u)
LINX_TRANS_TILE_OPERATION_DIRECT(trelu, 0x017u)
LINX_TRANS_TILE_OPERATION_DIRECT(tfma, 0x01cu)
LINX_TRANS_TILE_OPERATION_DIRECT(tdiv, 0x003u)
LINX_TRANS_TILE_OPERATION_DIRECT(trem, 0x004u)
LINX_TRANS_TILE_OPERATION_DIRECT(tsqrt, 0x015u)
LINX_TRANS_TILE_OPERATION_DIRECT(tlog, 0x013u)
LINX_TRANS_TILE_OPERATION_DIRECT(trecip, 0x014u)
LINX_TRANS_TILE_OPERATION_DIRECT(texp, 0x012u)
LINX_TRANS_TILE_OPERATION_DIRECT(trsqrt, 0x016u)
LINX_TRANS_TILE_OPERATION_DIRECT(tadds, 0x020u)
LINX_TRANS_TILE_OPERATION_DIRECT(tsubs, 0x021u)
LINX_TRANS_TILE_OPERATION_DIRECT(tmuls, 0x022u)
LINX_TRANS_TILE_OPERATION_DIRECT(tdivs, 0x023u)
LINX_TRANS_TILE_OPERATION_DIRECT(tmins, 0x02cu)
LINX_TRANS_TILE_OPERATION_DIRECT(tmaxs, 0x02bu)
LINX_TRANS_TILE_OPERATION_DIRECT(trems, 0x024u)
LINX_TRANS_TILE_OPERATION_DIRECT(tands, 0x026u)
LINX_TRANS_TILE_OPERATION_DIRECT(tors, 0x027u)
LINX_TRANS_TILE_OPERATION_DIRECT(txors, 0x028u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcmps, 0x02du)
LINX_TRANS_TILE_OPERATION_DIRECT(tsels, 0x03au)
LINX_TRANS_TILE_OPERATION_DIRECT(tshls, 0x029u)
LINX_TRANS_TILE_OPERATION_DIRECT(tshrs, 0x02au)
LINX_TRANS_TILE_OPERATION_DIRECT(trowsum, 0x040u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowprod, 0x043u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowmax, 0x041u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowmin, 0x042u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowargmax, 0x04cu)
LINX_TRANS_TILE_OPERATION_DIRECT(trowargmin, 0x04du)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolsum, 0x050u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolprod, 0x053u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolmax, 0x051u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolmin, 0x052u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolargmax, 0x05cu)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolargmin, 0x05du)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpand, 0x044u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpandadd, 0x045u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpandsub, 0x046u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpandmul, 0x047u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpanddiv, 0x048u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpandmax, 0x049u)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpandmin, 0x04au)
LINX_TRANS_TILE_OPERATION_DIRECT(trowexpandexpdif, 0x04bu)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpand, 0x054u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpandadd, 0x055u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpandsub, 0x056u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpandmul, 0x057u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpanddiv, 0x058u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpandmax, 0x059u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpandmin, 0x05au)
LINX_TRANS_TILE_OPERATION_DIRECT(tcolexpandexpdif, 0x05bu)
LINX_TRANS_TILE_OPERATION_DIRECT(texpands, 0x03bu)
LINX_TRANS_TILE_OPERATION_DIRECT(tci, 0x066u)
LINX_TRANS_TILE_OPERATION_DIRECT(ttri, 0x067u)
LINX_TRANS_TILE_OPERATION_DIRECT(tfillpad, 0x065u)
LINX_TRANS_TILE_OPERATION_DIRECT(tcvt, 0x01bu)
LINX_TRANS_TILE_OPERATION_DIRECT(tquant, 0x06au)
LINX_TRANS_TILE_OPERATION_DIRECT(tdequant, 0x06bu)
LINX_TRANS_TILE_OPERATION_DIRECT(textract, 0x062u)
LINX_TRANS_TILE_OPERATION_DIRECT(tinsert, 0x063u)
LINX_TRANS_TILE_OPERATION_DIRECT(tgather, 0x06fu)
LINX_TRANS_TILE_OPERATION_DIRECT(tscatter, 0x070u)
LINX_TRANS_TILE_OPERATION_DIRECT(tconcat, 0x060u)
LINX_TRANS_TILE_OPERATION_DIRECT(ttrans, 0x06eu)
LINX_TRANS_TILE_OPERATION_DIRECT(timg2col, 0x064u)
LINX_TRANS_TILE_OPERATION_DIRECT(tsort, 0x06cu)
LINX_TRANS_TILE_OPERATION_DIRECT(tmrgsort, 0x06du)
LINX_TRANS_TILE_OPERATION_DIRECT(thistogram, 0x068u)
LINX_TRANS_TILE_OPERATION_DIRECT(tpartadd, 0x071u)
LINX_TRANS_TILE_OPERATION_DIRECT(tpartmul, 0x072u)
LINX_TRANS_TILE_OPERATION_DIRECT(tpartmax, 0x073u)
LINX_TRANS_TILE_OPERATION_DIRECT(tpartmin, 0x074u)

#undef LINX_TRANS_TILE_OPERATION_DIRECT

static bool trans_bstart_tile_common(DisasContext *ctx, uint32_t dtype, uint32_t op)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    op &= 0x7fu;
    dtype &= 0x1fu;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_tile_dtype, dtype);

    /* Keep the canonical Mode+Function selector as the architectural ID. */
    tcg_gen_movi_i32(cpu_blocktype, 7); /* VEC/SFU via TEPL carrier */
    tcg_gen_movi_i32(cpu_tile_func, op);

    /* Canonical v0.4 baseline keeps these tile blocks coupled in QEMU. */
    ctx->decoupled_header = false;
    return true;
}

static bool trans_bstart_tepl(DisasContext *ctx, arg_bstart_tepl *a)
{
    return trans_bstart_tile_common(ctx, a->dtype,
                                   ((a->mode & 0x3u) << 5) |
                                   (a->function & 0x1fu));
}

static bool trans_bstart_mseq(DisasContext *ctx, arg_bstart_mseq *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 1); /* MSEQ: sequential vector with memory */
    ctx->decoupled_header = true;
    return true;
}

static bool trans_bstart_mpar(DisasContext *ctx, arg_bstart_mpar *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    (void)a;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, 0); /* MPAR: parallel vector with memory */
    ctx->decoupled_header = true;
    return true;
}

static bool trans_bstart_vpar(DisasContext *ctx, arg_bstart_vpar *a)
{
    return trans_bstart_mpar(ctx, (arg_bstart_mpar *)a);
}

static bool trans_bstart_vseq(DisasContext *ctx, arg_bstart_vseq *a)
{
    return trans_bstart_mseq(ctx, (arg_bstart_mseq *)a);
}

static bool trans_bstart_tload(DisasContext *ctx, arg_bstart_tload *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 0);
}

static bool trans_bstart_tstore(DisasContext *ctx, arg_bstart_tstore *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 1);
}

static bool trans_bstart_tstore_spart(DisasContext *ctx,
                                      arg_bstart_tstore_spart *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 1);
}

static bool trans_bstart_tmov(DisasContext *ctx, arg_bstart_tmov *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 2);
}

static bool trans_bstart_tmov_l2s_insert(
    DisasContext *ctx, arg_bstart_tmov_l2s_insert *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 9);
}

static bool trans_bstart_tmov_l2s_publish(
    DisasContext *ctx, arg_bstart_tmov_l2s_publish *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 10);
}

static bool trans_bstart_tmov_s2l_broadcast(
    DisasContext *ctx, arg_bstart_tmov_s2l_broadcast *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 11);
}

static bool trans_bstart_tmov_s2l_extract(
    DisasContext *ctx, arg_bstart_tmov_s2l_extract *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 12);
}

static bool trans_bstart_tprefetch(DisasContext *ctx, arg_bstart_tprefetch *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 3);
}

static bool trans_bstart_mgather(DisasContext *ctx, arg_bstart_mgather *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 4);
}

static bool trans_bstart_mscatter(DisasContext *ctx, arg_bstart_mscatter *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 5);
}

static bool trans_bstart_mgather_mask(DisasContext *ctx,
                                      arg_bstart_mgather_mask *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 6);
}

static bool trans_bstart_mscatter_mask(DisasContext *ctx,
                                       arg_bstart_mscatter_mask *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 7);
}

static bool trans_bstart_mgather_cas(DisasContext *ctx,
                                     arg_bstart_mgather_cas *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 8);
}

static bool trans_bstart_gmov(DisasContext *ctx, arg_bstart_gmov *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 2, 13);
}

static bool trans_bstart_tmatmul(DisasContext *ctx, arg_bstart_tmatmul *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 0);
}

static bool trans_bstart_tmatmul_bias(DisasContext *ctx,
                                      arg_bstart_tmatmul_bias *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 1);
}

static bool trans_bstart_tmatmul_acc(DisasContext *ctx,
                                     arg_bstart_tmatmul_acc *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 2);
}

static bool trans_bstart_tmatmulmx(DisasContext *ctx,
                                   arg_bstart_tmatmulmx *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 4);
}

static bool trans_bstart_tmatmulmx_bias(DisasContext *ctx,
                                        arg_bstart_tmatmulmx_bias *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 5);
}

static bool trans_bstart_tmatmulmx_acc(DisasContext *ctx,
                                       arg_bstart_tmatmulmx_acc *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 6);
}

static bool trans_bstart_tgemv(DisasContext *ctx, arg_bstart_tgemv *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 16);
}

static bool trans_bstart_tgemv_bias(DisasContext *ctx,
                                    arg_bstart_tgemv_bias *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 17);
}

static bool trans_bstart_tgemv_acc(DisasContext *ctx,
                                   arg_bstart_tgemv_acc *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 18);
}

static bool trans_bstart_tgemvmx(DisasContext *ctx, arg_bstart_tgemvmx *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 20);
}

static bool trans_bstart_tgemvmx_bias(DisasContext *ctx,
                                      arg_bstart_tgemvmx_bias *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 21);
}

static bool trans_bstart_tgemvmx_acc(DisasContext *ctx,
                                     arg_bstart_tgemvmx_acc *a)
{
    return trans_bstart_tile_func_common(ctx, a->dtype, 6, 22);
}

static bool trans_bstart_tile_func_common(DisasContext *ctx, uint32_t dtype,
                                          uint32_t blocktype, uint32_t func)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (!linx_tile_data_type_accepted(dtype & 0x1fu)) {
        return linx_illegal(ctx);
    }
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    linx_block_begin(ctx, LINX_BR_FALL, 0);
    tcg_gen_movi_i32(cpu_blocktype, blocktype);
    tcg_gen_movi_i32(cpu_tile_func, func & 0x1f);
    tcg_gen_movi_i32(cpu_tile_dtype, dtype & 0x1f);
    ctx->decoupled_header = false;
    return true;
}

static bool trans_b_dim_common(DisasContext *ctx, uint32_t reg, uint32_t uimm,
                               uint32_t lb)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_DIM);
    }
    if (lb > 2) {
        return linx_illegal(ctx);
    }

    TCGv_i64 src = linx_get_reg(reg);
    /*
     * The autogenerated decodetree field currently exposes B.DIM immediates
     * in the raw transport form, shifted left by five. Convert back to the
     * authored logical replay count before updating LB state.
     */
    const int64_t imm = (int64_t)((uimm & 0x1ffffu) >> 5);
    tcg_gen_addi_i64(cpu_lb[lb], src, imm);
    return true;
}

static bool trans_c_b_dimi(DisasContext *ctx, arg_c_b_dimi *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_DIM);
    }
    if (a->loopnest > 2u) {
        return linx_illegal(ctx);
    }

    tcg_gen_movi_i64(cpu_lb[a->loopnest], (uint64_t)(a->imm8 & 0xffu));
    return true;
}

static bool trans_b_ios(DisasContext *ctx, arg_b_ios *a)
{
    if (a->pe_mask == 0u) {
        return true;
    }
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_IOT);
    }
    gen_helper_linx_tile_append_shared_binder_v058(
        tcg_env, tcg_constant_i64((a->shared & 0xffu) |
                                  ((a->pe_mask & 0xfu) << 8) |
                                  ((a->tsize & 0x7u) << 12)));
    /* Shared bindings have no B.IOT, so the binder keeps commit pending. */
    tcg_gen_movi_i32(cpu_tile_iot_valid, 1);
    return true;
}

static bool trans_b_dim_lb0(DisasContext *ctx, arg_b_dim_lb0 *a)
{
    return trans_b_dim_common(ctx, a->reg, a->uimm, 0);
}

static bool trans_b_dim_lb1(DisasContext *ctx, arg_b_dim_lb1 *a)
{
    return trans_b_dim_common(ctx, a->reg, a->uimm, 1);
}

static bool trans_b_dim_lb2(DisasContext *ctx, arg_b_dim_lb2 *a)
{
    return trans_b_dim_common(ctx, a->reg, a->uimm, 2);
}

static bool linx_trans_b_iot(DisasContext *ctx, uint32_t func, uint32_t dst,
                             uint32_t last, uint32_t pe_mask,
                             uint32_t src0, uint32_t src1, uint32_t tsize)
{
    if (pe_mask == 0u) {
        return true;
    }
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_IOT);
    }

    if (func < 4u || func > 6u || tsize > 7u) {
        return linx_illegal(ctx);
    }
    const uint32_t flags = func == 4u ? 0u
                           : func == 5u ? LINX_IOT_S1V
                                       : LINX_IOT_S0V | LINX_IOT_S1V;
    /* TSize is the per-PE Local Tile capacity; PE_MASK selects participants. */
    const uint32_t local_size_code = tsize == 0u ? 0u : tsize + 2u;
    linx_emit_tile_iot_desc(ctx, flags, dst, last, src0, src1, pe_mask,
                            local_size_code, tsize != 0u);
    return true;
}

static bool trans_b_iot(DisasContext *ctx, arg_b_iot *a)
{
    return linx_trans_b_iot(ctx, a->func, a->dst, a->l, a->pe_mask,
                            a->src0, a->src1, a->tsize);
}

static bool trans_b_text(DisasContext *ctx, arg_b_text *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr body_tpc = linx_pcrel_target(pc, a->simm25);

    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_TEXT);
    }
    if ((body_tpc & 1u) != 0) {
        /* v0.3: non-MMU body-entry misalignment -> E_BLOCK(EC_BFETCH), BI=1. */
        const uint32_t cause = linx_eblock_bfetch_cause();
        tcg_gen_movi_i64(cpu_pending_trap_arg0, body_tpc);
        tcg_gen_movi_i32(cpu_pending_trap_cause, cause);
        if (linx_commit_trace_enabled) {
            tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
            tcg_gen_movi_i32(cpu_trace_trap_cause,
                             (int32_t)(((cause & 0xffffu) << 8) | 5));
            tcg_gen_movi_i64(cpu_trace_traparg0, body_tpc);
            gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(pc));
        }
        tcg_gen_movi_i64(cpu_pc, pc);
        gen_helper_raise_exception(tcg_env, tcg_constant_i32(LINX_EXCP_BLOCK_FAULT));
        ctx->base.is_jmp = DISAS_NORETURN;
        return true;
    }

    tcg_gen_movi_i64(cpu_body_tpc, body_tpc);
    tcg_gen_movi_i64(cpu_body_end, 0);
    return true;
}

static bool trans_b_ior(DisasContext *ctx, arg_b_ior *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_IOR);
    }
    const uint64_t desc =
        ((uint64_t)(a->RegDst & 0x1f) << 0) |
        ((uint64_t)(a->SrcL & 0x1f) << 5) |
        ((uint64_t)(a->SrcR & 0x1f) << 10) |
        ((uint64_t)(a->SrcD & 0x1f) << 15);
    gen_helper_linx_tile_append_ior(tcg_env, tcg_constant_i64(desc));
    return true;
}

static bool trans_b_catr(DisasContext *ctx, arg_b_catr *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK, 0);
    }
    const uint32_t packed =
        ((uint32_t)(a->trap & 0x1u) << 0) |
        ((uint32_t)(a->dr & 0x1u) << 1) |
        ((uint32_t)(a->aq & 0x1u) << 18) |
        ((uint32_t)(a->atom & 0x1u) << 19) |
        ((uint32_t)(a->far & 0x1u) << 20) |
        ((uint32_t)(a->rl & 0x1u) << 21);
    const uint32_t data_mask = 0x1fc3fffcu;
    tcg_gen_andi_i32(cpu_tile_attr_raw, cpu_tile_attr_raw, data_mask);
    tcg_gen_ori_i32(cpu_tile_attr_raw, cpu_tile_attr_raw, packed);
    return true;
}

static bool trans_b_datr(DisasContext *ctx, arg_b_datr *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK, 0);
    }
    if (a->cmode > 5u ||
        !linx_tile_data_type_accepted(a->dtype & 0x1fu) ||
        !linx_tile_layout_accepted(a->layout & 0x1fu)) {
        return linx_illegal(ctx);
    }
    const uint32_t packed =
        ((uint32_t)(a->layout & 0x1fu) << 2) |
        ((uint32_t)(a->dtype & 0x1fu) << 7) |
        ((uint32_t)(a->pad & 0x3u) << 12) |
        ((uint32_t)(a->canonicalize & 0x1u) << 17) |
        ((uint32_t)(a->cmode & 0x7u) << 22) |
        ((uint32_t)(a->rmode & 0x7u) << 25) |
        ((uint32_t)(a->sat & 0x1u) << 28);
    const uint32_t control_mask = 0x003c0003u;
    tcg_gen_andi_i32(cpu_tile_attr_raw, cpu_tile_attr_raw, control_mask);
    tcg_gen_ori_i32(cpu_tile_attr_raw, cpu_tile_attr_raw, packed);
    tcg_gen_movi_i32(cpu_tile_attr_dtype, 0x100u | (a->dtype & 0x1fu));
    tcg_gen_movi_i32(cpu_tile_attr_pad, a->pad & 0x3u);
    return true;
}

static bool trans_b_fpatr(DisasContext *ctx, arg_b_fpatr *a)
{
    static const uint64_t prequant_mask = UINT64_C(0x000000ff1f9f303f);

    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK, 0);
    }
    if (a->prequant >= 64u ||
        (prequant_mask & (UINT64_C(1) << a->prequant)) == 0u ||
        a->relu > 3u || a->groupn > 9u ||
        (!a->rowmax && a->rowinit) ||
        (a->groupmax != (a->groupn != 0u)) ||
        (!a->rowmax && !a->groupmax && a->maxabs)) {
        return linx_illegal(ctx);
    }
    tcg_gen_st_i32(
        tcg_constant_i32((a->prequant << 26) | (a->relu << 23) |
                         (a->groupn << 19) | (a->rowmax << 18) |
                         (a->groupmax << 17) | (a->rowinit << 16) |
                         (a->maxabs << 15)),
        tcg_env, offsetof(CPULinxState, tile_fpatr_raw));
    tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                   offsetof(CPULinxState, tile_fpatr_valid));
    return true;
}

static bool trans_b_hint(DisasContext *ctx, arg_b_hint *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK, 0);
    }
    (void)a;
    return true;
}

static bool trans_b_hint_trace(DisasContext *ctx, arg_b_hint_trace *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK, 0);
    }
    (void)a;
    return true;
}

static bool linx_setret_common(DisasContext *ctx, int64_t imm_hw)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_ARG);
    }
    if (ctx->brtype != LINX_BR_CALL && ctx->brtype != LINX_BR_ICALL) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_CALL_INVALID_SEQUENCE, 0);
    }
    if (ctx->ra_set) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_CALL_INVALID_SEQUENCE, 0);
    }

    {
        vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
        vaddr tgt = pc + ((vaddr)imm_hw << 1);
        linx_set_dest(LINX_REG_RA, tcg_constant_i64(tgt));
        ctx->call_ra_target = tgt;
        if (linx_call_trace_translate_enabled) {
            gen_helper_linx_call_trace_event(tcg_env, tcg_constant_i64(pc),
                                             tcg_constant_i32(LINX_CALL_TRACE_SETRET),
                                             tcg_constant_i64(tgt),
                                             tcg_constant_i64(0));
        }
    }
    ctx->ra_set = true;
    tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                   offsetof(CPULinxState, call_ra_set));
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(CPULinxState, call_setret_pending));
    return true;
}

static bool trans_setret(DisasContext *ctx, arg_setret *a)
{
    return linx_setret_common(ctx, (int64_t)a->imm20);
}

static bool trans_c_setc_tgt(DisasContext *ctx, arg_c_setc_tgt *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_ARG);
    }
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    ctx->tgt_modified = true;
    return true;
}

static bool trans_setc_tgt(DisasContext *ctx, arg_setc_tgt *a)
{
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_ARG);
    }
    TCGv_i64 v = linx_get_reg(a->SrcL);
    tcg_gen_mov_i64(cpu_tgt, v);
    tcg_gen_movi_i32(cpu_cond, 1);
    ctx->tgt_modified = true;
    return true;
}

static bool trans_setc_cmp(DisasContext *ctx, TCGCond c, TCGv_i64 l, TCGv_i64 r);

static bool trans_setc_cmp_imm(DisasContext *ctx, TCGCond c, TCGv_i64 l, int64_t imm)
{
    TCGv_i64 r = tcg_temp_new_i64();
    tcg_gen_movi_i64(r, (uint64_t)imm);
    return trans_setc_cmp(ctx, c, l, r);
}

static bool trans_c_setc_eq(DisasContext *ctx, arg_c_setc_eq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    return trans_setc_cmp(ctx, TCG_COND_EQ, l, r);
}

static bool trans_c_setc_ne(DisasContext *ctx, arg_c_setc_ne *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    return trans_setc_cmp(ctx, TCG_COND_NE, l, r);
}

static bool trans_setc_eq(DisasContext *ctx, arg_setc_eq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_EQ, l, r);
}

static bool trans_setc_eqi(DisasContext *ctx, arg_setc_eqi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_EQ, l, simm);
}

static bool trans_setc_ne(DisasContext *ctx, arg_setc_ne *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_NE, l, r);
}

static bool trans_setc_nei(DisasContext *ctx, arg_setc_nei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, l, simm);
}

static bool trans_setc_and(DisasContext *ctx, arg_setc_and *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_andi(DisasContext *ctx, arg_setc_andi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    tcg_gen_andi_i64(out, l, simm);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_or(DisasContext *ctx, arg_setc_or *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_ori(DisasContext *ctx, arg_setc_ori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    tcg_gen_ori_i64(out, l, simm);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_setc_cmp(DisasContext *ctx, TCGCond c, TCGv_i64 l, TCGv_i64 r)
{
    if (ctx->brtype == 0) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_DESC_OUTSIDE_BLOCK,
                                LINX_BLOCKFMT_FAMILY_ARG);
    }
    /*
     * SETC updates block commit arguments and is frequently used to drive
     * BSTART COND and predicate-controlled commits. Lower it branchlessly so
     * common compare patterns don't force extra host control-flow.
     */
    TCGv_i64 cond64 = tcg_temp_new_i64();
    TCGv_i32 cond32 = tcg_temp_new_i32();
    tcg_gen_setcond_i64(c, cond64, l, r);
    tcg_gen_extrl_i64_i32(cond32, cond64);
    tcg_gen_mov_i32(cpu_cond, cond32);
    tcg_gen_mov_i32(cpu_carg, cond32);
    return true;
}

static bool trans_setc_lt(DisasContext *ctx, arg_setc_lt *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_LT, l, r);
}

static bool trans_setc_lti(DisasContext *ctx, arg_setc_lti *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_LT, l, simm);
}

static bool trans_setc_ltu(DisasContext *ctx, arg_setc_ltu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_LTU, l, r);
}

static bool trans_setc_ltui(DisasContext *ctx, arg_setc_ltui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    uint64_t uimm = (uint64_t)a->uimm12 << a->shamt;
    return trans_setc_cmp_imm(ctx, TCG_COND_LTU, l, (int64_t)uimm);
}

static bool trans_setc_ge(DisasContext *ctx, arg_setc_ge *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_GE, l, r);
}

static bool trans_setc_gei(DisasContext *ctx, arg_setc_gei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    int64_t simm = (int64_t)((uint64_t)(int64_t)a->simm12 << a->shamt);
    return trans_setc_cmp_imm(ctx, TCG_COND_GE, l, simm);
}

static bool trans_setc_geu(DisasContext *ctx, arg_setc_geu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_setc_cmp(ctx, TCG_COND_GEU, l, r);
}

static bool trans_setc_geui(DisasContext *ctx, arg_setc_geui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    uint64_t uimm = (uint64_t)a->uimm12 << a->shamt;
    return trans_setc_cmp_imm(ctx, TCG_COND_GEU, l, (int64_t)uimm);
}

static bool trans_alu_binop(DisasContext *ctx, unsigned dst, TCGv_i64 res)
{
    linx_set_dest(dst, res);
    return true;
}

static bool trans_add(DisasContext *ctx, arg_add *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_add_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_sub(DisasContext *ctx, arg_sub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sub_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_and(DisasContext *ctx, arg_and *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_or(DisasContext *ctx, arg_or *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_xor(DisasContext *ctx, arg_xor *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xor_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_addi(DisasContext *ctx, arg_addi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_subi(DisasContext *ctx, arg_subi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_andi(DisasContext *ctx, arg_andi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_ori(DisasContext *ctx, arg_ori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_xori(DisasContext *ctx, arg_xori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm12);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool linx_binop_w(DisasContext *ctx, unsigned dst, TCGv_i64 out32)
{
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(out, out32);
    linx_set_dest(dst, out);
    return true;
}

static bool trans_addw(DisasContext *ctx, arg_addw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_add_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_subw(DisasContext *ctx, arg_subw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sub_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_andw(DisasContext *ctx, arg_andw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_orw(DisasContext *ctx, arg_orw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_xorw(DisasContext *ctx, arg_xorw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, a->shamt);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xor_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_addiw(DisasContext *ctx, arg_addiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_subiw(DisasContext *ctx, arg_subiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_andiw(DisasContext *ctx, arg_andiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_oriw(DisasContext *ctx, arg_oriw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_xoriw(DisasContext *ctx, arg_xoriw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm12);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_mul(DisasContext *ctx, arg_mul *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_mul_i64(out, l, r);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_madd(DisasContext *ctx, arg_madd *a)
{
    TCGv_i64 acc = linx_get_reg(a->SrcD);
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 prod = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    tcg_gen_mul_i64(prod, l, r);
    tcg_gen_add_i64(out, acc, prod);
    return trans_alu_binop(ctx, a->RegDst, out);
}

/*
 * Scalar DIV/REM are non-trapping.  Guard cases that host division cannot
 * execute directly: divide by zero and signed MIN_INT / -1 overflow.
 */
static void linx_emit_divrem(TCGv_i64 out, TCGv_i64 lhs, TCGv_i64 rhs,
                             bool is_div, bool is_signed, bool is_word)
{
    TCGLabel *divzero = gen_new_label();
    TCGLabel *done = gen_new_label();
    TCGv_i64 minval = tcg_constant_i64(
        (int64_t)(is_word ? UINT64_C(0xffffffff80000000) : INT64_MIN));

    tcg_gen_brcondi_i64(TCG_COND_EQ, rhs, 0, divzero);
    if (is_signed) {
        TCGLabel *not_overflow = gen_new_label();
        TCGLabel *overflow = gen_new_label();

        tcg_gen_brcond_i64(TCG_COND_NE, lhs, minval, not_overflow);
        tcg_gen_brcondi_i64(TCG_COND_EQ, rhs, -1, overflow);
        gen_set_label(not_overflow);
        if (is_div) {
            tcg_gen_div_i64(out, lhs, rhs);
        } else {
            tcg_gen_rem_i64(out, lhs, rhs);
        }
        tcg_gen_br(done);
        gen_set_label(overflow);
        if (is_div) {
            tcg_gen_mov_i64(out, lhs);
        } else {
            tcg_gen_movi_i64(out, 0);
        }
        tcg_gen_br(done);
    } else {
        if (is_div) {
            tcg_gen_divu_i64(out, lhs, rhs);
        } else {
            tcg_gen_remu_i64(out, lhs, rhs);
        }
        tcg_gen_br(done);
    }

    gen_set_label(divzero);
    if (is_div) {
        tcg_gen_movi_i64(out, 0);
    } else {
        tcg_gen_mov_i64(out, lhs);
    }
    gen_set_label(done);
}

static bool trans_div_like(DisasContext *ctx, unsigned dst,
                           TCGv_i64 l, TCGv_i64 r, bool is_div, bool is_signed,
                           bool is_word)
{
    TCGv_i64 out = tcg_temp_new_i64();

    if (is_word) {
        TCGv_i64 l32 = tcg_temp_new_i64();
        TCGv_i64 r32 = tcg_temp_new_i64();
        if (is_signed) {
            tcg_gen_ext32s_i64(l32, l);
            tcg_gen_ext32s_i64(r32, r);
        } else {
            tcg_gen_ext32u_i64(l32, l);
            tcg_gen_ext32u_i64(r32, r);
        }
        linx_emit_divrem(out, l32, r32, is_div, is_signed, true);
        return linx_binop_w(ctx, dst, out);
    }

    linx_emit_divrem(out, l, r, is_div, is_signed, false);
    return trans_alu_binop(ctx, dst, out);
}

static bool trans_div(DisasContext *ctx, arg_div *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, true, false);
}

static bool trans_divu(DisasContext *ctx, arg_divu *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, false, false);
}

static bool trans_divw(DisasContext *ctx, arg_divw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, true, true);
}

static bool trans_divuw(DisasContext *ctx, arg_divuw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          true, false, true);
}

static bool trans_rem(DisasContext *ctx, arg_rem *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, true, false);
}

static bool trans_remu(DisasContext *ctx, arg_remu *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, false, false);
}

static bool trans_remw(DisasContext *ctx, arg_remw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, true, true);
}

static bool trans_remuw(DisasContext *ctx, arg_remuw *a)
{
    return trans_div_like(ctx, a->RegDst,
                          linx_get_reg(a->SrcL), linx_get_reg(a->SrcR),
                          false, false, true);
}

static bool trans_fabs(DisasContext *ctx, arg_fabs *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fabs(out, tcg_env, v, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fadd(DisasContext *ctx, arg_fadd *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fadd(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fsub(DisasContext *ctx, arg_fsub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fsub(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fmul(DisasContext *ctx, arg_fmul *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fmul(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fdiv(DisasContext *ctx, arg_fdiv *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fdiv(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_feq(DisasContext *ctx, arg_feq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_feq(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fne(DisasContext *ctx, arg_fne *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fne(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_flt(DisasContext *ctx, arg_flt *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_flt(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fge(DisasContext *ctx, arg_fge *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fge(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_feqs(DisasContext *ctx, arg_feqs *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_feqs(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fnes(DisasContext *ctx, arg_fnes *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fnes(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_flts(DisasContext *ctx, arg_flts *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_flts(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fges(DisasContext *ctx, arg_fges *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fges(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fmax(DisasContext *ctx, arg_fmax *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fmax(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fmin(DisasContext *ctx, arg_fmin *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fmin(out, tcg_env, l, r, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fmadd(DisasContext *ctx, arg_fmadd *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 acc = linx_get_reg(a->SrcA);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fmadd(out, tcg_env, l, r, acc, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fmsub(DisasContext *ctx, arg_fmsub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 acc = linx_get_reg(a->SrcA);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fmsub(out, tcg_env, l, r, acc, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fnmadd(DisasContext *ctx, arg_fnmadd *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 acc = linx_get_reg(a->SrcA);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fnmadd(out, tcg_env, l, r, acc, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fnmsub(DisasContext *ctx, arg_fnmsub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 acc = linx_get_reg(a->SrcA);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fnmsub(out, tcg_env, l, r, acc, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fsqrt(DisasContext *ctx, arg_fsqrt *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fsqrt(out, tcg_env, v, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_frecip(DisasContext *ctx, arg_frecip *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_frecip(out, tcg_env, v, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fexp(DisasContext *ctx, arg_fexp *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fexp(out, tcg_env, v, tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvt(DisasContext *ctx, arg_fcvt *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvt(out, tcg_env, v, tcg_constant_i32(a->DstType),
                         tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvta(DisasContext *ctx, arg_fcvta *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvta(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvtm(DisasContext *ctx, arg_fcvtm *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvtm(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvtn(DisasContext *ctx, arg_fcvtn *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvtn(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvtp(DisasContext *ctx, arg_fcvtp *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvtp(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_fcvtz(DisasContext *ctx, arg_fcvtz *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_fcvtz(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_scvtf(DisasContext *ctx, arg_scvtf *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_scvtf(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_ucvtf(DisasContext *ctx, arg_ucvtf *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    gen_helper_linx_ucvtf(out, tcg_env, v, tcg_constant_i32(a->DstType),
                          tcg_constant_i32(a->SrcType));
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_mulw(DisasContext *ctx, arg_mulw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();

    tcg_gen_mul_i64(out, l, r);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_maddw(DisasContext *ctx, arg_maddw *a)
{
    TCGv_i64 acc = linx_get_reg(a->SrcD);
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 prod = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    tcg_gen_mul_i64(prod, l, r);
    tcg_gen_add_i64(out, acc, prod);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_cmp_out(DisasContext *ctx, unsigned dst,
                          TCGCond c, TCGv_i64 l, TCGv_i64 r)
{
    TCGv_i64 out = tcg_temp_new_i64();
    TCGLabel *t = gen_new_label();
    TCGLabel *done = gen_new_label();

    tcg_gen_movi_i64(out, 0);
    tcg_gen_brcond_i64(c, l, r, t);
    tcg_gen_br(done);
    gen_set_label(t);
    tcg_gen_movi_i64(out, 1);
    gen_set_label(done);
    linx_set_dest(dst, out);
    return true;
}

static bool trans_cmp_eq(DisasContext *ctx, arg_cmp_eq *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_EQ, l, r);
}

static bool trans_cmp_ne(DisasContext *ctx, arg_cmp_ne *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_NE, l, r);
}

static bool trans_cmp_and(DisasContext *ctx, arg_cmp_and *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_and_i64(tmp, l, r);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_NE, tmp, tcg_constant_i64(0));
}

static bool trans_cmp_or(DisasContext *ctx, arg_cmp_or *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_logic(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 tmp = tcg_temp_new_i64();
    tcg_gen_or_i64(tmp, l, r);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_NE, tmp, tcg_constant_i64(0));
}

static bool trans_cmp_lt(DisasContext *ctx, arg_cmp_lt *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_LT, l, r);
}

static bool trans_cmp_ltu(DisasContext *ctx, arg_cmp_ltu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_LTU, l, r);
}

static bool trans_cmp_ge(DisasContext *ctx, arg_cmp_ge *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_GE, l, r);
}

static bool trans_cmp_geu(DisasContext *ctx, arg_cmp_geu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    return trans_cmp_out(ctx, a->RegDst, TCG_COND_GEU, l, r);
}

static bool trans_csel(DisasContext *ctx, arg_csel *a)
{
    /* LinxISA csel: csel SrcP, SrcL, SrcR<.modifier>, ->{t, u, Rd}
     * Semantics: if SrcP != 0 (true), output = SrcL; else output = SrcR
     * The SrcRType modifier applies to SrcR before the false-case select.
     */
    TCGv_i64 pred = linx_get_reg(a->SrcP);
    TCGv_i64 tval = linx_get_reg(a->SrcL);
    TCGv_i64 fval = linx_srcR_addsub(ctx, a->SrcR, a->SrcRType, 0);
    TCGv_i64 out = tcg_temp_new_i64();
    TCGLabel *done = gen_new_label();

    tcg_gen_mov_i64(out, fval);               /* default to false case (SrcR) */
    tcg_gen_brcondi_i64(TCG_COND_EQ, pred, 0, done);  /* if pred == 0, done */
    tcg_gen_mov_i64(out, tval);               /* else use true case (SrcL) */
    gen_set_label(done);

    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_lui(DisasContext *ctx, arg_lui *a)
{
    int32_t imm = sextract32(a->imm20, 0, 20);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, (int64_t)imm << 12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_lui(DisasContext *ctx, arg_hl_lui *a)
{
    int32_t imm = (int32_t)a->imm;
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, (int64_t)imm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_sll(DisasContext *ctx, arg_sll *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(shamt, sh, 0x3f);
    tcg_gen_shl_i64(out, l, shamt);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_srl(DisasContext *ctx, arg_srl *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(shamt, sh, 0x3f);
    tcg_gen_shr_i64(out, l, shamt);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_sra(DisasContext *ctx, arg_sra *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(shamt, sh, 0x3f);
    tcg_gen_sar_i64(out, l, shamt);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_slli(DisasContext *ctx, arg_slli *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shli_i64(out, l, a->shamt & 0x3f);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_srli(DisasContext *ctx, arg_srli *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shri_i64(out, l, a->shamt & 0x3f);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_srai(DisasContext *ctx, arg_srai *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sari_i64(out, l, a->shamt & 0x3f);
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_sllw(DisasContext *ctx, arg_sllw *a)
{
    /* Word shifts operate on the low 32 bits only, regardless of the upper
     * bits in the source register (which are often undefined for 32-bit C
     * values). Mask the shift amount to 0..31.
     */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_andi_i64(shamt, sh, 0x1f);
    tcg_gen_shl_i64(out, l32, shamt);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_srlw(DisasContext *ctx, arg_srlw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_andi_i64(shamt, sh, 0x1f);
    tcg_gen_shr_i64(out, l32, shamt);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_sraw(DisasContext *ctx, arg_sraw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 sh = linx_get_reg(a->SrcR);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 shamt = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(l32, l);
    tcg_gen_andi_i64(shamt, sh, 0x1f);
    tcg_gen_sar_i64(out, l32, shamt);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_slliw(DisasContext *ctx, arg_slliw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_shli_i64(out, l32, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_srliw(DisasContext *ctx, arg_srliw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(l32, l);
    tcg_gen_shri_i64(out, l32, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_sraiw(DisasContext *ctx, arg_sraiw *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 l32 = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(l32, l);
    tcg_gen_sari_i64(out, l32, a->shamt & 0x1f);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_c_movr(DisasContext *ctx, arg_c_movr *a)
{
    TCGv_i64 v = linx_get_reg(a->SrcL);
    linx_set_dest(a->RegDst, v);
    return true;
}

static bool trans_c_movi(DisasContext *ctx, arg_c_movi *a)
{
    /* C.SETRET is a special case of C.MOVI when RegDst == RA */
    if (a->RegDst == LINX_REG_RA) {
        return linx_setret_common(ctx, (int64_t)(a->simm5 & 0x1f));
    }

    /* Normal C.MOVI */
    TCGv_i64 v = tcg_temp_new_i64();
    tcg_gen_movi_i64(v, (int64_t)a->simm5);
    linx_set_dest(a->RegDst, v);
    return true;
}

static bool trans_c_addi(DisasContext *ctx, arg_c_addi *a)
{
    /* C.ADDI: SrcL + simm5 -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (int64_t)a->simm5);
    linx_push_t(out);
    return true;
}

static bool trans_c_add(DisasContext *ctx, arg_c_add *a)
{
    /* C.ADD: SrcL + SrcR -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_add_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static bool trans_c_sub(DisasContext *ctx, arg_c_sub *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_sub_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static bool trans_c_and(DisasContext *ctx, arg_c_and *a)
{
    /* C.AND: SrcL & SrcR -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_and_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static bool trans_c_or(DisasContext *ctx, arg_c_or *a)
{
    /* C.OR: SrcL | SrcR -> T-hand */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_or_i64(out, l, r);
    linx_push_t(out);
    return true;
}

static TCGv linx_addr_from_i64(TCGv_i64 a64)
{
#if TARGET_LONG_BITS == 32
    TCGv a = tcg_temp_new();
    tcg_gen_trunc_i64_tl(a, a64);
    return a;
#else
    return a64;
#endif
}

static TCGv_i64 linx_addr_add_imm(DisasContext *ctx, unsigned base, int64_t off)
{
    TCGv_i64 addr = tcg_temp_new_i64();
    TCGv_i64 b = linx_get_reg(base);
    tcg_gen_addi_i64(addr, b, off);
    return addr;
}

static TCGv_i64 linx_addr_add_reg(DisasContext *ctx, unsigned base,
                                  unsigned idx, unsigned idx_type,
                                  unsigned shamt)
{
    TCGv_i64 b = linx_get_reg(base);
    TCGv_i64 i = linx_get_reg(idx);
    TCGv_i64 t = tcg_temp_new_i64();
    TCGv_i64 addr = tcg_temp_new_i64();

    switch (idx_type & 0x3) {
    case 0: /* .sw */
        tcg_gen_ext32s_i64(t, i);
        break;
    case 1: /* .uw */
        tcg_gen_ext32u_i64(t, i);
        break;
    case 2: /* .neg */
        tcg_gen_neg_i64(t, i);
        break;
    default: /* no modifier */
        tcg_gen_mov_i64(t, i);
        break;
    }
    if (shamt) {
        tcg_gen_shli_i64(t, t, shamt & 0x3f);
    }
    tcg_gen_add_i64(addr, b, t);
    return addr;
}

static bool linx_load_to_dest(DisasContext *ctx, unsigned dst, TCGv addr,
                              MemOp mop)
{
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    TCGv_i64 out = tcg_temp_new_i64();

    if (ctx->base.tb->flags & LINX_TB_FLAG_DBG_ACTIVE) {
        const unsigned size = memop_size(mop);
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        gen_helper_linx_dbg_check_load(tcg_env, tcg_constant_i64(pc), addr64,
                                       tcg_constant_i32((int32_t)size));
    }
    if (linx_mem_trace_translate_enabled && linx_mem_trace_translate_loads) {
        const unsigned size = memop_size(mop);
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        if (linx_mem_trace_translate_pre_enabled) {
            linx_gen_mem_trace_probe(false, true, pc, addr64, size,
                                     tcg_constant_i64(0));
        }
    }
    tcg_gen_qemu_ld_i64(out, addr, ctx->mem_idx, mop | linx_mo_endian());

    if (linx_mem_trace_translate_enabled && linx_mem_trace_translate_loads) {
        const unsigned size = memop_size(mop);
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        linx_gen_mem_trace_probe(false, false, pc, addr64, size, out);
    }

    if (linx_commit_trace_enabled) {
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        tcg_gen_movi_i32(cpu_trace_mem_valid, 1);
        tcg_gen_movi_i32(cpu_trace_mem_is_store, 0);
        tcg_gen_mov_i64(cpu_trace_mem_addr, addr64);
        tcg_gen_movi_i64(cpu_trace_mem_wdata, 0);
        tcg_gen_mov_i64(cpu_trace_mem_rdata, out);
        tcg_gen_movi_i32(cpu_trace_mem_size, (int32_t)memop_size(mop));
    }

    linx_set_dest(dst, out);
    return true;
}

static bool trans_lbi(DisasContext *ctx, arg_lbi *a)
{
    int64_t off = (int64_t)a->simm12;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_lbui(DisasContext *ctx, arg_lbui *a)
{
    int64_t off = (int64_t)a->simm12;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_lhi(DisasContext *ctx, arg_lhi *a)
{
    int64_t off = (int64_t)a->simm12 * 2;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lhui(DisasContext *ctx, arg_lhui *a)
{
    int64_t off = (int64_t)a->simm12 * 2;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_lwi(DisasContext *ctx, arg_lwi *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_lb_pcr(DisasContext *ctx, arg_lb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_lbu_pcr(DisasContext *ctx, arg_lbu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_lh_pcr(DisasContext *ctx, arg_lh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lhu_pcr(DisasContext *ctx, arg_lhu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_lw_pcr(DisasContext *ctx, arg_lw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_lwu_pcr(DisasContext *ctx, arg_lwu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_ld_pcr(DisasContext *ctx, arg_ld_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_hl_lb_pcr(DisasContext *ctx, arg_hl_lb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_hl_lbu_pcr(DisasContext *ctx, arg_hl_lbu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_hl_lh_pcr(DisasContext *ctx, arg_hl_lh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_hl_lhu_pcr(DisasContext *ctx, arg_hl_lhu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_hl_lw_pcr(DisasContext *ctx, arg_hl_lw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_hl_lwu_pcr(DisasContext *ctx, arg_hl_lwu_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_hl_ld_pcr(DisasContext *ctx, arg_hl_ld_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool linx_store_from_reg(DisasContext *ctx, TCGv addr, TCGv_i64 val,
                                MemOp mop);

static inline int64_t linx_scale_simm(int64_t simm, unsigned shift, bool unscaled)
{
    if (unscaled || shift == 0) {
        return simm;
    }
    return simm * (1ll << shift);
}

static bool linx_hl_load_pair_imm(DisasContext *ctx, int dst0, int dst1, int src_base,
                                  int64_t simm, unsigned shift, bool unscaled,
                                  MemOp mop, int elem_bytes)
{
    int64_t off = linx_scale_simm(simm, shift, unscaled);
    TCGv_i64 addr0 = linx_addr_add_imm(ctx, src_base, off);
    TCGv_i64 addr1 = tcg_temp_new_i64();

    if (linx_debug_local_enabled_p()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx hl_load_pair_imm pc=0x%" VADDR_PRIx
                      " src_base=%d simm=%" PRId64 " shift=%u unscaled=%u elem=%d mop=%u\n",
                      ctx->base.pc_next - ctx->cur_insn_len, src_base, simm,
                      shift, unscaled ? 1u : 0u, elem_bytes, (unsigned)mop);
    }
    tcg_gen_addi_i64(addr1, addr0, elem_bytes);

    if (!linx_load_to_dest(ctx, dst0, linx_addr_from_i64(addr0), mop)) {
        return false;
    }
    return linx_load_to_dest(ctx, dst1, linx_addr_from_i64(addr1), mop);
}

static bool linx_hl_load_imm(DisasContext *ctx, int dst, int src_base,
                             int64_t simm, unsigned shift, bool unscaled,
                             MemOp mop)
{
    int64_t off = linx_scale_simm(simm, shift, unscaled);
    TCGv_i64 addr = linx_addr_add_imm(ctx, src_base, off);
    return linx_load_to_dest(ctx, dst, linx_addr_from_i64(addr), mop);
}

static bool linx_hl_load_writeback(DisasContext *ctx, int dst_val, int dst_wb, int src_base,
                                   int64_t simm, unsigned shift, bool unscaled, bool pre_index,
                                   MemOp mop)
{
    int64_t off = linx_scale_simm(simm, shift, unscaled);
    TCGv_i64 wb = linx_addr_add_imm(ctx, src_base, off);
    TCGv_i64 addr = tcg_temp_new_i64();

    if (pre_index) {
        tcg_gen_mov_i64(addr, wb);
    } else {
        tcg_gen_mov_i64(addr, linx_get_reg(src_base));
    }

    if (!linx_load_to_dest(ctx, dst_val, linx_addr_from_i64(addr), mop)) {
        return false;
    }
    linx_set_dest(dst_wb, wb);
    return true;
}

static bool linx_hl_load_pair_reg(DisasContext *ctx, int dst0, int dst1,
                                  int src_base, int src_off, int src_off_type,
                                  int shamt, MemOp mop, int elem_bytes)
{
    TCGv_i64 addr0 = linx_addr_add_reg(ctx, src_base, src_off, src_off_type, shamt);
    TCGv_i64 addr1 = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr1, addr0, elem_bytes);

    if (!linx_load_to_dest(ctx, dst0, linx_addr_from_i64(addr0), mop)) {
        return false;
    }
    return linx_load_to_dest(ctx, dst1, linx_addr_from_i64(addr1), mop);
}

static bool linx_hl_load_writeback_reg(DisasContext *ctx, int dst_val, int dst_wb,
                                       int src_base, int src_off, int src_off_type,
                                       int shamt, bool pre_index, MemOp mop)
{
    TCGv_i64 wb = linx_addr_add_reg(ctx, src_base, src_off, src_off_type, shamt);
    TCGv_i64 addr = tcg_temp_new_i64();

    if (pre_index) {
        tcg_gen_mov_i64(addr, wb);
    } else {
        tcg_gen_mov_i64(addr, linx_get_reg(src_base));
    }

    if (!linx_load_to_dest(ctx, dst_val, linx_addr_from_i64(addr), mop)) {
        return false;
    }
    linx_set_dest(dst_wb, wb);
    return true;
}

static bool linx_hl_store_pair_imm(DisasContext *ctx, int src0, int src1, int src_base,
                                   int64_t simm, unsigned shift, bool unscaled,
                                   MemOp mop, int elem_bytes)
{
    int64_t off = linx_scale_simm(simm, shift, unscaled);
    TCGv_i64 addr0 = linx_addr_add_imm(ctx, src_base, off);
    TCGv_i64 addr1 = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr1, addr0, elem_bytes);

    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr0), linx_get_reg(src0), mop)) {
        return false;
    }
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr1), linx_get_reg(src1), mop);
}

static bool linx_hl_store_imm(DisasContext *ctx, int src_data, int src_base,
                              int64_t simm, unsigned shift, bool unscaled,
                              MemOp mop)
{
    int64_t off = linx_scale_simm(simm, shift, unscaled);
    TCGv_i64 addr = linx_addr_add_imm(ctx, src_base, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr), linx_get_reg(src_data), mop);
}

static bool linx_hl_store_pair_reg(DisasContext *ctx, int src0, int src1,
                                   int src_base, int src_off, int src_off_type,
                                   int shamt, MemOp mop, int elem_bytes)
{
    TCGv_i64 addr0 = linx_addr_add_reg(ctx, src_base, src_off, src_off_type, shamt);
    TCGv_i64 addr1 = tcg_temp_new_i64();
    tcg_gen_addi_i64(addr1, addr0, elem_bytes);

    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr0), linx_get_reg(src0), mop)) {
        return false;
    }
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr1), linx_get_reg(src1), mop);
}

static bool linx_hl_store_writeback_reg(DisasContext *ctx, int dst_wb, int src_data,
                                        int src_base, int src_off, int src_off_type,
                                        int shamt, bool pre_index, MemOp mop)
{
    TCGv_i64 wb = linx_addr_add_reg(ctx, src_base, src_off, src_off_type, shamt);
    TCGv_i64 addr = tcg_temp_new_i64();

    if (pre_index) {
        tcg_gen_mov_i64(addr, wb);
    } else {
        tcg_gen_mov_i64(addr, linx_get_reg(src_base));
    }

    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr), linx_get_reg(src_data), mop)) {
        return false;
    }
    linx_set_dest(dst_wb, wb);
    return true;
}

static bool linx_hl_swi_writeback(DisasContext *ctx, int dst_wb, int src_data, int src_base,
                                  int64_t simm, bool unscaled, bool pre_index)
{
    int64_t off = linx_scale_simm(simm, 2, unscaled);
    TCGv_i64 wb = linx_addr_add_imm(ctx, src_base, off);
    TCGv_i64 addr = tcg_temp_new_i64();

    if (pre_index) {
        tcg_gen_mov_i64(addr, wb);
    } else {
        tcg_gen_mov_i64(addr, linx_get_reg(src_base));
    }

    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr), linx_get_reg(src_data), MO_UL)) {
        return false;
    }
    linx_set_dest(dst_wb, wb);
    return true;
}

static bool linx_hl_sdi_writeback(DisasContext *ctx, int dst_wb, int src_data, int src_base,
                                  int64_t simm, bool unscaled, bool pre_index)
{
    int64_t off = linx_scale_simm(simm, 3, unscaled);
    TCGv_i64 wb = linx_addr_add_imm(ctx, src_base, off);
    TCGv_i64 addr = tcg_temp_new_i64();

    if (pre_index) {
        tcg_gen_mov_i64(addr, wb);
    } else {
        tcg_gen_mov_i64(addr, linx_get_reg(src_base));
    }

    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr), linx_get_reg(src_data), MO_UQ)) {
        return false;
    }
    linx_set_dest(dst_wb, wb);
    return true;
}

#define DEFINE_HL_LOAD_IMM(NAME, MOP, SHIFT, UNSCALED) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_load_imm(ctx, a->RegDst, a->SrcL, (int64_t)a->simm, SHIFT, UNSCALED, MOP); \
}

#define DEFINE_HL_LOAD_PAIR_IMM(NAME, MOP, SHIFT, UNSCALED, ELEM_BYTES) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL, \
                                 (int64_t)a->simm, SHIFT, UNSCALED, MOP, ELEM_BYTES); \
}

#define DEFINE_HL_LOAD_WB_IMM(NAME, MOP, SHIFT, UNSCALED, PRE_INDEX) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL, \
                                  (int64_t)a->simm, SHIFT, UNSCALED, PRE_INDEX, MOP); \
}

#define DEFINE_HL_LOAD_PAIR_REG(NAME, MOP, ELEM_BYTES) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_load_pair_reg(ctx, a->RegDst0, a->RegDst1, a->SrcL, a->SrcR, \
                                 a->SrcRType, a->shamt, MOP, ELEM_BYTES); \
}

#define DEFINE_HL_LOAD_WB_REG(NAME, MOP, PRE_INDEX) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_load_writeback_reg(ctx, a->RegDst0, a->RegDst1, a->SrcL, a->SrcR, \
                                      a->SrcRType, a->shamt, PRE_INDEX, MOP); \
}

#define DEFINE_HL_STORE_IMM(NAME, MOP, SHIFT, UNSCALED) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_store_imm(ctx, a->SrcD, a->SrcR, (int64_t)a->simm, SHIFT, UNSCALED, MOP); \
}

#define DEFINE_HL_STORE_PAIR_IMM(NAME, MOP, SHIFT, UNSCALED, ELEM_BYTES) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_store_pair_imm(ctx, a->SrcD, a->SrcD1, a->SrcR, \
                                  (int64_t)a->simm, SHIFT, UNSCALED, MOP, ELEM_BYTES); \
}

#define DEFINE_HL_STORE_WB_IMM(NAME, MOP, SHIFT, UNSCALED, PRE_INDEX) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    int64_t off = linx_scale_simm((int64_t)a->simm, SHIFT, UNSCALED); \
    TCGv_i64 wb = linx_addr_add_imm(ctx, a->SrcR, off); \
    TCGv_i64 addr = tcg_temp_new_i64(); \
    if (PRE_INDEX) { \
        tcg_gen_mov_i64(addr, wb); \
    } else { \
        tcg_gen_mov_i64(addr, linx_get_reg(a->SrcR)); \
    } \
    if (!linx_store_from_reg(ctx, linx_addr_from_i64(addr), linx_get_reg(a->SrcD), MOP)) { \
        return false; \
    } \
    linx_set_dest(a->RegDst1, wb); \
    return true; \
}

#define DEFINE_HL_STORE_PAIR_REG(NAME, MOP, ELEM_BYTES) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_store_pair_reg(ctx, a->SrcD, a->SrcD1, a->SrcL, a->SrcR, \
                                  a->SrcRType, a->shamt, MOP, ELEM_BYTES); \
}

#define DEFINE_HL_STORE_WB_REG(NAME, MOP, PRE_INDEX) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    return linx_hl_store_writeback_reg(ctx, a->RegDst1, a->SrcD, a->SrcL, a->SrcR, \
                                       a->SrcRType, a->shamt, PRE_INDEX, MOP); \
}

DEFINE_HL_LOAD_WB_REG(hl_lb_po, MO_SB, false)
DEFINE_HL_LOAD_WB_REG(hl_lb_pr, MO_SB, true)
DEFINE_HL_LOAD_IMM(hl_lbi, MO_SB, 0, false)
DEFINE_HL_LOAD_WB_IMM(hl_lbi_po, MO_SB, 0, false, false)
DEFINE_HL_LOAD_WB_IMM(hl_lbi_pr, MO_SB, 0, false, true)
DEFINE_HL_LOAD_PAIR_IMM(hl_lbip, MO_SB, 0, false, 1)
DEFINE_HL_LOAD_PAIR_REG(hl_lbp, MO_SB, 1)

DEFINE_HL_LOAD_WB_REG(hl_lbu_po, MO_UB, false)
DEFINE_HL_LOAD_WB_REG(hl_lbu_pr, MO_UB, true)
DEFINE_HL_LOAD_IMM(hl_lbui, MO_UB, 0, false)
DEFINE_HL_LOAD_WB_IMM(hl_lbui_po, MO_UB, 0, false, false)
DEFINE_HL_LOAD_WB_IMM(hl_lbui_pr, MO_UB, 0, false, true)
DEFINE_HL_LOAD_PAIR_IMM(hl_lbuip, MO_UB, 0, false, 1)
DEFINE_HL_LOAD_PAIR_REG(hl_lbup, MO_UB, 1)

DEFINE_HL_LOAD_WB_REG(hl_lh_po, MO_SW, false)
DEFINE_HL_LOAD_WB_REG(hl_lh_pr, MO_SW, true)
DEFINE_HL_LOAD_IMM(hl_lhi, MO_SW, 1, false)
DEFINE_HL_LOAD_WB_IMM(hl_lhi_po, MO_SW, 1, false, false)
DEFINE_HL_LOAD_WB_IMM(hl_lhi_pr, MO_SW, 1, false, true)
DEFINE_HL_LOAD_IMM(hl_lhi_u, MO_SW, 1, true)
DEFINE_HL_LOAD_WB_IMM(hl_lhi_upo, MO_SW, 1, true, false)
DEFINE_HL_LOAD_WB_IMM(hl_lhi_upr, MO_SW, 1, true, true)
DEFINE_HL_LOAD_PAIR_IMM(hl_lhip, MO_SW, 1, false, 2)
DEFINE_HL_LOAD_PAIR_IMM(hl_lhip_u, MO_SW, 1, true, 2)
DEFINE_HL_LOAD_PAIR_REG(hl_lhp, MO_SW, 2)

DEFINE_HL_LOAD_WB_REG(hl_lhu_po, MO_UW, false)
DEFINE_HL_LOAD_WB_REG(hl_lhu_pr, MO_UW, true)
DEFINE_HL_LOAD_IMM(hl_lhui, MO_UW, 1, false)
DEFINE_HL_LOAD_WB_IMM(hl_lhui_po, MO_UW, 1, false, false)
DEFINE_HL_LOAD_WB_IMM(hl_lhui_pr, MO_UW, 1, false, true)
DEFINE_HL_LOAD_IMM(hl_lhui_u, MO_UW, 1, true)
DEFINE_HL_LOAD_WB_IMM(hl_lhui_upo, MO_UW, 1, true, false)
DEFINE_HL_LOAD_WB_IMM(hl_lhui_upr, MO_UW, 1, true, true)
DEFINE_HL_LOAD_PAIR_IMM(hl_lhuip, MO_UW, 1, false, 2)
DEFINE_HL_LOAD_PAIR_IMM(hl_lhuip_u, MO_UW, 1, true, 2)
DEFINE_HL_LOAD_PAIR_REG(hl_lhup, MO_UW, 2)

DEFINE_HL_LOAD_WB_REG(hl_ld_po, MO_UQ, false)
DEFINE_HL_LOAD_WB_REG(hl_ld_pr, MO_UQ, true)
DEFINE_HL_LOAD_IMM(hl_ldi, MO_UQ, 3, false)
DEFINE_HL_LOAD_IMM(hl_ldi_u, MO_UQ, 3, true)
DEFINE_HL_LOAD_PAIR_REG(hl_ldp, MO_UQ, 8)

DEFINE_HL_LOAD_WB_REG(hl_lw_po, MO_SL, false)
DEFINE_HL_LOAD_WB_REG(hl_lw_pr, MO_SL, true)
DEFINE_HL_LOAD_IMM(hl_lwi, MO_SL, 2, false)
DEFINE_HL_LOAD_IMM(hl_lwi_u, MO_SL, 2, true)
DEFINE_HL_LOAD_PAIR_REG(hl_lwp, MO_SL, 4)

DEFINE_HL_LOAD_WB_REG(hl_lwu_po, MO_UL, false)
DEFINE_HL_LOAD_WB_REG(hl_lwu_pr, MO_UL, true)
DEFINE_HL_LOAD_IMM(hl_lwui, MO_UL, 2, false)
DEFINE_HL_LOAD_IMM(hl_lwui_u, MO_UL, 2, true)
DEFINE_HL_LOAD_PAIR_REG(hl_lwup, MO_UL, 4)

DEFINE_HL_STORE_WB_REG(hl_sb_po, MO_UB, false)
DEFINE_HL_STORE_WB_REG(hl_sb_pr, MO_UB, true)
DEFINE_HL_STORE_IMM(hl_sbi, MO_UB, 0, false)
DEFINE_HL_STORE_WB_IMM(hl_sbi_po, MO_UB, 0, false, false)
DEFINE_HL_STORE_WB_IMM(hl_sbi_pr, MO_UB, 0, false, true)
DEFINE_HL_STORE_PAIR_IMM(hl_sbip, MO_UB, 0, false, 1)
DEFINE_HL_STORE_PAIR_REG(hl_sbp, MO_UB, 1)

DEFINE_HL_STORE_WB_REG(hl_sh_po, MO_UW, false)
DEFINE_HL_STORE_WB_REG(hl_sh_pr, MO_UW, true)
DEFINE_HL_STORE_WB_REG(hl_sh_upo, MO_UW, false)
DEFINE_HL_STORE_WB_REG(hl_sh_upr, MO_UW, true)
DEFINE_HL_STORE_IMM(hl_shi, MO_UW, 1, false)
DEFINE_HL_STORE_WB_IMM(hl_shi_po, MO_UW, 1, false, false)
DEFINE_HL_STORE_WB_IMM(hl_shi_pr, MO_UW, 1, false, true)
DEFINE_HL_STORE_IMM(hl_shi_u, MO_UW, 1, true)
DEFINE_HL_STORE_WB_IMM(hl_shi_upo, MO_UW, 1, true, false)
DEFINE_HL_STORE_WB_IMM(hl_shi_upr, MO_UW, 1, true, true)
DEFINE_HL_STORE_PAIR_IMM(hl_ship, MO_UW, 1, false, 2)
DEFINE_HL_STORE_PAIR_IMM(hl_ship_u, MO_UW, 1, true, 2)
DEFINE_HL_STORE_PAIR_REG(hl_shp, MO_UW, 2)
DEFINE_HL_STORE_PAIR_REG(hl_shp_u, MO_UW, 2)

DEFINE_HL_STORE_WB_REG(hl_sd_po, MO_UQ, false)
DEFINE_HL_STORE_WB_REG(hl_sd_pr, MO_UQ, true)
DEFINE_HL_STORE_WB_REG(hl_sd_upo, MO_UQ, false)
DEFINE_HL_STORE_WB_REG(hl_sd_upr, MO_UQ, true)
DEFINE_HL_STORE_IMM(hl_sdi, MO_UQ, 3, false)
DEFINE_HL_STORE_IMM(hl_sdi_u, MO_UQ, 3, true)
DEFINE_HL_STORE_PAIR_REG(hl_sdp, MO_UQ, 8)
DEFINE_HL_STORE_PAIR_REG(hl_sdp_u, MO_UQ, 8)

DEFINE_HL_STORE_WB_REG(hl_sw_po, MO_UL, false)
DEFINE_HL_STORE_WB_REG(hl_sw_pr, MO_UL, true)
DEFINE_HL_STORE_WB_REG(hl_sw_upo, MO_UL, false)
DEFINE_HL_STORE_WB_REG(hl_sw_upr, MO_UL, true)
DEFINE_HL_STORE_IMM(hl_swi, MO_UL, 2, false)
DEFINE_HL_STORE_IMM(hl_swi_u, MO_UL, 2, true)
DEFINE_HL_STORE_PAIR_REG(hl_swp, MO_UL, 4)
DEFINE_HL_STORE_PAIR_REG(hl_swp_u, MO_UL, 4)

static bool trans_hl_ldip(DisasContext *ctx, arg_hl_ldip *a)
{
    return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                 (int64_t)a->simm, 3, false, MO_UQ, 8);
}

static bool trans_hl_ldip_u(DisasContext *ctx, arg_hl_ldip_u *a)
{
    return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                 (int64_t)a->simm, 3, true, MO_UQ, 8);
}

static bool trans_hl_lwuip(DisasContext *ctx, arg_hl_lwuip *a)
{
    return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                 (int64_t)a->simm, 2, false, MO_UL, 4);
}

static bool trans_hl_lwuip_u(DisasContext *ctx, arg_hl_lwuip_u *a)
{
    return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                 (int64_t)a->simm, 2, true, MO_UL, 4);
}

static bool trans_hl_lwip(DisasContext *ctx, arg_hl_lwip *a)
{
    return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                 (int64_t)a->simm, 2, false, MO_SL, 4);
}

static bool trans_hl_lwip_u(DisasContext *ctx, arg_hl_lwip_u *a)
{
    return linx_hl_load_pair_imm(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                 (int64_t)a->simm, 2, true, MO_SL, 4);
}

static bool trans_hl_lwui_po(DisasContext *ctx, arg_hl_lwui_po *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, false, false, MO_UL);
}

static bool trans_hl_lwui_pr(DisasContext *ctx, arg_hl_lwui_pr *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, false, true, MO_UL);
}

static bool trans_hl_lwui_upo(DisasContext *ctx, arg_hl_lwui_upo *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, true, false, MO_UL);
}

static bool trans_hl_lwui_upr(DisasContext *ctx, arg_hl_lwui_upr *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, true, true, MO_UL);
}

static bool trans_hl_lwi_po(DisasContext *ctx, arg_hl_lwi_po *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, false, false, MO_SL);
}

static bool trans_hl_lwi_pr(DisasContext *ctx, arg_hl_lwi_pr *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, false, true, MO_SL);
}

static bool trans_hl_lwi_upo(DisasContext *ctx, arg_hl_lwi_upo *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, true, false, MO_SL);
}

static bool trans_hl_lwi_upr(DisasContext *ctx, arg_hl_lwi_upr *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 2, true, true, MO_SL);
}

static bool trans_hl_ldi_po(DisasContext *ctx, arg_hl_ldi_po *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 3, false, false, MO_UQ);
}

static bool trans_hl_ldi_pr(DisasContext *ctx, arg_hl_ldi_pr *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 3, false, true, MO_UQ);
}

static bool trans_hl_ldi_upo(DisasContext *ctx, arg_hl_ldi_upo *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 3, true, false, MO_UQ);
}

static bool trans_hl_ldi_upr(DisasContext *ctx, arg_hl_ldi_upr *a)
{
    return linx_hl_load_writeback(ctx, a->RegDst0, a->RegDst1, a->SrcL,
                                  (int64_t)a->simm, 3, true, true, MO_UQ);
}

static bool trans_hl_swip(DisasContext *ctx, arg_hl_swip *a)
{
    return linx_hl_store_pair_imm(ctx, a->SrcD, a->SrcD1, a->SrcR,
                                  (int64_t)a->simm, 2, false, MO_UL, 4);
}

static bool trans_hl_swip_u(DisasContext *ctx, arg_hl_swip_u *a)
{
    return linx_hl_store_pair_imm(ctx, a->SrcD, a->SrcD1, a->SrcR,
                                  (int64_t)a->simm, 2, true, MO_UL, 4);
}

static bool trans_hl_sdip(DisasContext *ctx, arg_hl_sdip *a)
{
    return linx_hl_store_pair_imm(ctx, a->SrcD, a->SrcD1, a->SrcR,
                                  (int64_t)a->simm, 3, false, MO_UQ, 8);
}

static bool trans_hl_sdip_u(DisasContext *ctx, arg_hl_sdip_u *a)
{
    return linx_hl_store_pair_imm(ctx, a->SrcD, a->SrcD1, a->SrcR,
                                  (int64_t)a->simm, 3, true, MO_UQ, 8);
}

static bool trans_hl_swi_po(DisasContext *ctx, arg_hl_swi_po *a)
{
    return linx_hl_swi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, false, false);
}

static bool trans_hl_swi_pr(DisasContext *ctx, arg_hl_swi_pr *a)
{
    return linx_hl_swi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, false, true);
}

static bool trans_hl_swi_upo(DisasContext *ctx, arg_hl_swi_upo *a)
{
    return linx_hl_swi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, true, false);
}

static bool trans_hl_swi_upr(DisasContext *ctx, arg_hl_swi_upr *a)
{
    return linx_hl_swi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, true, true);
}

static bool trans_hl_sdi_po(DisasContext *ctx, arg_hl_sdi_po *a)
{
    return linx_hl_sdi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, false, false);
}

static bool trans_hl_sdi_pr(DisasContext *ctx, arg_hl_sdi_pr *a)
{
    return linx_hl_sdi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, false, true);
}

static bool trans_hl_sdi_upo(DisasContext *ctx, arg_hl_sdi_upo *a)
{
    return linx_hl_sdi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, true, false);
}

static bool trans_hl_sdi_upr(DisasContext *ctx, arg_hl_sdi_upr *a)
{
    return linx_hl_sdi_writeback(ctx, a->RegDst1, a->SrcD, a->SrcR,
                                 (int64_t)a->simm, true, true);
}

static bool trans_lwui(DisasContext *ctx, arg_lwui *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_ldi(DisasContext *ctx, arg_ldi *a)
{
    int64_t off = (int64_t)a->simm12 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_lb(DisasContext *ctx, arg_lb *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SB);
}

static bool trans_lbu(DisasContext *ctx, arg_lbu *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UB);
}

static bool trans_lh(DisasContext *ctx, arg_lh *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lhu(DisasContext *ctx, arg_lhu *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_lw(DisasContext *ctx, arg_lw *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_lwu(DisasContext *ctx, arg_lwu *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_ld(DisasContext *ctx, arg_ld *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool linx_store_from_reg(DisasContext *ctx, TCGv addr, TCGv_i64 val,
                                MemOp mop)
{
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->base.tb->flags & LINX_TB_FLAG_DBG_ACTIVE) {
        const unsigned size = memop_size(mop);
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        gen_helper_linx_dbg_check_store(tcg_env, tcg_constant_i64(pc), addr64,
                                        tcg_constant_i32((int32_t)size));
    }

    if (linx_commit_trace_enabled) {
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        tcg_gen_movi_i32(cpu_trace_mem_valid, 1);
        tcg_gen_movi_i32(cpu_trace_mem_is_store, 1);
        tcg_gen_mov_i64(cpu_trace_mem_addr, addr64);
        tcg_gen_mov_i64(cpu_trace_mem_wdata, val);
        tcg_gen_movi_i64(cpu_trace_mem_rdata, 0);
        tcg_gen_movi_i32(cpu_trace_mem_size, (int32_t)memop_size(mop));
    }

    if (linx_mem_trace_translate_enabled && linx_mem_trace_translate_stores) {
        const unsigned size = memop_size(mop);
        TCGv_i64 addr64;
#if TARGET_LONG_BITS == 32
        addr64 = tcg_temp_new_i64();
        tcg_gen_extu_tl_i64(addr64, addr);
#else
        addr64 = (TCGv_i64)addr;
#endif
        linx_gen_mem_trace_probe(true, false, pc, addr64, size, val);
    }

    linx_lr_invalidate();
    tcg_gen_qemu_st_i64(val, addr, ctx->mem_idx, mop | linx_mo_endian());
    return true;
}

static bool trans_sbi(DisasContext *ctx, arg_sbi *a)
{
    int64_t off = (int64_t)a->simm12;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UB);
}

static bool trans_shi(DisasContext *ctx, arg_shi *a)
{
    int64_t off = (int64_t)a->simm12 * 2;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_swi(DisasContext *ctx, arg_swi *a)
{
    int64_t off = (int64_t)a->simm12 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UL);
}

static inline int32_t linx_decode_pcr17_store_imm(uint32_t enc_imm)
{
    /*
     * lld encodePcr17Store packs simm17 as:
     *   simm[11:0]  -> insn[31:20]
     *   simm[16:12] -> insn[11:7]
     *
     * decode-insn32 currently exposes these bits in the opposite concatenation
     * order for arg_*_pcr::imm, so remap before sign-extension.
     */
    uint32_t uimm = ((enc_imm & 0x1fu) << 12) | ((enc_imm >> 5) & 0x0fffu);
    return (int32_t)(uimm << 15) >> 15;
}

static bool trans_sb_pcr(DisasContext *ctx, arg_sb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UB);
}

static bool trans_sh_pcr(DisasContext *ctx, arg_sh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_sw_pcr(DisasContext *ctx, arg_sw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UL);
}

static bool trans_sd_pcr(DisasContext *ctx, arg_sd_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    const int32_t simm17 = linx_decode_pcr17_store_imm(a->imm);
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)simm17);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UQ);
}

static bool trans_hl_sb_pcr(DisasContext *ctx, arg_hl_sb_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UB);
}

static bool trans_hl_sh_pcr(DisasContext *ctx, arg_hl_sh_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_hl_sw_pcr(DisasContext *ctx, arg_hl_sw_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UL);
}

static bool trans_hl_sd_pcr(DisasContext *ctx, arg_hl_sd_pcr *a)
{
    vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr addr = (vaddr)((int64_t)pc + (int64_t)a->simm);
    TCGv_i64 addr64 = tcg_temp_new_i64();
    tcg_gen_movi_i64(addr64, addr);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UQ);
}

static bool trans_sdi(DisasContext *ctx, arg_sdi *a)
{
    int64_t off = (int64_t)a->simm12 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UQ);
}

static bool trans_sb(DisasContext *ctx, arg_sb *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UB);
}

static bool trans_sh(DisasContext *ctx, arg_sh *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 1);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UW);
}

static bool trans_sw(DisasContext *ctx, arg_sw *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 2);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UL);
}

static bool trans_sd(DisasContext *ctx, arg_sd *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 3);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UQ);
}

static bool trans_sh_u(DisasContext *ctx, arg_sh_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UW);
}

static bool trans_sw_u(DisasContext *ctx, arg_sw_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UL);
}

static bool trans_sd_u(DisasContext *ctx, arg_sd_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, 0);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcD), MO_UQ);
}

static bool trans_c_lwi(DisasContext *ctx, arg_c_lwi *a)
{
    int64_t off = (int64_t)a->simm5 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, 31, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_c_ldi(DisasContext *ctx, arg_c_ldi *a)
{
    int64_t off = (int64_t)a->simm5 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_load_to_dest(ctx, 31, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_c_swi(DisasContext *ctx, arg_c_swi *a)
{
    int64_t off = (int64_t)a->simm5 * 4;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), cpu_tq[0], MO_UL);
}

static bool trans_c_sdi(DisasContext *ctx, arg_c_sdi *a)
{
    int64_t off = (int64_t)a->simm5 * 8;
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, off);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), cpu_tq[0], MO_UQ);
}

/* ===================== v0.3 SIMT/Vector (64-bit) ===================== */

static bool linx_require_in_body(DisasContext *ctx)
{
    if (!ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_HEADER, 0);
    }
    return true;
}

static bool trans_v_add(DisasContext *ctx, arg_v_add *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_add(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR),
                          tcg_constant_i32((int32_t)a->srctype),
                          tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_sub(DisasContext *ctx, arg_v_sub *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_sub(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR),
                          tcg_constant_i32((int32_t)a->srctype),
                          tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_addi(DisasContext *ctx, arg_v_addi *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_addi(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->uimm));
    return true;
}

static bool trans_v_subi(DisasContext *ctx, arg_v_subi *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_subi(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->uimm));
    return true;
}

static bool trans_v_and(DisasContext *ctx, arg_v_and *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_and(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR),
                          tcg_constant_i32((int32_t)a->srctype),
                          tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_andi(DisasContext *ctx, arg_v_andi *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_andi(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_or(DisasContext *ctx, arg_v_or *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_or(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                         tcg_constant_i32((int32_t)a->SrcL),
                         tcg_constant_i32((int32_t)a->SrcR),
                         tcg_constant_i32((int32_t)a->srctype),
                         tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_ori(DisasContext *ctx, arg_v_ori *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_ori(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_xor(DisasContext *ctx, arg_v_xor *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_xor(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR),
                          tcg_constant_i32((int32_t)a->srctype),
                          tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_xori(DisasContext *ctx, arg_v_xori *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_xori(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_mul(DisasContext *ctx, arg_v_mul *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_mul(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_sll(DisasContext *ctx, arg_v_sll *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_sll(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_slli(DisasContext *ctx, arg_v_slli *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_slli(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_srl(DisasContext *ctx, arg_v_srl *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_srl(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_srli(DisasContext *ctx, arg_v_srli *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_srli(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_sra(DisasContext *ctx, arg_v_sra *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_sra(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_srai(DisasContext *ctx, arg_v_srai *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_srai(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->shamt));
    return true;
}

static bool trans_v_max(DisasContext *ctx, arg_v_max *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_max(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_min(DisasContext *ctx, arg_v_min *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_min(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_madd(DisasContext *ctx, arg_v_madd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_madd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR),
                           tcg_constant_i32((int32_t)a->SrcD));
    return true;
}

static bool trans_v_div(DisasContext *ctx, arg_v_div *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_div(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_rem(DisasContext *ctx, arg_v_rem *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rem(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_eq(DisasContext *ctx, arg_v_cmp_eq *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_eq(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_ne(DisasContext *ctx, arg_v_cmp_ne *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ne(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_lt(DisasContext *ctx, arg_v_cmp_lt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_lt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_ltu(DisasContext *ctx, arg_v_cmp_ltu *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ltu(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_ge(DisasContext *ctx, arg_v_cmp_ge *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ge(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_geu(DisasContext *ctx, arg_v_cmp_geu *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_geu(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_and(DisasContext *ctx, arg_v_cmp_and *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_and(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_or(DisasContext *ctx, arg_v_cmp_or *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_or(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_cmp_andi(DisasContext *ctx, arg_v_cmp_andi *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_andi(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_cmp_eqi(DisasContext *ctx, arg_v_cmp_eqi *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_eqi(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_cmp_gei(DisasContext *ctx, arg_v_cmp_gei *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_gei(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_cmp_geui(DisasContext *ctx, arg_v_cmp_geui *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_geui(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->uimm));
    return true;
}

static bool trans_v_cmp_lti(DisasContext *ctx, arg_v_cmp_lti *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_lti(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_cmp_ltui(DisasContext *ctx, arg_v_cmp_ltui *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ltui(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->uimm));
    return true;
}

static bool trans_v_cmp_nei(DisasContext *ctx, arg_v_cmp_nei *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_nei(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_cmp_ori(DisasContext *ctx, arg_v_cmp_ori *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_cmp_ori(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                              tcg_constant_i32((int32_t)a->SrcL),
                              tcg_constant_i32((int32_t)a->simm));
    return true;
}

static bool trans_v_bxs(DisasContext *ctx, arg_v_bxs *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_bxs(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->imms),
                          tcg_constant_i32((int32_t)a->imml));
    return true;
}

static bool trans_v_bxu(DisasContext *ctx, arg_v_bxu *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_bxu(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->imms),
                          tcg_constant_i32((int32_t)a->imml));
    return true;
}

static bool trans_v_bic(DisasContext *ctx, arg_v_bic *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_bic(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->imms),
                          tcg_constant_i32((int32_t)a->imml));
    return true;
}

static bool trans_v_bis(DisasContext *ctx, arg_v_bis *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_bis(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->imms),
                          tcg_constant_i32((int32_t)a->imml));
    return true;
}

static bool trans_v_ctz(DisasContext *ctx, arg_v_ctz *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_ctz(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->imms),
                          tcg_constant_i32((int32_t)a->imml));
    return true;
}

static bool trans_v_clz(DisasContext *ctx, arg_v_clz *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_clz(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->imms),
                          tcg_constant_i32((int32_t)a->imml));
    return true;
}

static bool trans_v_bcnt(DisasContext *ctx, arg_v_bcnt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_bcnt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->imms),
                           tcg_constant_i32((int32_t)a->imml));
    return true;
}

static bool trans_v_feq(DisasContext *ctx, arg_v_feq *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_feq(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fne(DisasContext *ctx, arg_v_fne *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fne(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_flt(DisasContext *ctx, arg_v_flt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_flt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fge(DisasContext *ctx, arg_v_fge *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fge(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_feqs(DisasContext *ctx, arg_v_feqs *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_feqs(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fnes(DisasContext *ctx, arg_v_fnes *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fnes(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_flts(DisasContext *ctx, arg_v_flts *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_flts(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fges(DisasContext *ctx, arg_v_fges *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fges(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_csel(DisasContext *ctx, arg_v_csel *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_csel(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           /* Predicate lives in the SrcD field for V.CSEL. */
                           tcg_constant_i32((int32_t)a->SrcD),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR),
                           tcg_constant_i32((int32_t)a->srctype));
    return true;
}

static bool trans_v_psel(DisasContext *ctx, arg_v_psel *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_psel(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcP),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR),
                           tcg_constant_i32((int32_t)a->srctype));
    return true;
}

static bool trans_v_fadd(DisasContext *ctx, arg_v_fadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fsub(DisasContext *ctx, arg_v_fsub *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fsub(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fmul(DisasContext *ctx, arg_v_fmul *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fmul(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fdiv(DisasContext *ctx, arg_v_fdiv *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fdiv(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fabs(DisasContext *ctx, arg_v_fabs *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fabs(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_fmadd(DisasContext *ctx, arg_v_fmadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fmadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL),
                            tcg_constant_i32((int32_t)a->SrcR),
                            tcg_constant_i32((int32_t)a->SrcA));
    return true;
}

static bool trans_v_fmsub(DisasContext *ctx, arg_v_fmsub *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fmsub(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL),
                            tcg_constant_i32((int32_t)a->SrcR),
                            tcg_constant_i32((int32_t)a->SrcA));
    return true;
}

static bool trans_v_fnmadd(DisasContext *ctx, arg_v_fnmadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fnmadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR),
                             tcg_constant_i32((int32_t)a->SrcA));
    return true;
}

static bool trans_v_fnmsub(DisasContext *ctx, arg_v_fnmsub *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fnmsub(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL),
                             tcg_constant_i32((int32_t)a->SrcR),
                             tcg_constant_i32((int32_t)a->SrcA));
    return true;
}

static bool trans_v_fsqrt(DisasContext *ctx, arg_v_fsqrt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fsqrt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_frecip(DisasContext *ctx, arg_v_frecip *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_frecip(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_fexp(DisasContext *ctx, arg_v_fexp *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fexp(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_fclass(DisasContext *ctx, arg_v_fclass *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fclass(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_fcvt(DisasContext *ctx, arg_v_fcvt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fcvt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->DstType),
                           tcg_constant_i32((int32_t)a->SrcType));
    return true;
}

static bool trans_v_fcvti(DisasContext *ctx, arg_v_fcvti *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fcvti(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL),
                            tcg_constant_i32((int32_t)a->DstType),
                            tcg_constant_i32((int32_t)a->SrcType));
    return true;
}

static bool trans_v_fmax(DisasContext *ctx, arg_v_fmax *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fmax(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_fmin(DisasContext *ctx, arg_v_fmin *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_fmin(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->SrcR));
    return true;
}

static bool trans_v_icvt(DisasContext *ctx, arg_v_icvt *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_icvt(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL),
                           tcg_constant_i32((int32_t)a->DstType),
                           tcg_constant_i32((int32_t)a->SrcType));
    return true;
}

static bool trans_v_icvtf(DisasContext *ctx, arg_v_icvtf *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_icvtf(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL),
                            tcg_constant_i32((int32_t)a->DstType),
                            tcg_constant_i32((int32_t)a->SrcType));
    return true;
}

static bool trans_v_rdadd(DisasContext *ctx, arg_v_rdadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rev(DisasContext *ctx, arg_v_rev *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rev(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                          tcg_constant_i32((int32_t)a->SrcL),
                          tcg_constant_i32((int32_t)a->imml),
                          tcg_constant_i32((int32_t)a->imms));
    return true;
}

static bool trans_v_rdand(DisasContext *ctx, arg_v_rdand *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdand(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdfadd(DisasContext *ctx, arg_v_rdfadd *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdfadd(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdor(DisasContext *ctx, arg_v_rdor *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdor(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                           tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdxor(DisasContext *ctx, arg_v_rdxor *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdxor(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdmax(DisasContext *ctx, arg_v_rdmax *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdmax(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdmin(DisasContext *ctx, arg_v_rdmin *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdmin(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                            tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdfmax(DisasContext *ctx, arg_v_rdfmax *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdfmax(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_rdfmin(DisasContext *ctx, arg_v_rdfmin *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    gen_helper_linx_v_rdfmin(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                             tcg_constant_i32((int32_t)a->SrcL));
    return true;
}

static bool trans_v_lw(DisasContext *ctx, arg_v_lw *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_lw_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_lw_brg(DisasContext *ctx, arg_v_lw_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_lw_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_lw_brg(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_lb(DisasContext *ctx, arg_v_lb *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_lb_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_lb_brg(DisasContext *ctx, arg_v_lb_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_lb_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_lb_brg(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_lh(DisasContext *ctx, arg_v_lh *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_lh_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_lh_brg(DisasContext *ctx, arg_v_lh_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_lh_local(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_lh_brg(tcg_env, tcg_constant_i32((int32_t)a->RegDst),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_lbu(DisasContext *ctx, arg_v_lbu *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_lbu_local(tcg_env,
                                tcg_constant_i32((int32_t)a->RegDst),
                                tcg_constant_i32((int32_t)a->SrcL),
                                tcg_constant_i32((int32_t)a->SrcR),
                                tcg_constant_i32((int32_t)a->shamt),
                                tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_lbu_brg(DisasContext *ctx, arg_v_lbu_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_lbu_local(tcg_env,
                                    tcg_constant_i32((int32_t)a->RegDst),
                                    tcg_constant_i32((int32_t)a->SrcL),
                                    tcg_constant_i32((int32_t)a->SrcR),
                                    tcg_constant_i32((int32_t)a->shamt),
                                    tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_lbu_brg(tcg_env,
                                  tcg_constant_i32((int32_t)a->RegDst),
                                  tcg_constant_i32((int32_t)a->SrcL),
                                  tcg_constant_i32((int32_t)a->SrcR),
                                  tcg_constant_i32((int32_t)a->shamt),
                                  tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_lhu(DisasContext *ctx, arg_v_lhu *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_lhu_local(tcg_env,
                                tcg_constant_i32((int32_t)a->RegDst),
                                tcg_constant_i32((int32_t)a->SrcL),
                                tcg_constant_i32((int32_t)a->SrcR),
                                tcg_constant_i32((int32_t)a->shamt),
                                tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_lhu_brg(DisasContext *ctx, arg_v_lhu_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_lhu_local(tcg_env,
                                    tcg_constant_i32((int32_t)a->RegDst),
                                    tcg_constant_i32((int32_t)a->SrcL),
                                    tcg_constant_i32((int32_t)a->SrcR),
                                    tcg_constant_i32((int32_t)a->shamt),
                                    tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_lhu_brg(tcg_env,
                                  tcg_constant_i32((int32_t)a->RegDst),
                                  tcg_constant_i32((int32_t)a->SrcL),
                                  tcg_constant_i32((int32_t)a->SrcR),
                                  tcg_constant_i32((int32_t)a->shamt),
                                  tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_sb(DisasContext *ctx, arg_v_sb *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_sb_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_sb_brg(DisasContext *ctx, arg_v_sb_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_sb_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_sb_brg(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_sh(DisasContext *ctx, arg_v_sh *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_sh_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_sh_brg(DisasContext *ctx, arg_v_sh_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_sh_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_sh_brg(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

static bool trans_v_sw(DisasContext *ctx, arg_v_sw *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    gen_helper_linx_v_sw_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                               tcg_constant_i32((int32_t)a->SrcL),
                               tcg_constant_i32((int32_t)a->SrcR),
                               tcg_constant_i32((int32_t)a->shamt),
                               tcg_constant_i32((int32_t)a->l));
    return true;
}

static bool trans_v_sw_brg(DisasContext *ctx, arg_v_sw_brg *a)
{
    if (!linx_require_in_body(ctx)) {
        return false;
    }
    (void)a->c;
    if (a->l) {
        gen_helper_linx_v_sw_local(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                   tcg_constant_i32((int32_t)a->SrcL),
                                   tcg_constant_i32((int32_t)a->SrcR),
                                   tcg_constant_i32((int32_t)a->shamt),
                                   tcg_constant_i32(1));
    } else {
        gen_helper_linx_v_sw_brg(tcg_env, tcg_constant_i32((int32_t)a->SrcD),
                                 tcg_constant_i32((int32_t)a->SrcL),
                                 tcg_constant_i32((int32_t)a->SrcR),
                                 tcg_constant_i32((int32_t)a->shamt),
                                 tcg_constant_i32(0));
    }
    return true;
}

/*
 * The locked LinxISA 0.58 catalog contains an optional long-vector memory/atomic/
 * shuffle profile that the current QEMU execution engine does not advertise.
 * Decode every exact form so metadata and disassembly agree, then raise the
 * architectural capability fault before any guest-visible mutation.
 */
#define TRANS_V058_CAPABILITY_FAULT(name)                                   \
    static bool trans_##name(DisasContext *ctx, arg_##name *a)              \
    {                                                                        \
        (void)a;                                                             \
        return linx_illegal(ctx);                                            \
    }

TRANS_V058_CAPABILITY_FAULT(v_lbi)
TRANS_V058_CAPABILITY_FAULT(v_lbi_brg)
TRANS_V058_CAPABILITY_FAULT(v_lbui)
TRANS_V058_CAPABILITY_FAULT(v_lbui_brg)
TRANS_V058_CAPABILITY_FAULT(v_ld)
TRANS_V058_CAPABILITY_FAULT(v_ld_add)
TRANS_V058_CAPABILITY_FAULT(v_ld_and)
TRANS_V058_CAPABILITY_FAULT(v_ld_brg)
TRANS_V058_CAPABILITY_FAULT(v_ld_max)
TRANS_V058_CAPABILITY_FAULT(v_ld_min)
TRANS_V058_CAPABILITY_FAULT(v_ld_or)
TRANS_V058_CAPABILITY_FAULT(v_ld_xor)
TRANS_V058_CAPABILITY_FAULT(v_ldi)
TRANS_V058_CAPABILITY_FAULT(v_ldi_brg)
TRANS_V058_CAPABILITY_FAULT(v_ldi_u)
TRANS_V058_CAPABILITY_FAULT(v_ldi_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_lhi)
TRANS_V058_CAPABILITY_FAULT(v_lhi_brg)
TRANS_V058_CAPABILITY_FAULT(v_lhi_u)
TRANS_V058_CAPABILITY_FAULT(v_lhi_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_lhui)
TRANS_V058_CAPABILITY_FAULT(v_lhui_brg)
TRANS_V058_CAPABILITY_FAULT(v_lhui_u)
TRANS_V058_CAPABILITY_FAULT(v_lhui_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_lw_add)
TRANS_V058_CAPABILITY_FAULT(v_lw_and)
TRANS_V058_CAPABILITY_FAULT(v_lw_max)
TRANS_V058_CAPABILITY_FAULT(v_lw_min)
TRANS_V058_CAPABILITY_FAULT(v_lw_or)
TRANS_V058_CAPABILITY_FAULT(v_lw_xor)
TRANS_V058_CAPABILITY_FAULT(v_lwi)
TRANS_V058_CAPABILITY_FAULT(v_lwi_brg)
TRANS_V058_CAPABILITY_FAULT(v_lwi_u)
TRANS_V058_CAPABILITY_FAULT(v_lwi_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_lwu)
TRANS_V058_CAPABILITY_FAULT(v_lwu_brg)
TRANS_V058_CAPABILITY_FAULT(v_lwui)
TRANS_V058_CAPABILITY_FAULT(v_lwui_brg)
TRANS_V058_CAPABILITY_FAULT(v_lwui_u)
TRANS_V058_CAPABILITY_FAULT(v_lwui_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_qpop)
TRANS_V058_CAPABILITY_FAULT(v_qpush)
TRANS_V058_CAPABILITY_FAULT(v_sbi)
TRANS_V058_CAPABILITY_FAULT(v_sbi_brg)
TRANS_V058_CAPABILITY_FAULT(v_sd)
TRANS_V058_CAPABILITY_FAULT(v_sd_add)
TRANS_V058_CAPABILITY_FAULT(v_sd_and)
TRANS_V058_CAPABILITY_FAULT(v_sd_brg)
TRANS_V058_CAPABILITY_FAULT(v_sd_max)
TRANS_V058_CAPABILITY_FAULT(v_sd_min)
TRANS_V058_CAPABILITY_FAULT(v_sd_or)
TRANS_V058_CAPABILITY_FAULT(v_sd_u)
TRANS_V058_CAPABILITY_FAULT(v_sd_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_sd_xor)
TRANS_V058_CAPABILITY_FAULT(v_sdi)
TRANS_V058_CAPABILITY_FAULT(v_sdi_brg)
TRANS_V058_CAPABILITY_FAULT(v_sdi_u)
TRANS_V058_CAPABILITY_FAULT(v_sdi_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_sh_u)
TRANS_V058_CAPABILITY_FAULT(v_sh_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_shfl_bfly)
TRANS_V058_CAPABILITY_FAULT(v_shfl_down)
TRANS_V058_CAPABILITY_FAULT(v_shfl_idx)
TRANS_V058_CAPABILITY_FAULT(v_shfl_up)
TRANS_V058_CAPABILITY_FAULT(v_shfli_bfly)
TRANS_V058_CAPABILITY_FAULT(v_shfli_down)
TRANS_V058_CAPABILITY_FAULT(v_shfli_idx)
TRANS_V058_CAPABILITY_FAULT(v_shfli_up)
TRANS_V058_CAPABILITY_FAULT(v_shi)
TRANS_V058_CAPABILITY_FAULT(v_shi_brg)
TRANS_V058_CAPABILITY_FAULT(v_shi_u)
TRANS_V058_CAPABILITY_FAULT(v_shi_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_sw_add)
TRANS_V058_CAPABILITY_FAULT(v_sw_and)
TRANS_V058_CAPABILITY_FAULT(v_sw_max)
TRANS_V058_CAPABILITY_FAULT(v_sw_min)
TRANS_V058_CAPABILITY_FAULT(v_sw_or)
TRANS_V058_CAPABILITY_FAULT(v_sw_u)
TRANS_V058_CAPABILITY_FAULT(v_sw_u_brg)
TRANS_V058_CAPABILITY_FAULT(v_sw_xor)
TRANS_V058_CAPABILITY_FAULT(v_swi)
TRANS_V058_CAPABILITY_FAULT(v_swi_brg)
TRANS_V058_CAPABILITY_FAULT(v_swi_u)
TRANS_V058_CAPABILITY_FAULT(v_swi_u_brg)

#undef TRANS_V058_CAPABILITY_FAULT

static bool trans_xb(DisasContext *ctx, arg_xb *a)
{
    (void)a;
    return linx_illegal(ctx);
}

/* ===================== PC-relative Instructions ===================== */

static bool trans_addtpc(DisasContext *ctx, arg_addtpc *a)
{
    /* ADDTPC: PC-relative page base
     * rd = (PC & ~0xFFF) + (sext(imm20) << 12) */
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr pc_page = current_pc & ~(vaddr)0xfff;
    int64_t imm = (int64_t)(int32_t)(a->imm20 << 12) >> 12; /* sign-extend 20-bit */
    imm <<= 12;
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, pc_page + imm);
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Function Entry/Exit Macro Instructions ===================== */

/*
 * LinxISA Function Prologue/Epilogue Instructions
 * 
 * These are hardware macro instructions that expand to register save/restore
 * sequences. The register range [Begin ~ End] specifies which registers to
 * save/restore (can wrap around from R23 to R2).
 * 
 * Stack size encoding:
 *   uimm[14:10] in instruction bits [11:7]
 *   uimm[9:3] in instruction bits [31:25]
 *   uimm[2:0] implicitly 0 (8-byte aligned)
 *   Actual stack size = (uimm_hi << 10) | (uimm_lo << 3)
 */

/* FENTRY: Function entry - save registers [Begin ~ End], adjust SP */
static bool trans_fentry(DisasContext *ctx, arg_fentry *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    if (linx_template_chain_enabled) {
        gen_helper_linx_template_fentry_chain(tcg_env,
                                              tcg_constant_i64(current_pc),
                                              tcg_constant_i64(ctx->base.pc_next),
                                              tcg_constant_i32(a->reg_begin),
                                              tcg_constant_i32(a->reg_end),
                                              tcg_constant_i64(stacksize));
        linx_gen_goto_tb_after_committed_helper(ctx, 0, ctx->base.pc_next);
    } else {
        gen_helper_linx_template_fentry(tcg_env,
                                        tcg_constant_i64(current_pc),
                                        tcg_constant_i64(ctx->base.pc_next),
                                        tcg_constant_i32(a->reg_begin),
                                        tcg_constant_i32(a->reg_end),
                                        tcg_constant_i64(stacksize));
    }
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* FEXIT: Function exit - restore registers, adjust SP (used with IND block for indirect return) */
static bool trans_fexit(DisasContext *ctx, arg_fexit *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    if (linx_template_chain_enabled) {
        gen_helper_linx_template_fexit_chain(tcg_env,
                                             tcg_constant_i64(current_pc),
                                             tcg_constant_i64(ctx->base.pc_next),
                                             tcg_constant_i32(a->reg_begin),
                                             tcg_constant_i32(a->reg_end),
                                             tcg_constant_i64(stacksize));
        linx_gen_goto_tb_after_committed_helper(ctx, 0, ctx->base.pc_next);
    } else {
        gen_helper_linx_template_fexit(tcg_env,
                                       tcg_constant_i64(current_pc),
                                       tcg_constant_i64(ctx->base.pc_next),
                                       tcg_constant_i32(a->reg_begin),
                                       tcg_constant_i32(a->reg_end),
                                       tcg_constant_i64(stacksize));
    }
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* FRET.RA: Function return via RA - restore registers, adjust SP, return to RA */
static bool trans_fret_ra(DisasContext *ctx, arg_fret_ra *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    if (linx_template_chain_enabled) {
        gen_helper_linx_template_fret_ra_chain(tcg_env,
                                               tcg_constant_i64(current_pc),
                                               tcg_constant_i64(ctx->base.pc_next),
                                               tcg_constant_i32(a->reg_begin),
                                               tcg_constant_i32(a->reg_end),
                                               tcg_constant_i64(stacksize));
        tcg_gen_lookup_and_goto_ptr();
    } else {
        gen_helper_linx_template_fret_ra(tcg_env,
                                         tcg_constant_i64(current_pc),
                                         tcg_constant_i64(ctx->base.pc_next),
                                         tcg_constant_i32(a->reg_begin),
                                         tcg_constant_i32(a->reg_end),
                                         tcg_constant_i64(stacksize));
    }
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* FRET.STK: Function return via stack - restore registers, adjust SP, return */
static bool trans_fret_stk(DisasContext *ctx, arg_fret_stk *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    const uint64_t stacksize = ((uint64_t)a->uimm_hi << 10) | ((uint64_t)a->uimm_lo << 3);
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    if (linx_template_chain_enabled) {
        gen_helper_linx_template_fret_stk_chain(tcg_env,
                                                tcg_constant_i64(current_pc),
                                                tcg_constant_i64(ctx->base.pc_next),
                                                tcg_constant_i32(a->reg_begin),
                                                tcg_constant_i32(a->reg_end),
                                                tcg_constant_i64(stacksize));
        tcg_gen_lookup_and_goto_ptr();
    } else {
        gen_helper_linx_template_fret_stk(tcg_env,
                                          tcg_constant_i64(current_pc),
                                          tcg_constant_i64(ctx->base.pc_next),
                                          tcg_constant_i32(a->reg_begin),
                                          tcg_constant_i32(a->reg_end),
                                          tcg_constant_i64(stacksize));
    }
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* MCOPY: restartable bulk memory copy template (standalone block). */
static bool trans_mcopy(DisasContext *ctx, arg_mcopy *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_MCOPY),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* MSET: restartable bulk memory set template (standalone block). */
static bool trans_mset(DisasContext *ctx, arg_mset *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_MSET),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* ESAVE: restartable extended-state save template (standalone block). */
static bool trans_esave(DisasContext *ctx, arg_esave *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_ESAVE),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* ERCOV: restartable extended-state restore template (standalone block). */
static bool trans_ercov(DisasContext *ctx, arg_ercov *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_FALL, 0);
    gen_helper_linx_template_step(tcg_env,
                                  tcg_constant_i32(LINX_TEMPLATE_ERCOV),
                                  tcg_constant_i64(current_pc),
                                  tcg_constant_i64(ctx->base.pc_next),
                                  tcg_constant_i32(a->SrcL),
                                  tcg_constant_i32(a->SrcR),
                                  tcg_constant_i64(a->SrcD));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/* ===================== Min/Max Instructions ===================== */

static bool trans_max(DisasContext *ctx, arg_max *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_GE, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_maxu(DisasContext *ctx, arg_maxu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_GEU, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_min(DisasContext *ctx, arg_min *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_LE, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_minu(DisasContext *ctx, arg_minu *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 r = linx_get_reg(a->SrcR);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movcond_i64(TCG_COND_LEU, out, l, r, l, r);
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Bit Manipulation Instructions ===================== */

static bool trans_clz(DisasContext *ctx, arg_clz *a)
{
    /* CLZ: Count leading zeros in the N-bit field starting at bit M in SrcL.
     * Encoding notes: M=imms, N=imml+1 (ISA manual).
     */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 field = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (width == 0 || lsb >= 64 || lsb + width > 64) {
        tcg_gen_movi_i64(out, 0);
        linx_set_dest(a->RegDst, out);
        return true;
    }

    if (lsb) {
        tcg_gen_shri_i64(field, src, lsb);
    } else {
        tcg_gen_mov_i64(field, src);
    }

    if (width != 64) {
        uint64_t mask = (1ULL << width) - 1ULL;
        tcg_gen_andi_i64(field, field, mask);
    }

    tcg_gen_clzi_i64(out, field, width);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_ctz(DisasContext *ctx, arg_ctz *a)
{
    /* CTZ: Count trailing zeros in the N-bit field starting at bit M in SrcL.
     * Encoding notes: M=imms, N=imml+1 (ISA manual).
     */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 field = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (width == 0 || lsb >= 64 || lsb + width > 64) {
        tcg_gen_movi_i64(out, 0);
        linx_set_dest(a->RegDst, out);
        return true;
    }

    if (lsb) {
        tcg_gen_shri_i64(field, src, lsb);
    } else {
        tcg_gen_mov_i64(field, src);
    }

    if (width != 64) {
        uint64_t mask = (1ULL << width) - 1ULL;
        tcg_gen_andi_i64(field, field, mask);
    }

    tcg_gen_ctzi_i64(out, field, width);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bcnt(DisasContext *ctx, arg_bcnt *a)
{
    /* BCNT: Count set bits in the N-bit field starting at bit M in SrcL.
     * Encoding notes: M=imms, N=imml+1 (ISA manual).
     */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 field = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (width == 0 || lsb >= 64 || lsb + width > 64) {
        tcg_gen_movi_i64(out, 0);
        linx_set_dest(a->RegDst, out);
        return true;
    }

    if (lsb) {
        tcg_gen_shri_i64(field, src, lsb);
    } else {
        tcg_gen_mov_i64(field, src);
    }

    if (width != 64) {
        uint64_t mask = (1ULL << width) - 1ULL;
        tcg_gen_andi_i64(field, field, mask);
    }

    tcg_gen_ctpop_i64(out, field);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_rev(DisasContext *ctx, arg_rev *a)
{
    /* REV: Bit reversal */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Byte swap first, then reverse bits within each byte */
    tcg_gen_bswap64_i64(out, src);
    /* For full bit reversal, we need additional operations - simplified for now */
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Bit Extract/Insert Instructions ===================== */

static bool trans_bxs(DisasContext *ctx, arg_bxs *a)
{
    /* BXS: Bit extract signed - extract bit field and sign-extend */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_mov_i64(out, src);
        } else {
        tcg_gen_sextract_i64(out, src, lsb, width);
        }
    } else {
        tcg_gen_movi_i64(out, 0);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bxu(DisasContext *ctx, arg_bxu *a)
{
    /* BXU: Bit extract unsigned - extract bit field, zero-extend */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_mov_i64(out, src);
        } else {
            tcg_gen_extract_i64(out, src, lsb, width);
        }
    } else {
        tcg_gen_movi_i64(out, 0);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bic(DisasContext *ctx, arg_bic *a)
{
    /* BIC: Bit clear - clear bits in range [lsb, lsb+width) */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_movi_i64(out, 0);
        } else {
            uint64_t mask = ~(((1ULL << width) - 1) << lsb);
            tcg_gen_andi_i64(out, src, mask);
        }
    } else {
        tcg_gen_mov_i64(out, src);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_bis(DisasContext *ctx, arg_bis *a)
{
    /* BIS: Bit set - set bits in range [lsb, lsb+width) */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    /* Encoding notes: M=imms, N=imml+1 (ISA manual). */
    unsigned lsb = a->imms;
    unsigned width = a->imml + 1;
    if (lsb < 64 && lsb + width <= 64) {
        if (lsb == 0 && width == 64) {
            tcg_gen_movi_i64(out, -1);
        } else {
            uint64_t mask = ((1ULL << width) - 1) << lsb;
            tcg_gen_ori_i64(out, src, mask);
        }
    } else {
        tcg_gen_mov_i64(out, src);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Compare Immediate Instructions ===================== */

static bool trans_cmp_eqi(DisasContext *ctx, arg_cmp_eqi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_EQ, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_nei(DisasContext *ctx, arg_cmp_nei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_NE, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_andi(DisasContext *ctx, arg_cmp_andi *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(tmp, l, (int64_t)a->simm12);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_ori(DisasContext *ctx, arg_cmp_ori *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(tmp, l, (int64_t)a->simm12);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_lti(DisasContext *ctx, arg_cmp_lti *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LT, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_gei(DisasContext *ctx, arg_cmp_gei *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GE, out, l, (int64_t)a->simm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_ltui(DisasContext *ctx, arg_cmp_ltui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LTU, out, l, (uint64_t)a->uimm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_cmp_geui(DisasContext *ctx, arg_cmp_geui *a)
{
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GEU, out, l, (uint64_t)a->uimm12);
    linx_set_dest(a->RegDst, out);
    return true;
}

/* ===================== Branch on Zero/Non-Zero Instructions ===================== */

static bool linx_trans_body_branch_target(DisasContext *ctx, vaddr current_pc,
                                          vaddr target);

static bool linx_trans_body_pred_branch(DisasContext *ctx, TCGCond cond,
                                        int64_t simm_hw)
{
    TCGLabel *taken = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr fallthrough = ctx->base.pc_next;
    vaddr target = current_pc + (simm_hw << 1);
    const int32_t take_on_zero = (cond == TCG_COND_EQ) ? 1 : 0;

    gen_helper_linx_debug_body_pred_branch(tcg_env,
                                           tcg_constant_i64(current_pc),
                                           tcg_constant_i64(target),
                                           tcg_constant_i64(fallthrough),
                                           tcg_constant_i32(take_on_zero));
    tcg_gen_brcondi_i64(cond, cpu_vec_p, 0, taken);
    tcg_gen_movi_i64(cpu_pc, fallthrough);
    tcg_gen_exit_tb(NULL, 0);

    gen_set_label(taken);
    return linx_trans_body_branch_target(ctx, current_pc, target);
}

static bool linx_trans_body_branch_target(DisasContext *ctx, vaddr current_pc,
                                          vaddr target)
{
    TCGLabel *have_range = gen_new_label();
    TCGLabel *range_lo_ok = gen_new_label();
    TCGLabel *in_range = gen_new_label();
    TCGLabel *equal_end = gen_new_label();
    TCGLabel *fault = gen_new_label();
    const uint32_t bfetch_cause = linx_eblock_bfetch_cause();

    tcg_gen_brcondi_i64(TCG_COND_NE, cpu_body_end, 0, have_range);
    tcg_gen_br(in_range);

    gen_set_label(have_range);
    tcg_gen_brcond_i64(TCG_COND_GEU, tcg_constant_i64(target), cpu_body_tpc,
                       range_lo_ok);
    tcg_gen_br(fault);

    gen_set_label(range_lo_ok);
    tcg_gen_brcond_i64(TCG_COND_LTU, tcg_constant_i64(target), cpu_body_end,
                       in_range);
    tcg_gen_brcond_i64(TCG_COND_EQ, tcg_constant_i64(target), cpu_body_end,
                       equal_end);
    tcg_gen_br(fault);

    gen_set_label(equal_end);
    if (ctx->env &&
        (linx_is_c_bstop_at_pc(ctx, target) ||
         linx_is_bstop32_at_pc(ctx, target))) {
        tcg_gen_br(in_range);
    }
    tcg_gen_br(fault);

    gen_set_label(in_range);
    if (linx_commit_trace_enabled) {
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(target));
    }
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);

    gen_set_label(fault);
    tcg_gen_movi_i64(cpu_pending_trap_arg0, target);
    tcg_gen_movi_i32(cpu_pending_trap_cause, bfetch_cause);
    if (linx_commit_trace_enabled) {
        tcg_gen_movi_i32(cpu_trace_trap_valid, 1);
        tcg_gen_movi_i32(cpu_trace_trap_cause,
                         (int32_t)(((bfetch_cause & 0xffffu) << 8) | 5));
        tcg_gen_movi_i64(cpu_trace_traparg0, target);
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(current_pc));
    }
    tcg_gen_movi_i64(cpu_pc, current_pc);
    gen_helper_raise_exception(tcg_env,
                               tcg_constant_i32(LINX_EXCP_BLOCK_FAULT));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool linx_trans_body_cmp_branch(DisasContext *ctx, TCGCond cond,
                                       uint32_t src_l, uint32_t src_r,
                                       int64_t simm_hw)
{
    TCGLabel *taken = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr fallthrough = ctx->base.pc_next;
    vaddr target = linx_pcrel_target(current_pc, simm_hw);
    TCGv_i64 lhs = linx_get_reg(src_l);
    TCGv_i64 rhs = linx_get_reg(src_r);

    tcg_gen_brcond_i64(cond, lhs, rhs, taken);
    tcg_gen_movi_i64(cpu_pc, fallthrough);
    tcg_gen_lookup_and_goto_ptr();

    gen_set_label(taken);
    return linx_trans_body_branch_target(ctx, current_pc, target);
}

static bool trans_b_z(DisasContext *ctx, arg_b_z *a)
{
    if (ctx->in_body) {
        return linx_trans_body_pred_branch(ctx, TCG_COND_EQ, (int64_t)a->simm22);
    }
    /* B.Z: Branch if condition flag is zero */
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm22 << 1);
    
    tcg_gen_brcondi_i32(TCG_COND_EQ, cpu_cond, 0, taken);
    tcg_gen_br(done);
    
    gen_set_label(taken);
    linx_gen_check_bstart_target(ctx, tcg_constant_i64(target));
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    
    gen_set_label(done);
    return true;
}

static bool trans_b_nz(DisasContext *ctx, arg_b_nz *a)
{
    if (ctx->in_body) {
        return linx_trans_body_pred_branch(ctx, TCG_COND_NE, (int64_t)a->simm22);
    }
    /* B.NZ: Branch if condition flag is non-zero */
    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm22 << 1);
    
    tcg_gen_brcondi_i32(TCG_COND_NE, cpu_cond, 0, taken);
    tcg_gen_br(done);
    
    gen_set_label(taken);
    linx_gen_check_bstart_target(ctx, tcg_constant_i64(target));
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    
    gen_set_label(done);
    return true;
}

static bool linx_trans_branch_cmp(DisasContext *ctx, TCGCond cond,
                                  uint32_t src_l, uint32_t src_r, int64_t simm_hw)
{
    if (ctx->in_body) {
        return linx_trans_body_cmp_branch(ctx, cond, src_l, src_r, simm_hw);
    }

    TCGLabel *taken = gen_new_label();
    TCGLabel *done = gen_new_label();
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = linx_pcrel_target(current_pc, simm_hw);
    TCGv_i64 lhs = linx_get_reg(src_l);
    TCGv_i64 rhs = linx_get_reg(src_r);

    tcg_gen_brcond_i64(cond, lhs, rhs, taken);
    tcg_gen_br(done);

    gen_set_label(taken);
    linx_gen_check_bstart_target(ctx, tcg_constant_i64(target));
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);

    gen_set_label(done);
    return true;
}

static bool trans_b_eq(DisasContext *ctx, arg_b_eq *a)
{
    return linx_trans_branch_cmp(ctx, TCG_COND_EQ, a->SrcL, a->SrcR, a->simm12);
}

static bool trans_b_ne(DisasContext *ctx, arg_b_ne *a)
{
    return linx_trans_branch_cmp(ctx, TCG_COND_NE, a->SrcL, a->SrcR, a->simm12);
}

static bool trans_b_lt(DisasContext *ctx, arg_b_lt *a)
{
    return linx_trans_branch_cmp(ctx, TCG_COND_LT, a->SrcL, a->SrcR, a->simm12);
}

static bool trans_b_ge(DisasContext *ctx, arg_b_ge *a)
{
    return linx_trans_branch_cmp(ctx, TCG_COND_GE, a->SrcL, a->SrcR, a->simm12);
}

static bool trans_b_ltu(DisasContext *ctx, arg_b_ltu *a)
{
    return linx_trans_branch_cmp(ctx, TCG_COND_LTU, a->SrcL, a->SrcR, a->simm12);
}

static bool trans_b_geu(DisasContext *ctx, arg_b_geu *a)
{
    return linx_trans_branch_cmp(ctx, TCG_COND_GEU, a->SrcL, a->SrcR, a->simm12);
}

/* ===================== 16-bit Sign/Zero Extension ===================== */

static bool trans_c_sext_b(DisasContext *ctx, arg_c_sext_b *a)
{
    /* C.SEXT.B: Sign-extend byte to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext8s_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_sext_h(DisasContext *ctx, arg_c_sext_h *a)
{
    /* C.SEXT.H: Sign-extend halfword to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext16s_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_sext_w(DisasContext *ctx, arg_c_sext_w *a)
{
    /* C.SEXT.W: Sign-extend word to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32s_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_zext_b(DisasContext *ctx, arg_c_zext_b *a)
{
    /* C.ZEXT.B: Zero-extend byte to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext8u_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_zext_h(DisasContext *ctx, arg_c_zext_h *a)
{
    /* C.ZEXT.H: Zero-extend halfword to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext16u_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_zext_w(DisasContext *ctx, arg_c_zext_w *a)
{
    /* C.ZEXT.W: Zero-extend word to T-hand */
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ext32u_i64(out, src);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

/* ===================== 16-bit Shift Instructions ===================== */

static bool trans_c_cmp_eqi(DisasContext *ctx, arg_c_cmp_eqi *a)
{
    /* C.CMP.EQI: (T-hand == imm) -> T-hand */
    TCGv_i64 src = cpu_tq[0];
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_EQ, out, src, (int64_t)a->simm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_cmp_nei(DisasContext *ctx, arg_c_cmp_nei *a)
{
    /* C.CMP.NEI: (T-hand != imm) -> T-hand */
    TCGv_i64 src = cpu_tq[0];
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_NE, out, src, (int64_t)a->simm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_slli(DisasContext *ctx, arg_c_slli *a)
{
    /* C.SLLI: Shift T-hand left by immediate, result to T-hand */
    TCGv_i64 src = cpu_tq[0];  /* T-hand as source */
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shli_i64(out, src, a->uimm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

static bool trans_c_srli(DisasContext *ctx, arg_c_srli *a)
{
    /* C.SRLI: Shift T-hand right logical by immediate, result to T-hand */
    TCGv_i64 src = cpu_tq[0];  /* T-hand as source */
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_shri_i64(out, src, a->uimm5);
    linx_set_dest(31, out);  /* Output to T-hand */
    return true;
}

/* ===================== 48-bit Instructions ===================== */

static bool trans_hl_addtpc(DisasContext *ctx, arg_hl_addtpc *a)
{
    /* HL.SETRET is encoded as an HL.ADDTPC alias targeting RA. */
    if (a->RegDst == LINX_REG_RA) {
        return linx_setret_common(ctx, (int64_t)(int32_t)a->imm);
    }

    /* HL.ADDTPC: PC-relative with 32-bit offset */
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr pc_page = current_pc & ~(vaddr)0xfff;
    int64_t offset = (int64_t)(int32_t)a->imm;
    offset <<= 12;
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_movi_i64(out, pc_page + offset);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_addi(DisasContext *ctx, arg_hl_addi *a)
{
    /* HL.ADDI: Add with 24-bit unsigned immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_addiw(DisasContext *ctx, arg_hl_addiw *a)
{
    /* HL.ADDIW: Add with 24-bit unsigned immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_addi_i64(out, l, (uint64_t)a->uimm24);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_subi(DisasContext *ctx, arg_hl_subi *a)
{
    /* HL.SUBI: Subtract with 24-bit unsigned immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_subiw(DisasContext *ctx, arg_hl_subiw *a)
{
    /* HL.SUBIW: Subtract with 24-bit unsigned immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_subi_i64(out, l, (uint64_t)a->uimm24);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_andi(DisasContext *ctx, arg_hl_andi *a)
{
    /* HL.ANDI: AND with 24-bit signed immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_andiw(DisasContext *ctx, arg_hl_andiw *a)
{
    /* HL.ANDIW: AND with 24-bit signed immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(out, l, (int64_t)a->simm);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_ori(DisasContext *ctx, arg_hl_ori *a)
{
    /* HL.ORI: OR with 24-bit signed immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_oriw(DisasContext *ctx, arg_hl_oriw *a)
{
    /* HL.ORIW: OR with 24-bit signed immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(out, l, (int64_t)a->simm);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_xori(DisasContext *ctx, arg_hl_xori *a)
{
    /* HL.XORI: XOR with 24-bit signed immediate */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_xoriw(DisasContext *ctx, arg_hl_xoriw *a)
{
    /* HL.XORIW: XOR with 24-bit signed immediate (word) */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_xori_i64(out, l, (int64_t)a->simm);
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_hl_cmp_eqi(DisasContext *ctx, arg_hl_cmp_eqi *a)
{
    /* HL.CMP.EQI: (SrcL == simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_EQ, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_nei(DisasContext *ctx, arg_hl_cmp_nei *a)
{
    /* HL.CMP.NEI: (SrcL != simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_NE, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_andi(DisasContext *ctx, arg_hl_cmp_andi *a)
{
    /* HL.CMP.ANDI: ((SrcL & simm24) != 0) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_andi_i64(tmp, l, (int64_t)a->simm);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_ori(DisasContext *ctx, arg_hl_cmp_ori *a)
{
    /* HL.CMP.ORI: ((SrcL | simm24) != 0) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_ori_i64(tmp, l, (int64_t)a->simm);
    tcg_gen_setcondi_i64(TCG_COND_NE, out, tmp, 0);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_lti(DisasContext *ctx, arg_hl_cmp_lti *a)
{
    /* HL.CMP.LTI: (SrcL < simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LT, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_gei(DisasContext *ctx, arg_hl_cmp_gei *a)
{
    /* HL.CMP.GEI: (SrcL >= simm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GE, out, l, (int64_t)a->simm);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_ltui(DisasContext *ctx, arg_hl_cmp_ltui *a)
{
    /* HL.CMP.LTUI: (SrcL < uimm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_LTU, out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_cmp_geui(DisasContext *ctx, arg_hl_cmp_geui *a)
{
    /* HL.CMP.GEUI: (SrcL >= uimm24) -> {t,u,Rd} */
    TCGv_i64 l = linx_get_reg(a->SrcL);
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_setcondi_i64(TCG_COND_GEU, out, l, (uint64_t)a->uimm24);
    linx_set_dest(a->RegDst, out);
    return true;
}

static void linx_set_dest_pair(uint32_t dst0, uint32_t dst1,
                               TCGv_i64 lo, TCGv_i64 hi)
{
    linx_set_dest(dst0, lo);
    linx_set_dest(dst1, hi);
}

static bool linx_hl_mul_pair_common(DisasContext *ctx, uint32_t dst0,
                                    uint32_t dst1, TCGv_i64 lhs,
                                    TCGv_i64 rhs, bool is_unsigned)
{
    TCGv_i64 lo = tcg_temp_new_i64();
    TCGv_i64 hi = tcg_temp_new_i64();

    (void)ctx;
    if (is_unsigned) {
        tcg_gen_mulu2_i64(lo, hi, lhs, rhs);
    } else {
        tcg_gen_muls2_i64(lo, hi, lhs, rhs);
    }
    linx_set_dest_pair(dst0, dst1, lo, hi);
    return true;
}

static bool linx_hl_madd_pair_common(DisasContext *ctx, uint32_t dst0,
                                     uint32_t dst1, TCGv_i64 addend,
                                     TCGv_i64 lhs, TCGv_i64 rhs)
{
    TCGv_i64 prod_lo = tcg_temp_new_i64();
    TCGv_i64 prod_hi = tcg_temp_new_i64();
    TCGv_i64 add_hi = tcg_temp_new_i64();
    TCGv_i64 acc_lo = tcg_temp_new_i64();
    TCGv_i64 acc_hi = tcg_temp_new_i64();

    (void)ctx;
    tcg_gen_muls2_i64(prod_lo, prod_hi, lhs, rhs);
    tcg_gen_sari_i64(add_hi, addend, 63);
    tcg_gen_add2_i64(acc_lo, acc_hi, prod_lo, prod_hi, addend, add_hi);
    linx_set_dest_pair(dst0, dst1, acc_lo, acc_hi);
    return true;
}

static bool linx_hl_divrem_pair_common(DisasContext *ctx, uint32_t dst0,
                                       uint32_t dst1, TCGv_i64 dividend,
                                       TCGv_i64 divisor, bool is_unsigned,
                                       bool word)
{
    TCGv_i64 lhs = tcg_temp_new_i64();
    TCGv_i64 rhs = tcg_temp_new_i64();
    TCGv_i64 quot = tcg_temp_new_i64();
    TCGv_i64 rem = tcg_temp_new_i64();
    TCGLabel *divzero = gen_new_label();
    TCGLabel *overflow = gen_new_label();
    TCGLabel *done = gen_new_label();

    (void)ctx;
    if (word) {
        if (is_unsigned) {
            tcg_gen_ext32u_i64(lhs, dividend);
            tcg_gen_ext32u_i64(rhs, divisor);
        } else {
            tcg_gen_ext32s_i64(lhs, dividend);
            tcg_gen_ext32s_i64(rhs, divisor);
        }
    } else {
        tcg_gen_mov_i64(lhs, dividend);
        tcg_gen_mov_i64(rhs, divisor);
    }

    tcg_gen_brcondi_i64(TCG_COND_EQ, rhs, 0, divzero);
    if (!is_unsigned) {
        TCGv_i64 minval = tcg_constant_i64(
            word ? INT64_C(0xffffffff80000000) : INT64_MIN);
        TCGLabel *not_overflow = gen_new_label();

        tcg_gen_brcond_i64(TCG_COND_NE, lhs, minval, not_overflow);
        tcg_gen_brcondi_i64(TCG_COND_EQ, rhs, -1, overflow);
        gen_set_label(not_overflow);
    }

    if (is_unsigned) {
        tcg_gen_divu_i64(quot, lhs, rhs);
        tcg_gen_remu_i64(rem, lhs, rhs);
    } else {
        tcg_gen_div_i64(quot, lhs, rhs);
        tcg_gen_rem_i64(rem, lhs, rhs);
    }
    tcg_gen_br(done);

    gen_set_label(divzero);
    tcg_gen_movi_i64(quot, 0);
    tcg_gen_mov_i64(rem, lhs);
    tcg_gen_br(done);

    gen_set_label(overflow);
    tcg_gen_mov_i64(quot, lhs);
    tcg_gen_movi_i64(rem, 0);

    gen_set_label(done);
    if (word) {
        TCGv_i64 quot_w = tcg_temp_new_i64();
        TCGv_i64 rem_w = tcg_temp_new_i64();

        tcg_gen_ext32s_i64(quot_w, quot);
        tcg_gen_ext32s_i64(rem_w, rem);
        linx_set_dest_pair(dst0, dst1, quot_w, rem_w);
    } else {
        linx_set_dest_pair(dst0, dst1, quot, rem);
    }
    return true;
}

static bool trans_hl_mul(DisasContext *ctx, arg_hl_mul *a)
{
    return linx_hl_mul_pair_common(ctx, a->RegDst0, a->RegDst1,
                                   linx_get_reg(a->SrcL),
                                   linx_get_reg(a->SrcR), false);
}

static bool trans_hl_mulu(DisasContext *ctx, arg_hl_mulu *a)
{
    return linx_hl_mul_pair_common(ctx, a->RegDst0, a->RegDst1,
                                   linx_get_reg(a->SrcL),
                                   linx_get_reg(a->SrcR), true);
}

static bool trans_hl_madd(DisasContext *ctx, arg_hl_madd *a)
{
    return linx_hl_madd_pair_common(ctx, a->RegDst0, a->RegDst1,
                                    linx_get_reg(a->SrcD),
                                    linx_get_reg(a->SrcL),
                                    linx_get_reg(a->SrcR));
}

static bool trans_hl_maddw(DisasContext *ctx, arg_hl_maddw *a)
{
    TCGv_i64 d = tcg_temp_new_i64();
    TCGv_i64 l = tcg_temp_new_i64();
    TCGv_i64 r = tcg_temp_new_i64();

    tcg_gen_ext32s_i64(d, linx_get_reg(a->SrcD));
    tcg_gen_ext32s_i64(l, linx_get_reg(a->SrcL));
    tcg_gen_ext32s_i64(r, linx_get_reg(a->SrcR));
    return linx_hl_madd_pair_common(ctx, a->RegDst0, a->RegDst1, d, l, r);
}

static bool trans_hl_div(DisasContext *ctx, arg_hl_div *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), false, false);
}

static bool trans_hl_divu(DisasContext *ctx, arg_hl_divu *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), true, false);
}

static bool trans_hl_divw(DisasContext *ctx, arg_hl_divw *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), false, true);
}

static bool trans_hl_divuw(DisasContext *ctx, arg_hl_divuw *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), true, true);
}

static bool trans_hl_rem(DisasContext *ctx, arg_hl_rem *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), false, false);
}

static bool trans_hl_remu(DisasContext *ctx, arg_hl_remu *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), true, false);
}

static bool trans_hl_remw(DisasContext *ctx, arg_hl_remw *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), false, true);
}

static bool trans_hl_remuw(DisasContext *ctx, arg_hl_remuw *a)
{
    return linx_hl_divrem_pair_common(ctx, a->RegDst0, a->RegDst1,
                                      linx_get_reg(a->SrcL),
                                      linx_get_reg(a->SrcR), true, true);
}

/* 48-bit BSTART instructions with extended range */
static bool trans_hl_bstart_std_fall(DisasContext *ctx, arg_hl_bstart_std_fall *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_FALL,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_std_direct(DisasContext *ctx, arg_hl_bstart_std_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_DIRECT,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_std_cond(DisasContext *ctx, arg_hl_bstart_std_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_COND,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_std_call(DisasContext *ctx, arg_hl_bstart_std_call *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_CALL,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_fp_fall(DisasContext *ctx, arg_hl_bstart_fp_fall *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_FALL,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_fp_direct(DisasContext *ctx, arg_hl_bstart_fp_direct *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_DIRECT,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_fp_cond(DisasContext *ctx, arg_hl_bstart_fp_cond *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_COND,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_fp_call(DisasContext *ctx, arg_hl_bstart_fp_call *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    return linx_begin_header_target(ctx, LINX_BR_CALL,
                                    linx_pcrel_target(current_pc, a->simm));
}

static bool trans_hl_bstart_sys(DisasContext *ctx, arg_hl_bstart_sys *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    if (current_pc != ctx->base.pc_first && ctx->brtype != 0) {
        return true;
    }
    return linx_begin_sys_header_target(ctx,
                                        linx_pcrel_target(current_pc, a->simm));
}

static bool linx_trans_l_bstart(DisasContext *ctx)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    int64_t simm = linx_decode_l_bstart_simm42(ctx->cur_insn_raw);
    const uint8_t brtype = (ctx->cur_insn_raw >> 44) & 0x7u;
    return linx_begin_header_target(ctx, brtype,
                                    linx_pcrel_target(current_pc, simm));
}

static bool trans_l_bstart_std(DisasContext *ctx, arg_l_bstart_std *a)
{
    return linx_trans_l_bstart(ctx);
}

static bool trans_l_bstart_fp(DisasContext *ctx, arg_l_bstart_fp *a)
{
    return linx_trans_l_bstart(ctx);
}

static bool trans_l_bstart_sys(DisasContext *ctx, arg_l_bstart_sys *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    int64_t simm = linx_decode_l_bstart_simm42(ctx->cur_insn_raw);
    if (current_pc != ctx->base.pc_first && ctx->brtype != 0) {
        return true;
    }
    return linx_begin_sys_header_target(ctx,
                                        linx_pcrel_target(current_pc, simm));
}

/* ===================== System Instructions ===================== */

static bool trans_ssrget(DisasContext *ctx, arg_ssrget *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);

    gen_helper_linx_ssr_read(val, tcg_env, ssrid);
    linx_set_dest(a->RegDst, val);

    return true;
}

static bool trans_hl_ssrget(DisasContext *ctx, arg_hl_ssrget *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);

    gen_helper_linx_ssr_read(val, tcg_env, ssrid);
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_acrc(DisasContext *ctx, arg_acrc *a)
{
    /*
     * ACRC: request a synchronous system-call trap.
     *
     * v0.2 bring-up rule: ACRC MUST be followed by an explicit BSTOP/C.BSTOP in
     * the same block. The trap resume PC is the instruction after ACRC (bring-up:
     * the explicit terminator).
     */
    vaddr tpc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr bpc = ctx->base.pc_first;

    /* Enforce "ACRC followed by (C.)BSTOP" (bring-up). */
    {
        uint16_t hw_next = translator_lduw_end(ctx->env, &ctx->base, ctx->base.pc_next, MO_LE);
        if (hw_next != 0x0000) {
            return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ACRC_MISSING_BSTOP, ctx->base.pc_next);
        }
    }

    gen_helper_linx_service_request(
        tcg_env,
        tcg_constant_i32(a->rst),
        tcg_constant_i64(bpc),
        tcg_constant_i64(tpc),
        tcg_constant_i64(ctx->base.pc_next));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_acre(DisasContext *ctx, arg_acre *a)
{
    /* ACRE: request an ACR_ENTER (trap return / handoff) at block commit. */
    gen_helper_linx_acr_enter(tcg_env, tcg_constant_i32(a->rra));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_lr_w(DisasContext *ctx, arg_lr_w *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_lr_w(val, tcg_env, addr);
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_lr_d(DisasContext *ctx, arg_lr_d *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_lr_d(val, tcg_env, addr);
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_sc_w(DisasContext *ctx, arg_sc_w *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcR);
    TCGv_i64 val64 = linx_get_reg(a->SrcL);
    TCGv_i64 res = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, val64);
    gen_helper_linx_sc_w(res, tcg_env, addr, val32);
    linx_set_dest(a->RegDst, res);
    return true;
}

static bool trans_sc_d(DisasContext *ctx, arg_sc_d *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcR);
    TCGv_i64 val = linx_get_reg(a->SrcL);
    TCGv_i64 res = tcg_temp_new_i64();
    gen_helper_linx_sc_d(res, tcg_env, addr, val);
    linx_set_dest(a->RegDst, res);
    return true;
}

static bool trans_swapw(DisasContext *ctx, arg_swapw *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val64 = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, val64);
    gen_helper_linx_swapw(old, tcg_env, addr, val32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_swapd(DisasContext *ctx, arg_swapd *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    gen_helper_linx_swapd(old, tcg_env, addr, val);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_lw_add(DisasContext *ctx, arg_lw_add *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val64 = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, val64);
    gen_helper_linx_lw_add(old, tcg_env, addr, val32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_ld_add(DisasContext *ctx, arg_ld_add *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 val = linx_get_reg(a->SrcR);
    TCGv_i64 old = tcg_temp_new_i64();
    gen_helper_linx_ld_add(old, tcg_env, addr, val);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_casb(DisasContext *ctx, arg_casb *a)
{
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 cmp32 = tcg_temp_new_i32();
    TCGv_i32 new32 = tcg_temp_new_i32();

    tcg_gen_extrl_i64_i32(cmp32, linx_get_reg(a->SrcR));
    tcg_gen_extrl_i64_i32(new32, linx_get_reg(a->SrcD));
    gen_helper_linx_casb(old, tcg_env, linx_get_reg(a->SrcL), cmp32, new32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_cash(DisasContext *ctx, arg_cash *a)
{
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 cmp32 = tcg_temp_new_i32();
    TCGv_i32 new32 = tcg_temp_new_i32();

    tcg_gen_extrl_i64_i32(cmp32, linx_get_reg(a->SrcR));
    tcg_gen_extrl_i64_i32(new32, linx_get_reg(a->SrcD));
    gen_helper_linx_cash(old, tcg_env, linx_get_reg(a->SrcL), cmp32, new32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_casw(DisasContext *ctx, arg_casw *a)
{
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 cmp32 = tcg_temp_new_i32();
    TCGv_i32 new32 = tcg_temp_new_i32();

    tcg_gen_extrl_i64_i32(cmp32, linx_get_reg(a->SrcR));
    tcg_gen_extrl_i64_i32(new32, linx_get_reg(a->SrcD));
    gen_helper_linx_casw(old, tcg_env, linx_get_reg(a->SrcL), cmp32, new32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_casd(DisasContext *ctx, arg_casd *a)
{
    TCGv_i64 old = tcg_temp_new_i64();

    gen_helper_linx_casd(old, tcg_env, linx_get_reg(a->SrcL),
                         linx_get_reg(a->SrcR), linx_get_reg(a->SrcD));
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_dma(DisasContext *ctx, arg_dma *a)
{
    gen_helper_linx_dma(tcg_env, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR));
    return true;
}

static bool trans_fence_d(DisasContext *ctx, arg_fence_d *a)
{
    (void)a;
    tcg_gen_mb(TCG_MO_ALL);
    return true;
}

static bool trans_fence_i(DisasContext *ctx, arg_fence_i *a)
{
    (void)a;
    tcg_gen_mb(TCG_MO_ALL);
    return true;
}

static bool trans_tlb_iall(DisasContext *ctx, arg_tlb_iall *a)
{
    (void)a;
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    gen_helper_linx_tlb_iall(tcg_env, tcg_constant_i64(pc));
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_tlb_ia(DisasContext *ctx, arg_tlb_ia *a)
{
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    TCGv_i64 asid = linx_get_reg(a->SrcL);

    gen_helper_linx_tlb_ia(tcg_env, asid, tcg_constant_i64(pc));
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_tlb_iv(DisasContext *ctx, arg_tlb_iv *a)
{
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    TCGv_i64 addr = linx_get_reg(a->SrcL);

    gen_helper_linx_tlb_iv(tcg_env, addr, tcg_constant_i64(pc));
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_tlb_iav(DisasContext *ctx, arg_tlb_iav *a)
{
    const vaddr pc = ctx->base.pc_next - ctx->cur_insn_len;
    TCGv_i64 packed = linx_get_reg(a->SrcL);

    gen_helper_linx_tlb_iav(tcg_env, packed, tcg_constant_i64(pc));
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_assert(DisasContext *ctx, arg_assert *a)
{
    (void)ctx;
    (void)a;
    /* TODO(v0.4): wire ASSERT_FAIL trapnum 52 through QEMU's exception path. */
    return true;
}

static bool trans_bse(DisasContext *ctx, arg_bse *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_bwe(DisasContext *ctx, arg_bwe *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_bwi(DisasContext *ctx, arg_bwi *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_bwt(DisasContext *ctx, arg_bwt *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_bc_iall(DisasContext *ctx, arg_bc_iall *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_bc_iva(DisasContext *ctx, arg_bc_iva *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_ic_iall(DisasContext *ctx, arg_ic_iall *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_ic_iva(DisasContext *ctx, arg_ic_iva *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_iall(DisasContext *ctx, arg_dc_iall *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_iva(DisasContext *ctx, arg_dc_iva *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_civa(DisasContext *ctx, arg_dc_civa *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_cva(DisasContext *ctx, arg_dc_cva *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_csw(DisasContext *ctx, arg_dc_csw *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_cisw(DisasContext *ctx, arg_dc_cisw *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_isw(DisasContext *ctx, arg_dc_isw *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_dc_zva(DisasContext *ctx, arg_dc_zva *a)
{
    (void)ctx;
    (void)a;
    return true;
}

static bool trans_ssrset(DisasContext *ctx, arg_ssrset *a)
{
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);

    gen_helper_linx_ssr_write(tcg_env, ssrid, src);
    return true;
}

static bool trans_hl_ssrset(DisasContext *ctx, arg_hl_ssrset *a)
{
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);
    gen_helper_linx_ssr_write(tcg_env, ssrid, src);
    return true;
}

static bool trans_ssrswap(DisasContext *ctx, arg_ssrswap *a)
{
    TCGv_i64 src = linx_get_reg(a->SrcL);
    TCGv_i32 ssrid = tcg_constant_i32(a->ssrid);
    TCGv_i64 old = tcg_temp_new_i64();

    gen_helper_linx_ssr_swap(old, tcg_env, ssrid, src);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_ebreak(DisasContext *ctx, arg_ebreak *a)
{
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);

    /* EBREAK defaults to architecture trap semantics; semihost behavior is
     * opt-in and decided by helper-side runtime policy. */
    gen_helper_linx_ebreak(tcg_env, tcg_constant_i32(a->imm4));
    /*
     * Most semihosting operations return to the guest. Only exit/breakpoint
     * paths should terminate the TB.
     */
    if (a->imm4 != 1 && /* PUTCHAR */
        a->imm4 != 2 && /* WRITE */
        a->imm4 != 3 /* READ */) {
        ctx->base.is_jmp = DISAS_NORETURN;
    }
    return true;
}

static bool trans_c_ebreak(DisasContext *ctx, arg_c_ebreak *a)
{
    tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
    gen_helper_linx_ebreak(tcg_env, tcg_constant_i32(a->imm5));
    if (a->imm5 != 1 && a->imm5 != 2 && a->imm5 != 3) {
        ctx->base.is_jmp = DISAS_NORETURN;
    }
    return true;
}

static bool trans_c_ssrget(DisasContext *ctx, arg_c_ssrget *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_ssr_read(val, tcg_env, tcg_constant_i32(a->ssrid & 0x1f));
    linx_push_t(val);
    return true;
}

static bool trans_lsrget(DisasContext *ctx, arg_lsrget *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_ssr_read(val, tcg_env, tcg_constant_i32(a->lsrid));
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_j(DisasContext *ctx, arg_j *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr target = current_pc + ((int64_t)a->simm22 << 1);

    if (ctx->in_body) {
        bool ok = linx_trans_body_branch_target(ctx, current_pc, target);
        if (ok && ctx->base.is_jmp == DISAS_NEXT) {
            ctx->base.is_jmp = DISAS_NORETURN;
        }
        return ok;
    }

    /*
     * glibc's Linx syscall wrappers materialize the success path as an empty
     * RET block followed by `j __syscall_error` and a trailing BSTOP. When the
     * RET block is entered directly, that jump is not executable body code; it
     * is only the error landing pad for the previous COND block.
     */
    if ((ctx->brtype & 0x7) == LINX_BR_RET &&
        !ctx->tgt_modified && ctx->brtarget == 0 &&
        current_pc == ctx->base.pc_first + 2 &&
        linx_is_ret_wrapper_j_trailer_at_pc(ctx, ctx->base.pc_first)) {
        linx_gen_ret_to_ra(ctx);
        return true;
    }

    linx_gen_check_bstart_target(ctx, tcg_constant_i64(target));
    tcg_gen_movi_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_jr(DisasContext *ctx, arg_jr *a)
{
    TCGv_i64 base = linx_get_reg(a->SrcL);
    TCGv_i64 target = tcg_temp_new_i64();

    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    tcg_gen_addi_i64(target, base, ((int64_t)a->simm12) << 1);
    linx_gen_check_bstart_target(ctx, target);
    tcg_gen_mov_i64(cpu_pc, target);
    tcg_gen_exit_tb(NULL, 0);
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

static bool trans_mulu(DisasContext *ctx, arg_mulu *a)
{
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_mul_i64(out, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR));
    return trans_alu_binop(ctx, a->RegDst, out);
}

static bool trans_muluw(DisasContext *ctx, arg_muluw *a)
{
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_mul_i64(out, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR));
    return linx_binop_w(ctx, a->RegDst, out);
}

static bool trans_lhi_u(DisasContext *ctx, arg_lhi_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm12);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SW);
}

static bool trans_lhui_u(DisasContext *ctx, arg_lhui_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm12);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UW);
}

static bool trans_lwi_u(DisasContext *ctx, arg_lwi_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm12);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_SL);
}

static bool trans_lwui_u(DisasContext *ctx, arg_lwui_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm12);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UL);
}

static bool trans_ldi_u(DisasContext *ctx, arg_ldi_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm12);
    return linx_load_to_dest(ctx, a->RegDst, linx_addr_from_i64(addr64), MO_UQ);
}

static bool trans_shi_u(DisasContext *ctx, arg_shi_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, (int64_t)a->simm12);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UW);
}

static bool trans_swi_u(DisasContext *ctx, arg_swi_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, (int64_t)a->simm12);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UL);
}

static bool trans_sdi_u(DisasContext *ctx, arg_sdi_u *a)
{
    TCGv_i64 addr64 = linx_addr_add_imm(ctx, a->SrcR, (int64_t)a->simm12);
    return linx_store_from_reg(ctx, linx_addr_from_i64(addr64), linx_get_reg(a->SrcL), MO_UQ);
}

static bool trans_prf(DisasContext *ctx, arg_prf *a)
{
    (void)a->RegDst;
    (void)linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return true;
}

static bool trans_prfi_u(DisasContext *ctx, arg_prfi_u *a)
{
    (void)a->RegDst;
    (void)linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm12);
    return true;
}

static bool trans_lr_b(DisasContext *ctx, arg_lr_b *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_lr_b(val, tcg_env, linx_get_reg(a->SrcL));
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_lr_h(DisasContext *ctx, arg_lr_h *a)
{
    TCGv_i64 val = tcg_temp_new_i64();
    gen_helper_linx_lr_h(val, tcg_env, linx_get_reg(a->SrcL));
    linx_set_dest(a->RegDst, val);
    return true;
}

static bool trans_sc_b(DisasContext *ctx, arg_sc_b *a)
{
    TCGv_i64 res = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, linx_get_reg(a->SrcL));
    gen_helper_linx_sc_b(res, tcg_env, linx_get_reg(a->SrcR), val32);
    linx_set_dest(a->RegDst, res);
    return true;
}

static bool trans_sc_h(DisasContext *ctx, arg_sc_h *a)
{
    TCGv_i64 res = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, linx_get_reg(a->SrcL));
    gen_helper_linx_sc_h(res, tcg_env, linx_get_reg(a->SrcR), val32);
    linx_set_dest(a->RegDst, res);
    return true;
}

static bool trans_swapb(DisasContext *ctx, arg_swapb *a)
{
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, linx_get_reg(a->SrcR));
    gen_helper_linx_swapb(old, tcg_env, linx_get_reg(a->SrcL), val32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_swaph(DisasContext *ctx, arg_swaph *a)
{
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 val32 = tcg_temp_new_i32();
    tcg_gen_extrl_i64_i32(val32, linx_get_reg(a->SrcR));
    gen_helper_linx_swaph(old, tcg_env, linx_get_reg(a->SrcL), val32);
    linx_set_dest(a->RegDst, old);
    return true;
}

#define LINX_DEFINE_FETCH32(NAME, HELPER) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    TCGv_i64 old = tcg_temp_new_i64(); \
    TCGv_i32 val32 = tcg_temp_new_i32(); \
    tcg_gen_extrl_i64_i32(val32, linx_get_reg(a->SrcR)); \
    gen_helper_##HELPER(old, tcg_env, linx_get_reg(a->SrcL), val32); \
    linx_set_dest(a->RegDst, old); \
    return true; \
}

#define LINX_DEFINE_FETCH64(NAME, HELPER) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    TCGv_i64 old = tcg_temp_new_i64(); \
    gen_helper_##HELPER(old, tcg_env, linx_get_reg(a->SrcL), linx_get_reg(a->SrcR)); \
    linx_set_dest(a->RegDst, old); \
    return true; \
}

#define LINX_DEFINE_STORE32(NAME, HELPER) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    TCGv_i32 val32 = tcg_temp_new_i32(); \
    tcg_gen_extrl_i64_i32(val32, linx_get_reg(a->SrcL)); \
    gen_helper_##HELPER(tcg_env, linx_get_reg(a->SrcR), val32); \
    return true; \
}

#define LINX_DEFINE_STORE64(NAME, HELPER) \
static bool trans_##NAME(DisasContext *ctx, arg_##NAME *a) \
{ \
    gen_helper_##HELPER(tcg_env, linx_get_reg(a->SrcR), linx_get_reg(a->SrcL)); \
    return true; \
}

LINX_DEFINE_FETCH32(lw_and, linx_lw_and)
LINX_DEFINE_FETCH32(lw_or, linx_lw_or)
LINX_DEFINE_FETCH32(lw_xor, linx_lw_xor)
LINX_DEFINE_FETCH32(lw_smax, linx_lw_smax)
LINX_DEFINE_FETCH32(lw_smin, linx_lw_smin)
LINX_DEFINE_FETCH32(lw_umax, linx_lw_umax)
LINX_DEFINE_FETCH32(lw_umin, linx_lw_umin)
LINX_DEFINE_FETCH64(ld_and, linx_ld_and)
LINX_DEFINE_FETCH64(ld_or, linx_ld_or)
LINX_DEFINE_FETCH64(ld_xor, linx_ld_xor)
LINX_DEFINE_FETCH64(ld_smax, linx_ld_smax)
LINX_DEFINE_FETCH64(ld_smin, linx_ld_smin)
LINX_DEFINE_FETCH64(ld_umax, linx_ld_umax)
LINX_DEFINE_FETCH64(ld_umin, linx_ld_umin)
LINX_DEFINE_STORE32(sw_add, linx_sw_add)
LINX_DEFINE_STORE32(sw_and, linx_sw_and)
LINX_DEFINE_STORE32(sw_or, linx_sw_or)
LINX_DEFINE_STORE32(sw_xor, linx_sw_xor)
LINX_DEFINE_STORE32(sw_smax, linx_sw_smax)
LINX_DEFINE_STORE32(sw_smin, linx_sw_smin)
LINX_DEFINE_STORE32(sw_umax, linx_sw_umax)
LINX_DEFINE_STORE32(sw_umin, linx_sw_umin)
LINX_DEFINE_STORE64(sd_add, linx_sd_add)
LINX_DEFINE_STORE64(sd_and, linx_sd_and)
LINX_DEFINE_STORE64(sd_or, linx_sd_or)
LINX_DEFINE_STORE64(sd_xor, linx_sd_xor)
LINX_DEFINE_STORE64(sd_smax, linx_sd_smax)
LINX_DEFINE_STORE64(sd_smin, linx_sd_smin)
LINX_DEFINE_STORE64(sd_umax, linx_sd_umax)
LINX_DEFINE_STORE64(sd_umin, linx_sd_umin)

#undef LINX_DEFINE_FETCH32
#undef LINX_DEFINE_FETCH64
#undef LINX_DEFINE_STORE32
#undef LINX_DEFINE_STORE64

static bool trans_hl_setret(DisasContext *ctx, arg_hl_setret *a)
{
    return linx_setret_common(ctx, (int64_t)a->imm);
}

static bool trans_hl_lis(DisasContext *ctx, arg_hl_lis *a)
{
    linx_set_dest(a->RegDst, tcg_constant_i64((int64_t)a->simm));
    return true;
}

static bool trans_hl_liu(DisasContext *ctx, arg_hl_liu *a)
{
    linx_set_dest(a->RegDst, tcg_constant_i64((uint64_t)a->uimm));
    return true;
}

static bool trans_hl_miadd(DisasContext *ctx, arg_hl_miadd *a)
{
    TCGv_i64 prod = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_mul_i64(prod, linx_get_reg(a->SrcR), tcg_constant_i64((uint64_t)a->uimm19));
    tcg_gen_add_i64(out, linx_get_reg(a->SrcL), prod);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_misub(DisasContext *ctx, arg_hl_misub *a)
{
    TCGv_i64 prod = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();
    tcg_gen_mul_i64(prod, linx_get_reg(a->SrcR), tcg_constant_i64((uint64_t)a->uimm19));
    tcg_gen_sub_i64(out, linx_get_reg(a->SrcL), prod);
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_bfi(DisasContext *ctx, arg_hl_bfi *a)
{
    const unsigned m = a->immr & 0x3f;
    const unsigned s = a->imms & 0x3f;
    const unsigned n = (s >= m) ? (s - m + 1) : (s + (64u - m) + 1);
    TCGv_i64 base = linx_get_reg(a->SrcL);
    TCGv_i64 src = linx_get_reg(a->SrcR);
    TCGv_i64 rotated = tcg_temp_new_i64();
    TCGv_i64 merged = tcg_temp_new_i64();
    TCGv_i64 out = tcg_temp_new_i64();

    if (n == 64u) {
        linx_set_dest(a->RegDst, src);
        return true;
    }

    tcg_gen_rotri_i64(rotated, base, m);
    tcg_gen_deposit_i64(merged, rotated, src, 0, n);
    if (m != 0u) {
        tcg_gen_rotli_i64(out, merged, m);
    } else {
        tcg_gen_mov_i64(out, merged);
    }
    linx_set_dest(a->RegDst, out);
    return true;
}

static bool trans_hl_ccat(DisasContext *ctx, arg_hl_ccat *a)
{
    const unsigned shamt = a->shamt;
    TCGv_i64 src_l = linx_get_reg(a->SrcL);
    TCGv_i64 src_r = linx_get_reg(a->SrcR);
    TCGv_i64 out0 = tcg_temp_new_i64();
    TCGv_i64 out1 = tcg_temp_new_i64();

    if (shamt < 64) {
        tcg_gen_extract2_i64(out0, src_r, src_l, shamt);
        tcg_gen_shri_i64(out1, src_l, shamt);
    } else {
        tcg_gen_shri_i64(out0, src_l, shamt - 64);
        tcg_gen_movi_i64(out1, 0);
    }

    /* Compute both results before either destination can alias a source. */
    linx_set_dest(a->RegDst0, out0);
    linx_set_dest(a->RegDst1, out1);
    return true;
}

static bool trans_hl_ccatw(DisasContext *ctx, arg_hl_ccatw *a)
{
    const unsigned shamt = a->shamt;
    TCGv_i64 packed = tcg_temp_new_i64();
    TCGv_i64 shifted = tcg_temp_new_i64();
    TCGv_i64 out0 = tcg_temp_new_i64();
    TCGv_i64 out1 = tcg_temp_new_i64();

    if (shamt < 64) {
        tcg_gen_deposit_i64(packed, linx_get_reg(a->SrcR),
                            linx_get_reg(a->SrcL), 32, 32);
        tcg_gen_shri_i64(shifted, packed, shamt);
        tcg_gen_ext32s_i64(out0, shifted);
        tcg_gen_sari_i64(out1, shifted, 32);
    } else {
        tcg_gen_movi_i64(out0, 0);
        tcg_gen_movi_i64(out1, 0);
    }

    /* Compute both results before either destination can alias a source. */
    linx_set_dest(a->RegDst0, out0);
    linx_set_dest(a->RegDst1, out1);
    return true;
}

static bool trans_hl_setc_eqi(DisasContext *ctx, arg_hl_setc_eqi *a)
{
    return trans_setc_cmp_imm(ctx, TCG_COND_EQ, linx_get_reg(a->SrcL),
                              (int64_t)a->simm << a->shamt);
}

static bool trans_hl_setc_nei(DisasContext *ctx, arg_hl_setc_nei *a)
{
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, linx_get_reg(a->SrcL),
                              (int64_t)a->simm << a->shamt);
}

static bool trans_hl_setc_andi(DisasContext *ctx, arg_hl_setc_andi *a)
{
    TCGv_i64 out = tcg_temp_new_i64();
    int64_t simm = (int64_t)a->simm << a->shamt;

    tcg_gen_andi_i64(out, linx_get_reg(a->SrcL), simm);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_hl_setc_ori(DisasContext *ctx, arg_hl_setc_ori *a)
{
    TCGv_i64 out = tcg_temp_new_i64();
    int64_t simm = (int64_t)a->simm << a->shamt;

    tcg_gen_ori_i64(out, linx_get_reg(a->SrcL), simm);
    return trans_setc_cmp_imm(ctx, TCG_COND_NE, out, 0);
}

static bool trans_hl_setc_lti(DisasContext *ctx, arg_hl_setc_lti *a)
{
    return trans_setc_cmp_imm(ctx, TCG_COND_LT, linx_get_reg(a->SrcL),
                              (int64_t)a->simm << a->shamt);
}

static bool trans_hl_setc_gei(DisasContext *ctx, arg_hl_setc_gei *a)
{
    return trans_setc_cmp_imm(ctx, TCG_COND_GE, linx_get_reg(a->SrcL),
                              (int64_t)a->simm << a->shamt);
}

static bool trans_hl_setc_ltui(DisasContext *ctx, arg_hl_setc_ltui *a)
{
    return trans_setc_cmp_imm(ctx, TCG_COND_LTU, linx_get_reg(a->SrcL),
                              (int64_t)((uint64_t)a->uimm << a->shamt));
}

static bool trans_hl_setc_geui(DisasContext *ctx, arg_hl_setc_geui *a)
{
    return trans_setc_cmp_imm(ctx, TCG_COND_GEU, linx_get_reg(a->SrcL),
                              (int64_t)((uint64_t)a->uimm << a->shamt));
}

static bool trans_hl_prf(DisasContext *ctx, arg_hl_prf *a)
{
    (void)a->model;
    (void)linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    return true;
}

static bool trans_hl_prf_a(DisasContext *ctx, arg_hl_prf_a *a)
{
    TCGv_i64 ea = linx_addr_add_reg(ctx, a->SrcL, a->SrcR, a->SrcRType, a->shamt);
    (void)a->model;
    linx_set_dest(a->RegDst, ea);
    return true;
}

static bool trans_hl_prfi_u(DisasContext *ctx, arg_hl_prfi_u *a)
{
    (void)a->model;
    (void)linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm);
    return true;
}

static bool trans_hl_prfi_ua(DisasContext *ctx, arg_hl_prfi_ua *a)
{
    TCGv_i64 ea = linx_addr_add_imm(ctx, a->SrcL, (int64_t)a->simm);
    (void)a->model;
    linx_set_dest(a->RegDst, ea);
    return true;
}

static bool trans_hl_qmt(DisasContext *ctx, arg_hl_qmt *a)
{
    (void)a->SrcL;
    (void)a->SrcR;
    (void)a->e;
    (void)a->i;
    (void)a->r;
    (void)a->s;
    linx_set_dest(a->RegDst, tcg_constant_i64(0));
    return true;
}

static bool trans_hl_qpush(DisasContext *ctx, arg_hl_qpush *a)
{
    (void)a->SrcL;
    (void)a->SrcR;
    (void)a->e;
    (void)a->h;
    (void)a->r;
    linx_set_dest(a->RegDst, tcg_constant_i64(0));
    return true;
}

static bool trans_hl_qpop(DisasContext *ctx, arg_hl_qpop *a)
{
    (void)a->SrcL;
    (void)a->e;
    (void)a->r;
    linx_set_dest(a->RegDst0, tcg_constant_i64(0));
    linx_set_dest(a->RegDst1, tcg_constant_i64(0));
    return true;
}

static bool trans_hl_casb(DisasContext *ctx, arg_hl_casb *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 cmp64 = linx_get_reg(a->SrcR);
    TCGv_i64 new64 = linx_get_reg(a->SrcD1);
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 cmp32 = tcg_temp_new_i32();
    TCGv_i32 new32 = tcg_temp_new_i32();

    tcg_gen_extrl_i64_i32(cmp32, cmp64);
    tcg_gen_extrl_i64_i32(new32, new64);
    gen_helper_linx_casb(old, tcg_env, addr, cmp32, new32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_hl_cash(DisasContext *ctx, arg_hl_cash *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 cmp64 = linx_get_reg(a->SrcR);
    TCGv_i64 new64 = linx_get_reg(a->SrcD1);
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 cmp32 = tcg_temp_new_i32();
    TCGv_i32 new32 = tcg_temp_new_i32();

    tcg_gen_extrl_i64_i32(cmp32, cmp64);
    tcg_gen_extrl_i64_i32(new32, new64);
    gen_helper_linx_cash(old, tcg_env, addr, cmp32, new32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_hl_casw(DisasContext *ctx, arg_hl_casw *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 cmp64 = linx_get_reg(a->SrcR);
    TCGv_i64 new64 = linx_get_reg(a->SrcD1);
    TCGv_i64 old = tcg_temp_new_i64();
    TCGv_i32 cmp32 = tcg_temp_new_i32();
    TCGv_i32 new32 = tcg_temp_new_i32();

    tcg_gen_extrl_i64_i32(cmp32, cmp64);
    tcg_gen_extrl_i64_i32(new32, new64);
    gen_helper_linx_casw(old, tcg_env, addr, cmp32, new32);
    linx_set_dest(a->RegDst, old);
    return true;
}

static bool trans_hl_casd(DisasContext *ctx, arg_hl_casd *a)
{
    TCGv_i64 addr = linx_get_reg(a->SrcL);
    TCGv_i64 cmpv = linx_get_reg(a->SrcR);
    TCGv_i64 newv = linx_get_reg(a->SrcD1);
    TCGv_i64 old = tcg_temp_new_i64();

    gen_helper_linx_casd(old, tcg_env, addr, cmpv, newv);
    linx_set_dest(a->RegDst, old);
    return true;
}

static void linx_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULinxState *env = cpu_env(cpu);
    const vaddr pc = ctx->base.pc_first;

    ctx->env = env;
    ctx->brtype = (uint8_t)env->brtype;
    ctx->brtarget = 0;
    if (ctx->brtype == LINX_BR_COND ||
        ctx->brtype == LINX_BR_DIRECT ||
        ctx->brtype == LINX_BR_CALL) {
        /* Preserve fixed branch target when resuming from mid-block PCs. */
        ctx->brtarget = env->tgt;
    }
    ctx->cur_insn_len = 0;
    ctx->in_body = (ctx->base.tb->flags & LINX_TB_FLAG_IN_BODY) != 0;
    ctx->mem_idx = (ctx->base.tb->flags & LINX_TB_FLAG_USER_MMU) != 0 ? 1 : 0;
    ctx->decoupled_header = false;
    ctx->tgt_modified =
        (ctx->base.tb->flags & LINX_TB_FLAG_FUSED_ICALL) != 0;
    ctx->ra_set = (env->call_ra_set != 0);
    ctx->call_ra_target = ctx->ra_set ? env->gpr[LINX_REG_RA] : 0;
    ctx->block_insn_index = 0;

    /*
     * Entering a fresh block header must not inherit branch metadata from the
     * previously executed block. The new header will re-establish its own
     * brtype/target/return contract during decode.
     */
    if (!ctx->in_body && linx_is_bstart_at_pc(ctx, pc)) {
        ctx->brtype = 0;
        ctx->brtarget = 0;
        ctx->ra_set = false;
        ctx->call_ra_target = 0;
        return;
    }

    /*
     * A data fault can resume with stale FALL metadata and no owning header.
     * Drop that state only when BPC does not still identify the live block;
     * split TBs may enter a plain fallthrough block at a descriptor such as
     * SETC, and those descriptors must retain the header context.
     */
    if (!ctx->in_body && ctx->brtype == LINX_BR_FALL &&
        (env->bpc == pc || !linx_is_bstart_at_pc(ctx, env->bpc))) {
        ctx->brtype = 0;
        ctx->brtarget = 0;
        ctx->call_ra_target = 0;
        return;
    }

    /*
     * Non-header entries can come from precise user returns or from branches
     * to compiler-emitted continuation instructions.  If the live BPC is not a
     * block header either, the fixed-target state is stale metadata rather than
     * the suffix fragment's control-flow contract.  Translate the suffix as
     * fallthrough until the next header.
     */
    if (!ctx->in_body && ctx->brtype != 0 &&
        !linx_is_bstart_at_pc(ctx, pc) &&
        (env->bpc == pc || !linx_is_bstart_at_pc(ctx, env->bpc))) {
        ctx->brtype = LINX_BR_FALL;
        ctx->brtarget = 0;
        ctx->call_ra_target = 0;
        return;
    }

    /*
     * Branches can legally target non-header instructions (LLVM emits this in
     * libc startup paths). When we enter such a target, stale block metadata
     * from the source block must not be re-applied at the next header boundary.
     */
    if (linx_is_call_continuation(env, pc)) {
        ctx->brtype = LINX_BR_FALL;
        ctx->brtarget = 0;
        ctx->call_ra_target = 0;
        return;
    }

    if ((ctx->brtype == LINX_BR_COND ||
         ctx->brtype == LINX_BR_DIRECT ||
         ctx->brtype == LINX_BR_CALL) &&
        env->tgt == pc &&
        !linx_is_bstart_at_pc(ctx, pc)) {
        ctx->brtype = LINX_BR_FALL;
        ctx->brtarget = 0;
        ctx->call_ra_target = 0;
    }
}

static void linx_tr_tb_start(DisasContextBase *db, CPUState *cpu)
{
    if (linx_heartbeat_enabled) {
        TCGLabel *done = gen_new_label();
        tcg_gen_brcond_i64(TCG_COND_LTU, cpu_insn_count,
                           cpu_heartbeat_next_count, done);
        gen_helper_linx_heartbeat(tcg_env, tcg_constant_i64(db->pc_first));
        gen_set_label(done);
    }
    if (linx_pc_sample_enabled) {
        gen_helper_linx_pc_sample(tcg_env, tcg_constant_i64(db->pc_first));
    }
}

static void linx_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    tcg_gen_insn_start(ctx->base.pc_next);
    tcg_gen_addi_i64(cpu_insn_count, cpu_insn_count, 1);
}

static unsigned linx_insn_len(uint16_t hw)
{
    if ((hw & 0x1) == 0) {
        return ((hw & 0xf) == 0xe) ? 6 : 2;
    }
    return ((hw & 0xf) == 0xf) ? 8 : 4;
}

static bool linx_is_c_setret_hw(uint16_t hw)
{
    return (hw & 0xf83f) == 0x5016;
}

static uint8_t linx_c_setret_uimm5(uint16_t hw)
{
    return (uint8_t)((hw >> 6) & 0x1fu);
}

static bool linx_trans_fused_bstart_call(DisasContext *ctx,
                                         int64_t call_simm,
                                         uint8_t ret_uimm5,
                                         unsigned ret_base_offset)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;

    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }

    linx_block_begin(ctx, LINX_BR_CALL,
                     linx_pcrel_target(current_pc, call_simm));

    /*
     * Both exact fused forms carry the call and return displacements in one
     * architectural instruction. The return field uses the address of its
     * embedded compact SETRET component as its PC base: P+2 for the 16+16
     * form and P+4 for the 32+16 form.
     */
    {
        vaddr ret_target = current_pc + ret_base_offset +
                           ((vaddr)ret_uimm5 << 1);
        linx_set_dest(LINX_REG_RA, tcg_constant_i64(ret_target));
        if (linx_call_trace_translate_enabled) {
            gen_helper_linx_call_trace_event(
                tcg_env, tcg_constant_i64(current_pc + ret_base_offset),
                tcg_constant_i32(LINX_CALL_TRACE_SETRET),
                tcg_constant_i64(ret_target), tcg_constant_i64(0));
        }
        ctx->ra_set = true;
        ctx->call_ra_target = ret_target;
        tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                       offsetof(CPULinxState, call_ra_set));
        tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                       offsetof(CPULinxState, call_setret_pending));
    }
    return true;
}

static bool trans_start_call_32(DisasContext *ctx, arg_start_call_32 *a)
{
    return linx_trans_fused_bstart_call(ctx, a->simm12, a->uimm5, 2);
}

static bool trans_start_icall_32(DisasContext *ctx, arg_start_icall_32 *a)
{
    vaddr current_pc = ctx->base.pc_next - ctx->cur_insn_len;
    vaddr ret_target = current_pc + 2u + ((vaddr)a->uimm5 << 1);
    TCGv_i64 snapshot = tcg_temp_new_i64();

    if (ctx->in_body) {
        return linx_block_fault(ctx, LINX_EBLOCK_LEGACY_ILLEGAL_IN_BODY, 0);
    }
    if (current_pc != ctx->base.pc_first) {
        linx_gen_block_end(ctx, current_pc);
        return true;
    }
    tcg_gen_mov_i64(snapshot, cpu_tgt);
    /* Validate the retiring BARG target before changing BPC, RA, or BARG. */
    linx_gen_check_bstart_target(ctx, snapshot);
    linx_block_begin(ctx, LINX_BR_ICALL, 0);
    tcg_gen_st_i64(snapshot, tcg_env,
                   offsetof(CPULinxState, fused_icall_target));
    tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                   offsetof(CPULinxState, fused_icall_target_valid));
    linx_set_dest(LINX_REG_RA, tcg_constant_i64(ret_target));
    ctx->tgt_modified = true;
    ctx->ra_set = true;
    ctx->call_ra_target = ret_target;
    tcg_gen_st_i32(tcg_constant_i32(1), tcg_env,
                   offsetof(CPULinxState, call_ra_set));
    tcg_gen_st_i32(tcg_constant_i32(0), tcg_env,
                   offsetof(CPULinxState, call_setret_pending));
    return true;
}

static bool trans_start_call_48(DisasContext *ctx, arg_start_call_48 *a)
{
    return linx_trans_fused_bstart_call(ctx, a->simm25, a->uimm5, 4);
}

static void linx_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);
    CPULinxState *env = cpu_env(cpu);
    vaddr pc = ctx->base.pc_next;
    uint16_t hw;
    unsigned len;
    bool decoded = false;
    uint32_t insn_val = 0;

    /*
     * Arm co-sim exactly at instruction start so trigger snapshot captures
     * pre-execution architectural state for the trigger instruction.
     */
    if (linx_host_insn_hook_enabled_p(pc) ||
        (ctx->base.tb->flags & LINX_TB_FLAG_COSIM_PRECHECK)) {
        gen_helper_linx_cosim_before_insn(tcg_env, tcg_constant_i64(pc));
    }

    /* LinxISA is little-endian: bytes in memory like [00 08] should be read as 0x0800 */
    /* Use MO_LE to read instruction in little-endian format */
    hw = translator_lduw_end(env, &ctx->base, pc, MO_LE);
    
    len = linx_insn_len(hw);
    /*
     * HL.BSTART.CALL is a 32+16 fused form whose low halfword has the normal
     * 32-bit BSTART prefix. Peek at the extension before committing to the
     * base length, and only promote the exact golden mask/match to six bytes.
     */
    if (len == 4 && (hw & 0x007fu) == 0x0011u) {
        uint64_t candidate;
        LinxFusedCallProbe probe =
            linx_probe_hl_fused_call(ctx, pc, hw, &candidate);

        if (probe == LINX_FUSED_CALL_UNAVAILABLE) {
            /* Restart with this PC as the first page instead of executing the
             * low 32 bits before the extension can be inspected. */
            ctx->base.is_jmp = DISAS_TOO_MANY;
            ctx->base.pc_next = pc;
            return;
        }
        if (probe == LINX_FUSED_CALL_MATCH) {
            len = 6;
        }
    }
    ctx->cur_insn_len = len;
    /* Always update pc_next to ensure tb->size is non-zero even if exception occurs */
    ctx->base.pc_next = pc + len;
    tcg_gen_movi_i64(cpu_insn_pc_next, ctx->base.pc_next);
    if (ctx->base.tb->flags & LINX_TB_FLAG_DBG_ACTIVE) {
        gen_helper_linx_dbg_check_pc(tcg_env, tcg_constant_i64(pc));
    }

    switch (len) {
           case 2:
               insn_val = hw;
               if ((hw & 0x000f) == 0x0002) {
                   uint16_t hw2 = translator_lduw_end(env, &ctx->base,
                                                       pc + 2, MO_LE);
                   if (linx_is_c_setret_hw(hw2)) {
                       len = 4;
                       ctx->cur_insn_len = len;
                       ctx->base.pc_next = pc + len;
                       tcg_gen_movi_i64(cpu_insn_pc_next, ctx->base.pc_next);
                       insn_val = (uint32_t)hw | ((uint32_t)hw2 << 16);
                       ctx->cur_insn_raw = insn_val;
                       linx_trace_begin(pc, (uint64_t)insn_val, len);
                       decoded = linx_trans_fused_bstart_call(
                           ctx, sextract32(hw, 4, 12),
                           linx_c_setret_uimm5(hw2), 2);
                       if (decoded) {
                           trace_linx_insn_exec(pc, insn_val, len,
                                                "c-bstart-call-setret");
                       }
                       break;
                   }
               }
               linx_trace_begin(pc, (uint64_t)hw, len);
               /* Explicit check for c_bstop (0x0000) - decode tree should handle this but workaround for now */
               if (hw == 0x0000) {
                   union {
                       arg_decode_insn160 f_decode_insn160;
                   } u = { };
                   decoded = trans_c_bstop(ctx, &u.f_decode_insn160);
                   if (decoded) {
                       trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                   }
               } else if ((hw & 0xc7ff) == 0x0000) {
                   /* C.BSTART.STD: mask=0xc7ff, match=0x0000, BrType in bits [13:11] */
                   uint8_t brtype = (hw >> 11) & 0x7;
                   if (brtype != 0) {
                       /* Handle C.BSTART.STD with non-zero BrType */
                       decoded = linx_trans_c_bstart_std(ctx, brtype);
                       if (decoded) {
                           trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                       }
                   } else {
                       /* BrType=0 is c_bstop */
                       union {
                           arg_decode_insn160 f_decode_insn160;
                       } u = { };
                       decoded = trans_c_bstop(ctx, &u.f_decode_insn160);
                       if (decoded) {
                           trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                       }
                   }
               } else if (linx_is_c_setret_hw(hw)) {
                   /* C.SETRET compact alias (decoded outside decodetree due overlap with C.MOVI). */
                   decoded = linx_setret_common(ctx,
                                                (int64_t)linx_c_setret_uimm5(hw));
                   if (decoded) {
                       trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                   }
               } else {
                   ctx->cur_insn_raw = hw;
                   if (!linx_validate_opcode_meta(ctx, pc, (uint64_t)hw, len)) {
                       break;
                   }
                   decoded = decode_insn16(ctx, hw);
                   if (decoded) {
                       trace_linx_insn_exec(pc, insn_val, len, "16-bit");
                   }
               }
               if (!decoded) {
                   trace_linx_insn_decode_fail_raw(pc, (uint64_t)hw, len);
                   qemu_log_mask(LOG_GUEST_ERROR, "Linx: decode failed @ PC=0x%" VADDR_PRIx
                                " hw=0x%04x len=%u\n", pc, hw, len);
                   linx_illegal(ctx);
               }
               break;
    case 4: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, MO_LE);
        insn_val = (uint32_t)hw | ((uint32_t)hw2 << 16);
        ctx->cur_insn_raw = insn_val;
        linx_trace_begin(pc, (uint64_t)insn_val, len);
        if (!linx_validate_opcode_meta(ctx, pc, (uint64_t)insn_val, len)) {
            break;
        }
        decoded = decode_insn32(ctx, insn_val);
        if (!decoded) {
            trace_linx_insn_decode_fail_raw(pc, (uint64_t)insn_val, len);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: decode32 failed @ PC=0x%" VADDR_PRIx " insn=0x%08x\n",
                          pc, insn_val);
            linx_illegal(ctx);
        } else {
            trace_linx_insn_exec(pc, insn_val, len, "32-bit");
        }
        break;
    }
    case 6: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, MO_LE);
        uint16_t hw3 = translator_lduw_end(env, &ctx->base, pc + 4, MO_LE);
        uint32_t hi = (uint32_t)hw2 | ((uint32_t)hw3 << 16);
        uint64_t insn48 = (uint64_t)hw | ((uint64_t)hi << 16);
        ctx->cur_insn_raw = insn48;
        trace_linx_insn_decode48(pc, insn48,
                                 (uint64_t)(insn48 & 0xffff0000007f000full));
        insn_val = (uint32_t)(insn48 & 0xFFFFFFFF);
        linx_trace_begin(pc, insn48, len);
        if (!linx_validate_opcode_meta(ctx, pc, insn48, len)) {
            break;
        }
        decoded = decode_insn48(ctx, insn48);
        if (!decoded) {
            trace_linx_insn_decode_fail_raw(pc, insn48, len);
            linx_illegal(ctx);
        } else {
            trace_linx_insn_exec(pc, insn_val, len, "48-bit");
        }
        break;
    }
    case 8: {
        uint16_t hw2 = translator_lduw_end(env, &ctx->base, pc + 2, MO_LE);
        uint16_t hw3 = translator_lduw_end(env, &ctx->base, pc + 4, MO_LE);
        uint16_t hw4 = translator_lduw_end(env, &ctx->base, pc + 6, MO_LE);
        uint32_t hi = (uint32_t)hw2 | ((uint32_t)hw3 << 16);
        uint32_t top = (uint32_t)hw4;
        uint64_t insn64 = (uint64_t)hw |
                          ((uint64_t)hi << 16) |
                          ((uint64_t)top << 48);
        ctx->cur_insn_raw = insn64;
        insn_val = (uint32_t)(insn64 & 0xFFFFFFFFu);
        linx_trace_begin(pc, insn64, len);
        if (!linx_validate_opcode_meta(ctx, pc, insn64, len)) {
            break;
        }
        decoded = decode_insn64(ctx, insn64);
        if (!decoded) {
            trace_linx_insn_decode_fail_raw(pc, insn64, len);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Linx: decode64 failed @ PC=0x%" VADDR_PRIx
                          " insn=0x%016" PRIx64 "\n",
                          pc, insn64);
            linx_illegal(ctx);
        } else {
            trace_linx_insn_exec(pc, insn_val, len, "64-bit");
        }
        break;
    }
    default:
        linx_illegal(ctx);
        break;
    }

    if (linx_commit_trace_enabled && ctx->base.is_jmp != DISAS_NORETURN) {
        gen_helper_linx_commit_trace(tcg_env, tcg_constant_i64(ctx->base.pc_next));
    }

    if (decoded && ctx->brtype != 0 && ctx->base.is_jmp != DISAS_NORETURN) {
        ctx->block_insn_index++;
    }

    /* Always update pc_next to ensure tb->size is non-zero */
    ctx->base.pc_next = pc + len;
}

static void linx_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *ctx = container_of(dcbase, DisasContext, base);

    switch (ctx->base.is_jmp) {
    case DISAS_NEXT:
        tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
        tcg_gen_exit_tb(NULL, 0);
        break;
    case DISAS_TOO_MANY:
        if (linx_is_bstart_at_pc(ctx, ctx->base.pc_next)) {
            linx_gen_block_end(ctx, ctx->base.pc_next);
        } else {
            tcg_gen_movi_i64(cpu_pc, ctx->base.pc_next);
            tcg_gen_exit_tb(NULL, 0);
        }
        break;
    case DISAS_NORETURN:
        break;
    default:
        g_assert_not_reached();
    }
}

static const TranslatorOps linx_tr_ops = {
    .init_disas_context = linx_tr_init_disas_context,
    .tb_start = linx_tr_tb_start,
    .insn_start = linx_tr_insn_start,
    .translate_insn = linx_tr_translate_insn,
    .tb_stop = linx_tr_tb_stop,
};

void linx_translate_code(CPUState *cs, TranslationBlock *tb,
                         int *max_insns, vaddr pc, void *host_pc)
{
    DisasContext dc;
    translator_loop(cs, tb, max_insns, pc, host_pc, &linx_tr_ops, &dc.base);
}

void linx_translate_init(void)
{
    int i;
    const char *commit_trace = getenv("LINX_COMMIT_TRACE");
    const char *minst_trace = getenv("LINX_MINST_TRACE");
    const char *cosim_enable = getenv("LINX_COSIM_ENABLE");
    const char *opcode_meta_strict = getenv("LINX_OPCODE_META_STRICT");
    const char *pc_sample_interval = getenv("LINX_PC_SAMPLE_INTERVAL");
    const char *heartbeat_interval = getenv("LINX_HEARTBEAT_INTERVAL");
    const char *qemu_heartbeat_interval = getenv("LINX_QEMU_HEARTBEAT_INTERVAL");
    const char *call_trace = getenv("LINX_CALL_TRACE");
    const char *call_trace_ring = getenv("LINX_CALL_TRACE_RING");
    const char *template_chain = getenv("LINX_QEMU_TEMPLATE_CHAIN");
    const char *mem_trace = getenv("LINX_MEM_TRACE");
    const char *mem_trace_addr = getenv("LINX_MEM_TRACE_ADDR");
    const char *mem_trace_size = getenv("LINX_MEM_TRACE_SIZE");
    const char *mem_trace_access = getenv("LINX_MEM_TRACE_ACCESS");
    const char *mem_trace_fast = getenv("LINX_MEM_TRACE_FAST");
    const char *mem_trace_pc = getenv("LINX_MEM_TRACE_PC");
    const char *mem_trace_pc_lo = getenv("LINX_MEM_TRACE_PC_LO");
    const char *mem_trace_pc_hi = getenv("LINX_MEM_TRACE_PC_HI");
    const char *mem_trace_pre = getenv("LINX_MEM_TRACE_PRE");
    uint64_t mem_trace_addr_value = 0;
    uint64_t mem_trace_size_value = 8;
    uint64_t mem_trace_pc_value = 0;
    uint64_t mem_trace_pc_lo_value = 0;
    uint64_t mem_trace_pc_hi_value = 0;
    static const char *gpr_names[LINX_GPR_COUNT] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23"
    };
    static const char *tq_names[4] = { "t#1", "t#2", "t#3", "t#4" };
    static const char *uq_names[4] = { "u#1", "u#2", "u#3", "u#4" };

    linx_commit_trace_enabled =
        (commit_trace && commit_trace[0] && strcmp(commit_trace, "0") != 0) ||
        (minst_trace && minst_trace[0] && strcmp(minst_trace, "0") != 0) ||
        (cosim_enable && cosim_enable[0] && strcmp(cosim_enable, "0") != 0) ||
        qemu_loglevel_mask(LOG_LINX_MEM);
    linx_opcode_meta_strict = !(opcode_meta_strict && opcode_meta_strict[0] && strcmp(opcode_meta_strict, "0") == 0);
    linx_pc_sample_enabled =
        pc_sample_interval && pc_sample_interval[0] &&
        strcmp(pc_sample_interval, "0") != 0;
    linx_heartbeat_enabled =
        (heartbeat_interval && heartbeat_interval[0] &&
         strcmp(heartbeat_interval, "0") != 0) ||
        (qemu_heartbeat_interval && qemu_heartbeat_interval[0] &&
         strcmp(qemu_heartbeat_interval, "0") != 0);
    linx_call_trace_translate_enabled =
        (call_trace && call_trace[0] && strcmp(call_trace, "0") != 0) ||
        (call_trace_ring && call_trace_ring[0] &&
         strcmp(call_trace_ring, "0") != 0);
    linx_template_chain_enabled =
        template_chain && template_chain[0] && strcmp(template_chain, "0") != 0;
    linx_mem_trace_translate_addr_filter_enabled =
        mem_trace_addr && mem_trace_addr[0] &&
        linx_translate_parse_u64(mem_trace_addr, &mem_trace_addr_value);
    if (mem_trace_pc && mem_trace_pc[0] &&
        linx_translate_parse_u64(mem_trace_pc, &mem_trace_pc_value)) {
        linx_mem_trace_translate_pc_lo = mem_trace_pc_value;
        linx_mem_trace_translate_pc_hi = mem_trace_pc_value;
        linx_mem_trace_translate_pc_filter_enabled = true;
    } else if (mem_trace_pc_lo && mem_trace_pc_lo[0] &&
               mem_trace_pc_hi && mem_trace_pc_hi[0] &&
               linx_translate_parse_u64(mem_trace_pc_lo,
                                        &mem_trace_pc_lo_value) &&
               linx_translate_parse_u64(mem_trace_pc_hi,
                                        &mem_trace_pc_hi_value)) {
        linx_mem_trace_translate_pc_lo =
            MIN(mem_trace_pc_lo_value, mem_trace_pc_hi_value);
        linx_mem_trace_translate_pc_hi =
            MAX(mem_trace_pc_lo_value, mem_trace_pc_hi_value);
        linx_mem_trace_translate_pc_filter_enabled = true;
    }
    linx_mem_trace_translate_enabled =
        (mem_trace && mem_trace[0] && strcmp(mem_trace, "0") != 0) ||
        linx_mem_trace_translate_addr_filter_enabled;
    linx_mem_trace_translate_pre_enabled =
        linx_mem_trace_translate_enabled &&
        mem_trace_pre && mem_trace_pre[0] && strcmp(mem_trace_pre, "0") != 0;
    if (linx_mem_trace_translate_enabled && mem_trace_access &&
        mem_trace_access[0]) {
        if (strcmp(mem_trace_access, "load") == 0 ||
            strcmp(mem_trace_access, "loads") == 0) {
            linx_mem_trace_translate_loads = true;
            linx_mem_trace_translate_stores = false;
        } else if (strcmp(mem_trace_access, "store") == 0 ||
                   strcmp(mem_trace_access, "stores") == 0) {
            linx_mem_trace_translate_loads = false;
            linx_mem_trace_translate_stores = true;
        }
    }
    linx_mem_trace_translate_fast_enabled = false;
    if (linx_mem_trace_translate_enabled &&
        linx_mem_trace_translate_addr_filter_enabled &&
        !(mem_trace_fast && mem_trace_fast[0] &&
          strcmp(mem_trace_fast, "0") == 0)) {
        if (mem_trace_size && mem_trace_size[0] &&
            strcmp(mem_trace_size, "0") != 0) {
            (void)linx_translate_parse_u64(mem_trace_size,
                                           &mem_trace_size_value);
        }
        if (mem_trace_size_value == 0) {
            mem_trace_size_value = 1;
        }
        linx_mem_trace_translate_addr = mem_trace_addr_value;
        linx_mem_trace_translate_end =
            mem_trace_addr_value + mem_trace_size_value - 1;
        if (linx_mem_trace_translate_end < mem_trace_addr_value) {
            linx_mem_trace_translate_end = UINT64_MAX;
        }
        linx_mem_trace_translate_fast_enabled = true;
    }
    
    for (i = 0; i < LINX_GPR_COUNT; i++) {
        cpu_gpr[i] = tcg_global_mem_new_i64(tcg_env,
                                            offsetof(CPULinxState, gpr[i]),
                                            gpr_names[i]);
    }
    for (i = 0; i < 4; i++) {
        cpu_tq[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPULinxState, tq[i]),
                                           tq_names[i]);
        cpu_uq[i] = tcg_global_mem_new_i64(tcg_env,
                                           offsetof(CPULinxState, uq[i]),
                                           uq_names[i]);
    }
    cpu_bpc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, bpc), "bpc");
    cpu_tgt = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, tgt), "tgt");
    cpu_cond = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, cond), "cond");
    cpu_vec_p = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, vec_p), "vec_p");
    cpu_carg = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, carg), "carg");
    cpu_brtype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, brtype), "brtype");
    cpu_blocktype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, blocktype), "blocktype");
    cpu_body_tpc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, body_tpc), "body_tpc");
    cpu_body_end = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, body_end), "body_end");
    cpu_return_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, return_pc), "return_pc");
    cpu_in_body = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, in_body), "in_body");
    cpu_tile_func = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_func), "tile_func");
    cpu_tile_dtype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_dtype), "tile_dtype");
    cpu_tile_iot_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_valid), "tile_iot_valid");
    cpu_tile_iot_flags = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_flags), "tile_iot_flags");
    cpu_tile_iot_dst = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_dst), "tile_iot_dst");
    cpu_tile_iot_grp = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_grp), "tile_iot_grp");
    cpu_tile_iot_src0 = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_src0), "tile_iot_src0");
    cpu_tile_iot_src1 = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_src1), "tile_iot_src1");
    cpu_tile_iot_reg = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_reg), "tile_iot_reg");
    cpu_tile_iot_size = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_iot_size), "tile_iot_size");
    cpu_tile_attr_raw = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_attr_raw), "tile_attr_raw");
    cpu_tile_attr_pad = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_attr_pad), "tile_attr_pad");
    cpu_tile_attr_dtype = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, tile_attr_dtype), "tile_attr_dtype");
    cpu_lb[0] = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, lb[0]), "lb0");
    cpu_lb[1] = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, lb[1]), "lb1");
    cpu_lb[2] = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, lb[2]), "lb2");
    cpu_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, pc), "pc");
    cpu_insn_pc_next = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, insn_pc_next), "insn_pc_next");
    cpu_insn_count = tcg_global_mem_new_i64(tcg_env,
                                            offsetof(CPULinxState, insn_count),
                                            "insn_count");
    cpu_heartbeat_next_count =
        tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, heartbeat_next_count),
                               "heartbeat_next_count");
    cpu_pending_trap_arg0 =
        tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, pending_trap_arg0),
                               "pending_trap_arg0");
    cpu_pending_trap_cause =
        tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, pending_trap_cause),
                               "pending_trap_cause");

    if (linx_commit_trace_enabled) {
        cpu_trace_pc = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_pc), "trace_pc");
        cpu_trace_insn = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_insn), "trace_insn");
        cpu_trace_len = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_len), "trace_len");
        cpu_trace_wb_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_wb_valid), "trace_wb_valid");
        cpu_trace_wb_rd = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_wb_rd), "trace_wb_rd");
        cpu_trace_wb_data = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_wb_data), "trace_wb_data");
        cpu_trace_mem_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_mem_valid), "trace_mem_valid");
        cpu_trace_mem_is_store = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_mem_is_store), "trace_mem_is_store");
        cpu_trace_mem_addr = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_mem_addr), "trace_mem_addr");
        cpu_trace_mem_wdata = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_mem_wdata), "trace_mem_wdata");
        cpu_trace_mem_rdata = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_mem_rdata), "trace_mem_rdata");
        cpu_trace_mem_size = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_mem_size), "trace_mem_size");
        cpu_trace_trap_valid = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_trap_valid), "trace_trap_valid");
        cpu_trace_trap_cause = tcg_global_mem_new_i32(tcg_env, offsetof(CPULinxState, trace_trap_cause), "trace_trap_cause");
        cpu_trace_traparg0 = tcg_global_mem_new_i64(tcg_env, offsetof(CPULinxState, trace_traparg0), "trace_traparg0");
    }

}
