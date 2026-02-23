/*
 * Standalone OEX Trace Manager plugin for QEMU.
 *
 * Phase-1 behavior:
 * - single-vCPU shadow checker
 * - in-process runtime ABI via dlopen
 * - passive memory oracle
 */

#include <glib.h>
#include <inttypes.h>
#include <qemu-plugin.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_manager_proto.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    uint64_t pc;
    uint64_t raw;
    uint8_t len;
} InsnMeta;

typedef struct {
    uint64_t seq;
    uint16_t mem_idx;
    uint8_t kind;
    uint64_t addr;
    uint8_t size;
    uint64_t value;
} MemEvent;

typedef struct {
    oex_mem_req_t req;
    uint32_t age;
} PendingReq;

static qemu_plugin_id_t g_plugin_id;
static GSList *g_insn_metas;
static bool g_verbose;
static bool g_failfast = true;
static bool g_fatal;

static uint64_t g_next_seq;
static uint64_t g_active_seq = UINT64_MAX;
static uint16_t g_active_mem_idx;

static uint32_t g_max_oex_cycles_per_insn = 4;
static uint32_t g_pump_insn_quantum = 4;
static uint32_t g_insn_since_pump = 0;

static FILE *g_retire_fp;
static char *g_retire_path;
static FILE *g_occ_fp;
static char *g_occ_path;

static void *g_oex_so;
static oex_ctx_t *g_oex;
static oex_create_fn g_oex_create;
static oex_destroy_fn g_oex_destroy;
static oex_push_inst_fn g_oex_push_inst;
static oex_tick_fn g_oex_tick;
static oex_take_mem_req_fn g_oex_take_mem_req;
static oex_push_mem_rsp_fn g_oex_push_mem_rsp;
static oex_pop_retire_fn g_oex_pop_retire;
static oex_pop_trace_occ_fn g_oex_pop_trace_occ;
static oex_last_error_fn g_oex_last_error;

/* key: uint64_t encoded (seq<<17 | mem_idx<<1 | kind), value: MemEvent* */
static GHashTable *g_mem_events;
static GQueue *g_pending_reqs;

static uint64_t key_for(uint64_t seq, uint16_t mem_idx, uint8_t kind)
{
    return (seq << 17) | ((uint64_t)mem_idx << 1) | (uint64_t)(kind & 1u);
}

static void set_fatal(const char *msg)
{
    if (g_fatal) {
        return;
    }
    g_fatal = true;
    fprintf(stderr, "trace_manager fatal: %s\n", msg ? msg : "unknown");
}

static void runtime_last_error(char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (!g_oex || !g_oex_last_error) {
        return;
    }
    if (g_oex_last_error(g_oex, buf, cap) != 0) {
        g_strlcpy(buf, "oex_last_error failed", cap);
    }
}

static uint64_t mem_value_to_u64(qemu_plugin_mem_value v)
{
    switch (v.type) {
    case QEMU_PLUGIN_MEM_VALUE_U8:
        return (uint64_t)v.data.u8;
    case QEMU_PLUGIN_MEM_VALUE_U16:
        return (uint64_t)v.data.u16;
    case QEMU_PLUGIN_MEM_VALUE_U32:
        return (uint64_t)v.data.u32;
    case QEMU_PLUGIN_MEM_VALUE_U64:
        return (uint64_t)v.data.u64;
    case QEMU_PLUGIN_MEM_VALUE_U128:
        return (uint64_t)v.data.u128.low;
    default:
        return 0;
    }
}

static bool occ_payload_valid(const oex_trace_occ_t *e)
{
    if (!e) {
        return false;
    }
    if (e->stage_id > OEX_TRACE_STAGE_IB) {
        return false;
    }
    if (e->lane_id > OEX_TRACE_LANE_AUX) {
        return false;
    }
    return true;
}

static void write_retire_row(const oex_retire_row_t *r)
{
    if (!g_retire_fp || !r) {
        return;
    }
    fprintf(
        g_retire_fp,
        "{\"seq\":%" PRIu64 ",\"pc\":%" PRIu64 ",\"insn\":%" PRIu64 ",\"len\":%u,"
        "\"src0_valid\":%u,\"src0_reg\":%u,\"src0_data\":%" PRIu64 ","
        "\"src1_valid\":%u,\"src1_reg\":%u,\"src1_data\":%" PRIu64 ","
        "\"dst_valid\":%u,\"dst_reg\":%u,\"dst_data\":%" PRIu64 ","
        "\"mem_valid\":%u,\"mem_is_store\":%u,\"mem_addr\":%" PRIu64 ","
        "\"mem_wdata\":%" PRIu64 ",\"mem_rdata\":%" PRIu64 ",\"mem_size\":%u,"
        "\"trap_valid\":%u,\"trap_cause\":%u,\"traparg0\":%" PRIu64 ","
        "\"next_pc\":%" PRIu64 "}\n",
        r->seq,
        r->pc,
        r->raw,
        (unsigned)r->len,
        (unsigned)r->src0_valid,
        (unsigned)r->src0_reg,
        r->src0_data,
        (unsigned)r->src1_valid,
        (unsigned)r->src1_reg,
        r->src1_data,
        (unsigned)r->dst_valid,
        (unsigned)r->dst_reg,
        r->dst_data,
        (unsigned)r->mem_valid,
        (unsigned)r->mem_is_store,
        r->mem_addr,
        r->mem_wdata,
        r->mem_rdata,
        (unsigned)r->mem_size,
        (unsigned)r->trap_valid,
        (unsigned)r->trap_cause,
        r->traparg0,
        r->next_pc);
    fflush(g_retire_fp);
}

static void write_occ_event(const oex_trace_occ_t *e)
{
    if (!g_occ_fp || !e) {
        return;
    }
    fprintf(
        g_occ_fp,
        "{\"cycle\":%" PRIu64 ",\"seq\":%" PRIu64 ",\"uop_uid\":%" PRIu64 ","
        "\"parent_uop_uid\":%" PRIu64 ",\"pc\":%" PRIu64 ",\"raw\":%" PRIu64 ","
        "\"len\":%u,\"stage_id\":%u,\"lane_id\":%u,\"stall\":%u,"
        "\"cause\":%u,\"is_gen_uop\":%u}\n",
        e->cycle,
        e->seq,
        e->uop_uid,
        e->parent_uop_uid,
        e->pc,
        e->raw,
        (unsigned)e->len,
        (unsigned)e->stage_id,
        (unsigned)e->lane_id,
        (unsigned)e->stall,
        (unsigned)e->cause,
        (unsigned)e->is_gen_uop);
    fflush(g_occ_fp);
}

static bool append_pending_req(const oex_mem_req_t *req)
{
    PendingReq *pr = g_new0(PendingReq, 1);
    if (!pr) {
        return false;
    }
    pr->req = *req;
    pr->age = 0;
    g_queue_push_tail(g_pending_reqs, pr);
    return true;
}

static void resolve_pending_reqs(void)
{
    if (!g_pending_reqs || !g_mem_events || !g_oex || g_fatal) {
        return;
    }

    guint n = g_queue_get_length(g_pending_reqs);
    for (guint i = 0; i < n; ++i) {
        PendingReq *pr = (PendingReq *)g_queue_pop_head(g_pending_reqs);
        if (!pr) {
            continue;
        }

        uint64_t key = key_for(pr->req.seq, pr->req.mem_idx, (uint8_t)pr->req.kind);
        MemEvent *ev = (MemEvent *)g_hash_table_lookup(g_mem_events, &key);
        if (ev) {
            if (pr->req.kind == OEX_MEM_STORE && pr->req.data != ev->value) {
                set_fatal("store-data mismatch in passive oracle");
                g_free(pr);
                continue;
            }

            oex_mem_rsp_t rsp = {
                .seq = pr->req.seq,
                .mem_idx = pr->req.mem_idx,
                .ok = 1,
                .data = (pr->req.kind == OEX_MEM_LOAD) ? ev->value : 0,
            };
            if (g_oex_push_mem_rsp(g_oex, &rsp) != 0) {
                char err[256];
                runtime_last_error(err, sizeof(err));
                if (err[0] != '\0') {
                    fprintf(stderr, "trace_manager: oex_push_mem_rsp failed: %s\n", err);
                }
                set_fatal("oex_push_mem_rsp failed");
            }

            g_hash_table_remove(g_mem_events, &key);
            g_free(pr);
            continue;
        }

        pr->age++;
        if (g_failfast && pr->age > 4096) {
            set_fatal("unresolved memory request");
            g_free(pr);
            continue;
        }
        g_queue_push_tail(g_pending_reqs, pr);
    }
}

static bool pump_runtime(uint32_t budget, bool break_on_idle)
{
    if (!g_oex || g_fatal) {
        return true;
    }
    bool idle = false;
    for (uint32_t step = 0; step < budget; ++step) {
        if (g_oex_tick(g_oex, 1) != 0) {
            char err[256];
            runtime_last_error(err, sizeof(err));
            if (err[0] != '\0') {
                fprintf(stderr, "trace_manager: oex_tick failed: %s\n", err);
            }
            set_fatal("oex_tick failed");
            return false;
        }

        oex_mem_req_t reqs[32];
        size_t nreq = g_oex_take_mem_req(g_oex, reqs, 32);
        for (size_t i = 0; i < nreq; ++i) {
            if (!append_pending_req(&reqs[i])) {
                set_fatal("failed to queue pending mem request");
                return false;
            }
        }

        resolve_pending_reqs();

        oex_retire_row_t rows[32];
        size_t nret = g_oex_pop_retire(g_oex, rows, 32);
        for (size_t i = 0; i < nret; ++i) {
            write_retire_row(&rows[i]);
        }

        size_t nocc = 0;
        if (g_oex_pop_trace_occ) {
            oex_trace_occ_t occ[128];
            nocc = g_oex_pop_trace_occ(g_oex, occ, 128);
            for (size_t i = 0; i < nocc; ++i) {
                if (!occ_payload_valid(&occ[i])) {
                    set_fatal("malformed oex OCC payload");
                    return false;
                }
                write_occ_event(&occ[i]);
            }
        }

        idle = (nreq == 0 && nret == 0 && nocc == 0 && g_queue_is_empty(g_pending_reqs));
        if (break_on_idle && idle) {
            break;
        }
    }
    return idle;
}

static void store_mem_event(MemEvent *ev)
{
    uint64_t *k = g_new(uint64_t, 1);
    *k = key_for(ev->seq, ev->mem_idx, ev->kind);
    g_hash_table_replace(g_mem_events, k, ev);
}

static void vcpu_mem_cb(unsigned int cpu_index, qemu_plugin_meminfo_t info, uint64_t vaddr, void *udata)
{
    (void)cpu_index;
    (void)udata;

    if (g_fatal || g_active_seq == UINT64_MAX) {
        return;
    }

    MemEvent *ev = g_new0(MemEvent, 1);
    ev->seq = g_active_seq;
    ev->mem_idx = g_active_mem_idx++;
    ev->kind = qemu_plugin_mem_is_store(info) ? 1u : 0u;
    ev->addr = vaddr;
    ev->size = (uint8_t)(1u << qemu_plugin_mem_size_shift(info));
    ev->value = mem_value_to_u64(qemu_plugin_mem_get_value(info));
    store_mem_event(ev);

    if (g_verbose) {
        fprintf(stderr,
                "trace_manager: mem seq=%" PRIu64 " idx=%u kind=%u addr=0x%" PRIx64 " val=0x%" PRIx64 "\n",
                ev->seq,
                (unsigned)ev->mem_idx,
                (unsigned)ev->kind,
                ev->addr,
                ev->value);
    }

    resolve_pending_reqs();
    (void)pump_runtime(1, false);
}

static void vcpu_insn_exec_cb(unsigned int cpu_index, void *udata)
{
    (void)cpu_index;
    if (g_fatal || !g_oex) {
        return;
    }

    const InsnMeta *meta = (const InsnMeta *)udata;
    g_active_seq = g_next_seq++;
    g_active_mem_idx = 0;

    oex_inst_event_t ev = {
        .seq = g_active_seq,
        .pc = meta->pc,
        .raw = meta->raw,
        .len = meta->len,
    };
    if (g_oex_push_inst(g_oex, &ev) != 0) {
        char err[256];
        runtime_last_error(err, sizeof(err));
        if (err[0] != '\0') {
            fprintf(stderr, "trace_manager: oex_push_inst failed: %s\n", err);
        }
        set_fatal("oex_push_inst failed");
        return;
    }

    g_insn_since_pump += 1;
    if (g_insn_since_pump >= g_pump_insn_quantum) {
        const uint32_t budget = g_max_oex_cycles_per_insn * g_insn_since_pump;
        (void)pump_runtime(budget, false);
        g_insn_since_pump = 0;
    }
}

static void vcpu_tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    (void)id;

    const size_t n_insns = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n_insns; ++i) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        InsnMeta *meta = g_new0(InsnMeta, 1);
        uint8_t bytes[8] = {0};
        size_t copied = qemu_plugin_insn_data(insn, bytes, sizeof(bytes));

        meta->pc = qemu_plugin_insn_vaddr(insn);
        meta->len = (uint8_t)qemu_plugin_insn_size(insn);
        meta->raw = 0;
        for (size_t b = 0; b < copied; ++b) {
            meta->raw |= ((uint64_t)bytes[b]) << (8u * b);
        }

        g_insn_metas = g_slist_prepend(g_insn_metas, meta);

        qemu_plugin_register_vcpu_insn_exec_cb(
            insn, vcpu_insn_exec_cb, QEMU_PLUGIN_CB_NO_REGS, (void *)meta);
        qemu_plugin_register_vcpu_mem_cb(
            insn, vcpu_mem_cb, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW, NULL);
    }
}

static void unload_runtime(void)
{
    if (g_oex && g_oex_destroy) {
        g_oex_destroy(g_oex);
        g_oex = NULL;
    }
    if (g_oex_so) {
        dlclose(g_oex_so);
        g_oex_so = NULL;
    }
    g_oex_create = NULL;
    g_oex_destroy = NULL;
    g_oex_push_inst = NULL;
    g_oex_tick = NULL;
    g_oex_take_mem_req = NULL;
    g_oex_push_mem_rsp = NULL;
    g_oex_pop_retire = NULL;
    g_oex_pop_trace_occ = NULL;
    g_oex_last_error = NULL;
}

static bool load_runtime(const char *lib_path, const char *profile, const char *cfg)
{
    g_oex_so = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_oex_so) {
        fprintf(stderr, "trace_manager: dlopen failed: %s\n", dlerror());
        return false;
    }

#define LOAD_SYM(name, type)                                                               \
    do {                                                                                   \
        g_##name = (type)dlsym(g_oex_so, #name);                                           \
        if (!g_##name) {                                                                    \
            fprintf(stderr, "trace_manager: missing symbol %s\n", #name);                \
            unload_runtime();                                                               \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

    LOAD_SYM(oex_create, oex_create_fn);
    LOAD_SYM(oex_destroy, oex_destroy_fn);
    LOAD_SYM(oex_push_inst, oex_push_inst_fn);
    LOAD_SYM(oex_tick, oex_tick_fn);
    LOAD_SYM(oex_take_mem_req, oex_take_mem_req_fn);
    LOAD_SYM(oex_push_mem_rsp, oex_push_mem_rsp_fn);
    LOAD_SYM(oex_pop_retire, oex_pop_retire_fn);
    LOAD_SYM(oex_last_error, oex_last_error_fn);
    g_oex_pop_trace_occ = (oex_pop_trace_occ_fn)dlsym(g_oex_so, "oex_pop_trace_occ");

#undef LOAD_SYM

    g_oex = g_oex_create(profile, cfg ? cfg : "");
    if (!g_oex) {
        fprintf(stderr, "trace_manager: oex_create failed (profile=%s)\n", profile);
        unload_runtime();
        return false;
    }
    return true;
}

static void plugin_cleanup(qemu_plugin_id_t id)
{
    (void)id;
    g_slist_free_full(g_insn_metas, g_free);
    g_insn_metas = NULL;

    if (g_pending_reqs) {
        while (!g_queue_is_empty(g_pending_reqs)) {
            PendingReq *pr = (PendingReq *)g_queue_pop_head(g_pending_reqs);
            g_free(pr);
        }
        g_queue_free(g_pending_reqs);
        g_pending_reqs = NULL;
    }

    if (g_mem_events) {
        g_hash_table_destroy(g_mem_events);
        g_mem_events = NULL;
    }

    unload_runtime();

    if (g_retire_fp) {
        fclose(g_retire_fp);
        g_retire_fp = NULL;
    }
    g_free(g_retire_path);
    g_retire_path = NULL;
    if (g_occ_fp) {
        fclose(g_occ_fp);
        g_occ_fp = NULL;
    }
    g_free(g_occ_path);
    g_occ_path = NULL;
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    (void)p;

    g_active_seq = UINT64_MAX;
    if (!g_fatal && g_insn_since_pump > 0) {
        const uint32_t budget = g_max_oex_cycles_per_insn * g_insn_since_pump;
        (void)pump_runtime(budget, false);
        g_insn_since_pump = 0;
    }
    for (int i = 0; i < 16384 && !g_fatal; ++i) {
        bool idle = pump_runtime(g_max_oex_cycles_per_insn * 4, true);
        if (idle && g_queue_is_empty(g_pending_reqs)) {
            break;
        }
    }

    if (g_failfast && g_pending_reqs && !g_queue_is_empty(g_pending_reqs)) {
        set_fatal("unresolved mem requests at exit");
    }

    if (g_fatal) {
        qemu_plugin_outs("trace_manager: failed\n");
    } else {
        qemu_plugin_outs("trace_manager: ok\n");
    }

    plugin_cleanup(id);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    (void)info;

    g_plugin_id = id;
    g_fatal = false;
    g_next_seq = 0;
    g_active_seq = UINT64_MAX;
    g_active_mem_idx = 0;
    g_insn_since_pump = 0;

    g_autofree char *oex_lib = NULL;
    g_autofree char *oex_profile = g_strdup("oex_target");
    g_autofree char *oex_cfg = g_strdup("");
    g_autofree char *retire_out = g_strdup("/tmp/oex_retire.jsonl");
    g_autofree char *occ_out = g_strdup("");

    for (int i = 0; i < argc; ++i) {
        char *p = argv[i];
        g_auto(GStrv) tokens = g_strsplit(p, "=", 2);
        if (!tokens[0]) {
            continue;
        }

        if (g_strcmp0(tokens[0], "oex_lib") == 0) {
            oex_lib = g_strdup(tokens[1]);
        } else if (g_strcmp0(tokens[0], "oex_profile") == 0) {
            oex_profile = g_strdup(tokens[1]);
        } else if (g_strcmp0(tokens[0], "oex_cfg") == 0) {
            oex_cfg = g_strdup(tokens[1]);
        } else if (g_strcmp0(tokens[0], "retire_out") == 0) {
            retire_out = g_strdup(tokens[1]);
        } else if (g_strcmp0(tokens[0], "occ_out") == 0) {
            occ_out = g_strdup(tokens[1]);
        } else if (g_strcmp0(tokens[0], "max_oex_cycles_per_insn") == 0) {
            char *end = NULL;
            guint64 v = g_ascii_strtoull(tokens[1], &end, 0);
            if (!end || *end != '\0' || v == 0) {
                fprintf(stderr, "trace_manager: invalid max_oex_cycles_per_insn: %s\n", tokens[1]);
                return -1;
            }
            g_max_oex_cycles_per_insn = (uint32_t)v;
        } else if (g_strcmp0(tokens[0], "pump_insn_quantum") == 0) {
            char *end = NULL;
            guint64 v = g_ascii_strtoull(tokens[1], &end, 0);
            if (!end || *end != '\0' || v == 0) {
                fprintf(stderr, "trace_manager: invalid pump_insn_quantum: %s\n", tokens[1]);
                return -1;
            }
            g_pump_insn_quantum = (uint32_t)v;
        } else if (g_strcmp0(tokens[0], "failfast") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &g_failfast)) {
                fprintf(stderr, "trace_manager: invalid bool option %s\n", p);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "verbose") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &g_verbose)) {
                fprintf(stderr, "trace_manager: invalid bool option %s\n", p);
                return -1;
            }
        } else {
            fprintf(stderr, "trace_manager: unknown option: %s\n", p);
            return -1;
        }
    }

    if (!oex_lib) {
        fprintf(stderr, "trace_manager: missing required option oex_lib=<path>\n");
        return -1;
    }

    g_retire_fp = fopen(retire_out, "w");
    if (!g_retire_fp) {
        fprintf(stderr, "trace_manager: failed to open retire_out: %s\n", retire_out);
        return -1;
    }
    g_retire_path = g_strdup(retire_out);

    if (occ_out && occ_out[0] != '\0') {
        g_occ_fp = fopen(occ_out, "w");
        if (!g_occ_fp) {
            fprintf(stderr, "trace_manager: failed to open occ_out: %s\n", occ_out);
            plugin_cleanup(id);
            return -1;
        }
        g_occ_path = g_strdup(occ_out);
    }

    g_mem_events = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    g_pending_reqs = g_queue_new();

    if (!load_runtime(oex_lib, oex_profile, oex_cfg)) {
        plugin_cleanup(id);
        return -1;
    }
    if (g_occ_fp && !g_oex_pop_trace_occ) {
        fprintf(stderr, "trace_manager: runtime missing oex_pop_trace_occ but occ_out requested\n");
        plugin_cleanup(id);
        return -1;
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans_cb);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
