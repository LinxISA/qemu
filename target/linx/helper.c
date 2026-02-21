/*
 * LinxISA helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "opcode_meta.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/accel-cpu-ops.h"
#include "fpu/softfloat-helpers.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "exec/memopidx.h"
#include "accel/tcg/cpu-ldst-common.h"
#include "exec/cputlb.h"
#include "exec/target_page.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include <inttypes.h>
#include <math.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <unistd.h>

/* Optional compatibility addend configured from $LINX_CALLFRAME_SIZE. */
extern uint64_t linx_callframe_size;

static bool linx_trace_mmu_inited;
static bool linx_trace_mmu_enabled;

static inline bool linx_trace_mmu(void)
{
    if (!linx_trace_mmu_inited) {
        const char *v = getenv("LINX_TRACE_MMU");
        linx_trace_mmu_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_trace_mmu_inited = true;
    }
    return linx_trace_mmu_enabled;
}

static bool linx_trace_ra_inited;
static bool linx_trace_ra_enabled;
static bool linx_trace_ra_pc_filter_enabled;
static uint64_t linx_trace_ra_pc;
static bool linx_print_insn_count_inited;
static bool linx_print_insn_count_enabled;
static bool linx_tile_debug_inited;
static bool linx_tile_debug_enabled;
static bool linx_trace_vbrg_inited;
static bool linx_trace_vbrg_enabled;
static uint64_t linx_trace_vbrg_max_bodies;
static uint64_t linx_trace_vbrg_body_count;

static inline bool linx_trace_vbrg(void)
{
    if (!linx_trace_vbrg_inited) {
        const char *v = getenv("LINX_TRACE_VBRG");
        linx_trace_vbrg_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_trace_vbrg_max_bodies = 0;

        if (linx_trace_vbrg_enabled) {
            /* Default: only trace the first vblock body to keep output small. */
            linx_trace_vbrg_max_bodies = 1;
            if (strcmp(v, "all") == 0 || strcmp(v, "ALL") == 0) {
                linx_trace_vbrg_max_bodies = UINT64_MAX;
            } else {
                char *endp = NULL;
                errno = 0;
                uint64_t parsed = strtoull(v, &endp, 0);
                if (errno == 0 && endp && endp != v && *endp == '\0' && parsed > 0) {
                    linx_trace_vbrg_max_bodies = parsed;
                }
            }
        }

        linx_trace_vbrg_inited = true;
    }
    return linx_trace_vbrg_enabled;
}

static inline bool linx_trace_ra_match(uint64_t pc)
{
    if (!linx_trace_ra_inited) {
        const char *v = getenv("LINX_TRACE_RA");
        linx_trace_ra_enabled = v && v[0] && strcmp(v, "0") != 0;

        const char *pc_s = getenv("LINX_TRACE_RA_PC");
        if (pc_s && pc_s[0] && strcmp(pc_s, "0") != 0) {
            char *endp = NULL;
            errno = 0;
            uint64_t parsed = strtoull(pc_s, &endp, 0);
            if (errno == 0 && endp && endp != pc_s && *endp == '\0') {
                linx_trace_ra_pc = parsed;
                linx_trace_ra_pc_filter_enabled = true;
            }
        }

        linx_trace_ra_inited = true;
    }

    if (!linx_trace_ra_enabled) {
        return false;
    }
    return !linx_trace_ra_pc_filter_enabled || pc == linx_trace_ra_pc;
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

static inline bool linx_tile_debug(void)
{
    if (!linx_tile_debug_inited) {
        const char *v = getenv("LINX_TILE_DEBUG");
        linx_tile_debug_enabled = v && v[0] && strcmp(v, "0") != 0;
        linx_tile_debug_inited = true;
    }
    return linx_tile_debug_enabled;
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

/* ECSTATE bits (v0.2 bring-up profile; mirrors key CSTATE fields). */
#define LINX_ECSTATE_BI_BIT        (1ULL << 62)

/* TRAPNO encoding (v0.2 bring-up profile; keep in sync with target/linx/cpu.c). */
#define LINX_TRAPNO_E_BIT          (1ULL << 63) /* 1=async interrupt */
#define LINX_TRAPNO_ARGV_BIT       (1ULL << 62)
#define LINX_TRAPNO_CAUSE_SHIFT    24u
#define LINX_TRAPNO_CAUSE_MASK     0xFFFFFFu
#define LINX_TRAPNO_TRAPNUM_MASK   0x3Fu

static inline uint64_t linx_trapno_make(bool async, bool argv, uint32_t cause, uint8_t trapnum)
{
    const uint64_t e = async ? LINX_TRAPNO_E_BIT : 0;
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

#if !defined(_WIN32)

static inline bool linx_env_enabled(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
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
        return;
    }

    if (strlen(socket_s) >= sizeof(env->cosim.socket_path) ||
        strlen(snapshot_s) >= sizeof(env->cosim.snapshot_path)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx cosim: socket or snapshot path too long\n");
        env->cosim.enabled = 0;
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
            return;
        }
    }
    env->cosim.max_commits = max_commits;
    env->cosim.enabled = 1;
}

void HELPER(linx_cosim_before_insn)(CPULinxState *env, uint64_t pc)
{
    char start_line[1024];

    linx_cosim_init(env);
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
}

#else /* _WIN32 */

/* Windows build: co-sim currently relies on UNIX domain sockets; disable it. */
static void linx_cosim_init(CPULinxState *env)
{
    if (env->cosim.inited) {
        return;
    }
    memset(&env->cosim, 0, sizeof(env->cosim));
    env->cosim.sock_fd = -1;
    env->cosim.inited = 1;
    env->cosim.enabled = 0;
}

static void linx_cosim_finish(CPULinxState *env)
{
    (void)env;
}

static bool linx_cosim_send_end(CPULinxState *env, const char *reason)
{
    (void)env;
    (void)reason;
    return false;
}

void HELPER(linx_cosim_before_insn)(CPULinxState *env, uint64_t pc)
{
    (void)pc;
    linx_cosim_init(env);
}

#endif /* !_WIN32 */

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

static inline bool linx_commit_trace_active(CPULinxState *env)
{
    linx_commit_trace_init(env);
    return env->commit_trace.enabled && env->commit_trace.fp;
}

static inline bool linx_trace_capture_active(CPULinxState *env)
{
    linx_cosim_init(env);
    return linx_commit_trace_active(env) || env->cosim.active;
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

/*
 * Trace schema block-kind labels:
 *   scalar|vpar|vseq|tma|cube|tepl|call|ret|sys
 *
 * Keep these values aligned with docs/bringup/contracts/trace_schema.md.
 */
enum {
    LINX_TRACE_BR_CALL  = 4,
    LINX_TRACE_BR_ICALL = 6,
    LINX_TRACE_BR_RET   = 7,
};

enum {
    LINX_TRACE_BLOCK_MPAR = 0,
    LINX_TRACE_BLOCK_MSEQ = 1,
    LINX_TRACE_BLOCK_TMA  = 2,
    LINX_TRACE_BLOCK_VPAR = 4,
    LINX_TRACE_BLOCK_VSEQ = 5,
    LINX_TRACE_BLOCK_CUBE = 6,
    LINX_TRACE_BLOCK_TEPL = 7,
};

static const char *linx_trace_block_kind(CPULinxState *env, uint64_t insn_raw, uint32_t len)
{
    const uint32_t blocktype = env->blocktype & 0x1fu;
    const uint32_t brtype = env->brtype & 0xffu;
    const uint32_t len_meta = linx_trace_len_to_meta_len(len);
    const LinxOpcodeMeta *meta = linx_opcode_meta_lookup(insn_raw, len_meta);
    const uint16_t hw = (uint16_t)(insn_raw & 0xffffu);

    /*
     * SYS/FPSYS instructions must retain "sys" classification even when they
     * execute inside vector/tile block contexts.
     */
    if (!meta) {
        meta = linx_opcode_meta_lookup(insn_raw, 0);
    }
    if (meta && meta->major_cat == LINX_CAT_FP_SYS) {
        return "sys";
    }

    switch (blocktype) {
    case LINX_TRACE_BLOCK_TMA:
        return "tma";
    case LINX_TRACE_BLOCK_CUBE:
        return "cube";
    case LINX_TRACE_BLOCK_TEPL:
        return "tepl";
    case LINX_TRACE_BLOCK_VPAR:
        return "vpar";
    case LINX_TRACE_BLOCK_VSEQ:
        return "vseq";
    case LINX_TRACE_BLOCK_MPAR:
        if (env->in_body) {
            return "vpar";
        }
        break;
    case LINX_TRACE_BLOCK_MSEQ:
        if (env->in_body) {
            return "vseq";
        }
        break;
    default:
        break;
    }

    /*
     * Compressed fixed-form block headers may flow through generic C.BSTART
     * handling; classify directly from the instruction bits.
     */
    if (len == 2) {
        switch (hw) {
        case 0x88c0:
            return "vpar";
        case 0xc8c0:
            return "vseq";
        case 0x0840:
            return "sys";
        default:
            break;
        }
    }

    if (brtype == LINX_TRACE_BR_CALL || brtype == LINX_TRACE_BR_ICALL) {
        return "call";
    }
    if (brtype == LINX_TRACE_BR_RET) {
        return "ret";
    }

    return "scalar";
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

static inline void linx_template_commit_and_exit(CPULinxState *env,
                                                 CPUState *cs,
                                                 uint64_t next_pc)
{
    if (linx_trace_capture_active(env)) {
        HELPER(linx_commit_trace)(env, next_pc);
    }
    cpu_loop_exit_noexc(cs);
}

#if !defined(_WIN32)
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
#else /* _WIN32 */
static void linx_cosim_send_commit_and_wait_ack(CPULinxState *env, uint64_t next_pc)
{
    (void)env;
    (void)next_pc;
}
#endif /* !_WIN32 */

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
        const char *block_kind = linx_trace_block_kind(env, env->trace_insn, env->trace_len);
        const bool vector_block = strcmp(block_kind, "vpar") == 0 || strcmp(block_kind, "vseq") == 0;
        const bool tile_block = strcmp(block_kind, "tma") == 0 ||
                                strcmp(block_kind, "cube") == 0 ||
                                strcmp(block_kind, "tepl") == 0;
        const int32_t lane_id = vector_block ? 0 : -1;
        const uint32_t tile_ref_src = tile_block ? (env->tile_iot_src0 & 0x1f) : 0u;
        const uint32_t tile_ref_dst = tile_block ? (env->tile_iot_src1 & 0x1f) : 0u;
        char tile_meta[96];
        const uint8_t trapnum = (uint8_t)(trap_cause & 0xffu);
        const uint32_t cause = (uint32_t)((trap_cause >> 8) & 0xffu);
        const bool argv = trap_valid != 0; /* commit-trace: treat TRAPARG0 as present when trap_valid */
        const uint64_t trapno_full = trap_valid ? linx_trapno_make(false, argv, cause, trapnum) : 0;

        if (tile_block) {
            snprintf(tile_meta, sizeof(tile_meta),
                     "func=%u,dtype=%u,iot_valid=%u,iot_count=%u",
                     env->tile_func & 0x3ffu,
                     env->tile_dtype & 0x1fu,
                     env->tile_iot_valid ? 1u : 0u,
                     env->tile_iot_count);
        } else {
            tile_meta[0] = '\0';
        }

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
                ",\"tile_meta\":\"%s\",\"tile_ref_src\":%u,\"tile_ref_dst\":%u"
                ",\"trapno_full\":%" PRIu64 ",\"traparg0\":%" PRIu64
                ",\"next_pc\":%" PRIu64 "}\n",
                cycle,
                pc,
                env->trace_insn,
                env->trace_len,
                env->trace_wb_valid, env->trace_wb_rd, env->trace_wb_data,
                env->trace_src0_valid, env->trace_src0_reg, env->trace_src0_data,
                env->trace_src1_valid, env->trace_src1_reg, env->trace_src1_data,
                dst_valid, dst_reg, dst_data,
                env->trace_mem_valid, env->trace_mem_is_store, env->trace_mem_addr,
                env->trace_mem_wdata, env->trace_mem_rdata, env->trace_mem_size,
                trap_valid, trap_cause,
                block_kind, lane_id,
                tile_meta, tile_ref_src, tile_ref_dst,
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

    if (env->cosim.active) {
        linx_cosim_send_commit_and_wait_ack(env, next_pc);
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

static inline void linx_raise_illegal_inst(CPULinxState *env)
{
    env->pending_trap_arg0 = 0;
    env->pending_trap_cause = 0;
    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
}

uint64_t HELPER(linx_ssr_read)(CPULinxState *env, uint32_t ssrid)
{
    uint32_t idx = linx_ssr_low12(ssrid);
    const bool is_manager = linx_ssr_is_manager_idx(idx);
    const uint32_t bank = is_manager ? ((ssrid >> 12) & 0xFu) : 0u;

    switch (idx) {
    case LINX_SSR_CYCLE:
        /* Bring-up: model CYCLE as the dynamic instruction counter. */
        return env->insn_count;
    case LINX_SSR_TIME:
        /* Virtual time in nanoseconds. */
        return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    default:
        if (is_manager) {
            /* v0.2: legacy trap-save SSRs are illegal. */
            if (idx == 0xF0B || idx == 0xF0C || idx == 0xF0D || idx == 0xF0E) {
                env->pending_trap_arg0 = 0;
                env->pending_trap_cause = 0;
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return 0;
            }
            if (idx == LINX_SSR_TIMER_TIME) {
                return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
            if (idx == LINX_SSR_DBGID) {
                const uint64_t cps_minus1 = 0; /* CPs=1 */
                const uint64_t bps_minus1 = 3; /* BPs=4 */
                const uint64_t wps_minus1 = 3; /* WPs=4 */
                return (cps_minus1 << 0) | (bps_minus1 << 4) | (wps_minus1 << 8);
            }
            if (bank < LINX_ACR_COUNT) {
                return env->ssr_acr[bank][idx];
            }
            return 0;
        }
        return env->ssr[idx];
    }
}

void HELPER(linx_ssr_write)(CPULinxState *env, uint32_t ssrid, uint64_t value)
{
    uint32_t idx = linx_ssr_low12(ssrid);
    const bool is_manager = linx_ssr_is_manager_idx(idx);
    const uint32_t bank = is_manager ? ((ssrid >> 12) & 0xFu) : 0u;

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
        linx_irq_kick_if_allowed(env, 1);
        return;
    default:
        if (is_manager) {
            if (bank >= LINX_ACR_COUNT) {
                return;
            }

            /* v0.2: legacy trap-save SSRs are illegal. */
            if (idx == 0xF0B || idx == 0xF0C || idx == 0xF0D || idx == 0xF0E) {
                env->pending_trap_arg0 = 0;
                env->pending_trap_cause = 0;
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (idx == LINX_SSR_DBGID) {
                env->pending_trap_arg0 = 0;
                env->pending_trap_cause = 0;
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }

            if (linx_trace_mmu()) {
                switch (idx) {
                case LINX_SSR_TTBR0:
                case LINX_SSR_TTBR1:
                case LINX_SSR_TCR:
                case LINX_SSR_MAIR:
                case LINX_SSR_IOTTBR:
                case LINX_SSR_IOTCR:
                case LINX_SSR_IOMAIR: {
                    const char *name =
                        (idx == LINX_SSR_TTBR0) ? "TTBR0" :
                        (idx == LINX_SSR_TTBR1) ? "TTBR1" :
                        (idx == LINX_SSR_TCR) ? "TCR" :
                        (idx == LINX_SSR_MAIR) ? "MAIR" :
                        (idx == LINX_SSR_IOTTBR) ? "IOTTBR" :
                        (idx == LINX_SSR_IOTCR) ? "IOTCR" :
                        "IOMAIR";
                    fprintf(stderr,
                            "linx: ssr_write %-6s ssrid=0x%06" PRIx32 " bank=%u idx=0x%03" PRIx32
                            " val=0x%016" PRIx64 "\n",
                            name, ssrid, bank, idx, value);
                    fflush(stderr);
                    break;
                }
                default:
                    break;
                }
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
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    tlb_flush(env_cpu(env));
                    return;
                }
                if (idx == LINX_SSR_IOTCR) {
                    const uint64_t allowed = (1ull << 0) | (0x3full << 1);
                    if ((value & ~allowed) != 0) {
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    return;
                }
                if (idx == LINX_SSR_TTBR0 || idx == LINX_SSR_TTBR1 || idx == LINX_SSR_IOTTBR) {
                    if ((value & 0xfffu) != 0) {
                        CPUState *cs = env_cpu(env);
                        cs->exception_index = LINX_EXCP_ILLEGAL_INST;
                        cpu_loop_exit(cs);
                    }
                    env->ssr_acr[bank][idx] = value;
                    tlb_flush(env_cpu(env));
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

                env->ssr_acr[bank][LINX_SSR_IPENDING] &= ~bit;
                if (env->irq_level_acr[bank] & bit) {
                    env->ssr_acr[bank][LINX_SSR_IPENDING] |= bit;
                }

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
                env->ssr_acr[bank][idx] = value;

                if (bank == 1 && env->timer) {
                    CPUState *cs = env_cpu(env);
                    if (value == 0) {
                        timer_del(env->timer);
                        env->ssr_acr[1][LINX_SSR_IPENDING] &= ~(1ull << 0);
                        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
                        return;
                    }

                    const uint64_t now = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                    if (value <= now) {
                        env->ssr_acr[1][LINX_SSR_IPENDING] |= (1ull << 0);
                        linx_irq_kick_if_allowed(env, 1);
                        return;
                    }
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

            env->ssr_acr[bank][idx] = value;
            return;
        }
        env->ssr[idx] = value;
        return;
    }
}

uint64_t HELPER(linx_ssr_swap)(CPULinxState *env, uint32_t ssrid, uint64_t value)
{
    uint64_t old = HELPER(linx_ssr_read)(env, ssrid);
    HELPER(linx_ssr_write)(env, ssrid, value);
    return old;
}

void HELPER(linx_tlb_iall)(CPULinxState *env)
{
    tlb_flush(env_cpu(env));
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

static inline void linx_dbg_check_mem(CPULinxState *env, uint64_t addr, uint32_t size,
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
        cs->exception_index = LINX_EXCP_HW_WATCHPOINT;
        cpu_loop_exit_restore(cs, GETPC());
    }
}

static bool linx_dbg_trace_load_pc_inited;
static bool linx_dbg_trace_load_pc_enabled;
static uint64_t linx_dbg_trace_load_pc_lo;
static uint64_t linx_dbg_trace_load_pc_hi;

static inline void linx_dbg_trace_load_pc_init(void)
{
    if (linx_dbg_trace_load_pc_inited) {
        return;
    }

    linx_dbg_trace_load_pc_inited = true;
    const char *spec = getenv("LINX_TRACE_DBG_LOAD_PC");
    if (!spec || !spec[0] || strcmp(spec, "0") == 0) {
        return;
    }

    errno = 0;
    char *endp = NULL;
    uint64_t lo = strtoull(spec, &endp, 0);
    if (errno != 0 || !endp || endp == spec) {
        return;
    }

    uint64_t hi = lo;
    if (*endp == ':') {
        const char *rhs = endp + 1;
        if (!*rhs) {
            return;
        }
        errno = 0;
        char *endp2 = NULL;
        hi = strtoull(rhs, &endp2, 0);
        if (errno != 0 || !endp2 || endp2 == rhs || *endp2 != '\0') {
            return;
        }
        if (hi < lo) {
            uint64_t tmp = lo;
            lo = hi;
            hi = tmp;
        }
    } else if (*endp != '\0') {
        return;
    }

    linx_dbg_trace_load_pc_enabled = true;
    linx_dbg_trace_load_pc_lo = lo;
    linx_dbg_trace_load_pc_hi = hi;
}

void HELPER(linx_dbg_check_load)(CPULinxState *env, uint64_t pc, uint64_t addr, uint32_t size)
{
    linx_dbg_trace_load_pc_init();
    if (linx_dbg_trace_load_pc_enabled &&
        pc >= linx_dbg_trace_load_pc_lo &&
        pc <= linx_dbg_trace_load_pc_hi) {
        fprintf(stderr,
                "Linx: DBG_LOAD pc=0x%016" PRIx64 " addr=0x%016" PRIx64
                " size=%u acr=%u tq=[0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 "]"
                " uq=[0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 "]"
                " bpc=0x%016" PRIx64 " tgt=0x%016" PRIx64 " cond=%u carg=0x%08x"
                " sp=0x%016" PRIx64
                " a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64
                " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 " a6=0x%016" PRIx64 " a7=0x%016" PRIx64
                " s0=0x%016" PRIx64 " s1=0x%016" PRIx64 " s2=0x%016" PRIx64 " s3=0x%016" PRIx64
                " s4=0x%016" PRIx64 " s5=0x%016" PRIx64 " s6=0x%016" PRIx64 " s7=0x%016" PRIx64
                " s8=0x%016" PRIx64 "\n",
                pc, addr, size, env->acr & 0xfu,
                env->tq[0], env->tq[1], env->tq[2], env->tq[3],
                env->uq[0], env->uq[1], env->uq[2], env->uq[3],
                env->bpc, env->tgt, env->cond, env->carg,
                env->gpr[LINX_REG_SP],
                env->gpr[LINX_REG_A0], env->gpr[LINX_REG_A1], env->gpr[LINX_REG_A2], env->gpr[LINX_REG_A3],
                env->gpr[LINX_REG_A4], env->gpr[LINX_REG_A5], env->gpr[LINX_REG_A6], env->gpr[LINX_REG_A7],
                env->gpr[11], env->gpr[12], env->gpr[13], env->gpr[14],
                env->gpr[15], env->gpr[16], env->gpr[17], env->gpr[18],
                env->gpr[19]);
        fflush(stderr);
    }
    linx_dbg_check_mem(env, addr, size, false);
}

void HELPER(linx_dbg_check_store)(CPULinxState *env, uint64_t pc, uint64_t addr, uint32_t size)
{
    (void)pc;
    linx_dbg_check_mem(env, addr, size, true);
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
        linx_trapno_make(false, true, (uint32_t)request_type, 6 /* SYSCALL */);
    env->ssr_acr[dst_acr][LINX_SSR_TRAPARG0] = (uint64_t)request_type;

    /* Disable interrupts and switch to managing ring, then vector to EVBASE. */
    env->ssr[LINX_SSR_CSTATE] &= ~LINX_CSTATE_I_BIT;
    env->acr = dst_acr;
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
    const uint32_t target = linx_cstate_get_acr(ecstate);
    const bool bi = (ecstate & LINX_ECSTATE_BI_BIT) != 0;
    const uint64_t resume_bpc = env->ssr_acr[mgr][LINX_SSR_EBARG_BPC_CUR];
    const uint64_t resume_tpc = env->ssr_acr[mgr][LINX_SSR_EBARG_TPC];
    const uint64_t resume_pc = bi ? resume_tpc : resume_bpc;

    if (getenv("LINX_TRACE_ACR_ENTER")) {
        if (mgr != target || getenv("LINX_TRACE_ACR_ENTER_VERBOSE")) {
            fprintf(stderr,
                    "Linx: ACR_ENTER mgr=%u -> target=%u rra=%u bi=%u"
                    " resume_pc=0x%016" PRIx64
                    " resume_bpc=0x%016" PRIx64
                    " resume_tpc=0x%016" PRIx64
                    " a0=0x%016" PRIx64
                    " ecstate=0x%016" PRIx64 "\n",
                    mgr, target, rra_type, bi ? 1u : 0u,
                    resume_pc, resume_bpc, resume_tpc,
                    env->gpr[LINX_REG_A0], ecstate);
            fflush(stderr);
        }
    }

    /*
     * v0.2 bring-up: ACR_ENTER may keep privilege or drop privilege.
     * Entering a more-privileged ring directly from software is invalid.
     */
    if (target >= LINX_ACR_COUNT || target < mgr) {
        env->pending_trap_arg0 = (uint64_t)target;
        env->pending_trap_cause = 0;
        helper_raise_exception(env, LINX_EXCP_EXEC_STATE_CHECK);
        cpu_loop_exit(cs);
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
        env->body_tpc = 0;
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
        const uint64_t lb = env->ssr_acr[mgr][LINX_SSR_EBARG_LB];
        const uint64_t lc = env->ssr_acr[mgr][LINX_SSR_EBARG_LC];
        env->tq[0] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ0];
        env->tq[1] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ1];
        env->tq[2] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ2];
        env->tq[3] = env->ssr_acr[mgr][LINX_SSR_EBARG_TQ3];
        env->uq[0] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ0];
        env->uq[1] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ1];
        env->uq[2] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ2];
        env->uq[3] = env->ssr_acr[mgr][LINX_SSR_EBARG_UQ3];
        env->lb[0] = (lb >> 0) & 0xffffu;
        env->lb[1] = (lb >> 16) & 0xffffu;
        env->lb[2] = (lb >> 32) & 0xffffu;
        env->lc[0] = (lc >> 0) & 0xffffu;
        env->lc[1] = (lc >> 16) & 0xffffu;
        env->lc[2] = (lc >> 32) & 0xffffu;
    } else {
        env->pending_trap_arg0 = (uint64_t)rra_type;
        env->pending_trap_cause = 0;
        helper_raise_exception(env, LINX_EXCP_EXEC_STATE_CHECK);
        cpu_loop_exit(cs);
    }

    /* v0.2: always restore BPC from EBARG. */
    env->bpc = resume_bpc;

    env->acr = target;
    env->ssr[LINX_SSR_CSTATE] = ecstate & ~LINX_ECSTATE_BI_BIT;
    env->pc = resume_pc;
    /*
     * External IRQs route to ACR1 in the bring-up profile.
     * Re-latch a pending request after privilege/state restore.
     */
    linx_irq_kick_if_allowed(env, 1);

    cs->exception_index = -1;
    cpu_loop_exit(cs);
}

/* ------------------------------------------------------------------------- */
/* Atomics (LR/SC + fetch-RMW)                                               */
/* ------------------------------------------------------------------------- */

static inline MemOpIdx linx_oi_le(MemOp mop)
{
    /* Linx uses a single MMU index (0) and little-endian. */
    return make_memop_idx(mop | MO_LE, 0);
}

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

uint64_t HELPER(linx_ld_add)(CPULinxState *env, uint64_t addr, uint64_t value)
{
    linx_lr_clear(env);
    return cpu_atomic_fetch_addq_le_mmu((CPUArchState *)env, addr, value,
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

uint64_t HELPER(linx_fcvt)(CPULinxState *env, uint64_t a, uint32_t dsttype, uint32_t srctype)
{
    return linx_fp_fcvt(env, a, dsttype, srctype);
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
    
    qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK imm=%d, a0=0x%lx, a1=0x%lx, a2=0x%lx\n",
                  imm, (unsigned long)env->gpr[LINX_REG_A0],
                  (unsigned long)env->gpr[LINX_REG_A1],
                  (unsigned long)env->gpr[LINX_REG_A2]);
    
    switch (imm) {
    case LINX_SEMIHOST_EXIT:
        /* Exit program - graceful shutdown */
        qemu_log_mask(CPU_LOG_INT, "Linx: EBREAK EXIT at PC=0x%lx\n",
                      (unsigned long)env->pc);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        cpu_loop_exit_noexc(cs);
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
        /* Unhandled semihosting operation - treat as a software breakpoint trap. */
        qemu_log_mask(LOG_GUEST_ERROR, 
                      "Linx: Unhandled EBREAK imm=%d at PC=0x%lx\n",
                      imm, (unsigned long)env->pc);
        env->pending_trap_cause = imm & 0xffu;
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

/* ------------------------------------------------------------------------- */
/* Tile block helpers (TAU bring-up)                                         */
/* ------------------------------------------------------------------------- */

enum {
    LINX_BLOCK_MPAR = 0,
    LINX_BLOCK_MSEQ = 1,
    LINX_BLOCK_TMA  = 2,
    LINX_BLOCK_VPAR = 4,
    LINX_BLOCK_VSEQ = 5,
    LINX_BLOCK_CUBE = 6,
    LINX_BLOCK_TEPL = 7,
};

enum {
    LINX_TMA_TLOAD  = 0,
    LINX_TMA_TSTORE = 1,
    LINX_TMA_TMOV = 2,
    LINX_TMA_TCVT_COMPAT = 31,
};

enum {
    LINX_CUBE_MAMULB = 0,
    LINX_CUBE_MAMULB_ACC = 2,
    LINX_CUBE_ACCCVT = 8,
};

enum {
    LINX_TEPL_TADD = 0x000u,
    LINX_TEPL_TSUB = 0x001u,
    LINX_TEPL_TMUL = 0x002u,
    LINX_TEPL_TMAX = 0x004u,
    LINX_TEPL_TCVT = 0x00fu,
    LINX_TEPL_TROWMAX = 0x020u,
    LINX_TEPL_TROWSUM = 0x022u,
    LINX_TEPL_TCOLEXPAND = 0x027u,
    LINX_TEPL_TEXP = 0x040u,
    LINX_TEPL_TRECIP = 0x044u,
    LINX_TEPL_TEXPANDS = 0x045u,
};

enum {
    LINX_IOT_S0V = 1u << 0,
    LINX_IOT_S1V = 1u << 1,
    LINX_IOT_S0R = 1u << 2,
    LINX_IOT_S1R = 1u << 3,
};

enum {
    LINX_TILE_STRICT_MIN_BYTES = 512u,
};

static inline bool linx_block_has_simt_body(uint32_t blocktype)
{
    return blocktype == LINX_BLOCK_MPAR || blocktype == LINX_BLOCK_MSEQ ||
           blocktype == LINX_BLOCK_VPAR || blocktype == LINX_BLOCK_VSEQ;
}

static inline bool linx_tile_bytes_in_strict_range(uint64_t bytes)
{
    return bytes >= LINX_TILE_STRICT_MIN_BYTES &&
           bytes <= LINX_TILE_MAX_BYTES &&
           (bytes & 3u) == 0u;
}

static bool linx_tile_size_code_to_bytes(uint32_t size_code, uint64_t *bytes_out)
{
    const uint64_t bytes = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (!linx_tile_bytes_in_strict_range(bytes)) {
        return false;
    }
    *bytes_out = bytes;
    return true;
}

static bool linx_tile_size_code_from_raw(uint64_t raw, uint32_t *size_code_out)
{
    uint32_t size_code = 0u;
    if (raw <= 31u) {
        size_code = (uint32_t)raw;
    } else {
        if (raw == 0u || (raw & (raw - 1u)) != 0u) {
            return false;
        }
        unsigned log2v = 0u;
        uint64_t t = raw;
        while (t > 1u) {
            t >>= 1;
            log2v++;
        }
        if (log2v < 4u || log2v > 35u) {
            return false;
        }
        size_code = log2v - 4u;
    }

    uint64_t bytes = 0u;
    if (!linx_tile_size_code_to_bytes(size_code, &bytes)) {
        return false;
    }
    *size_code_out = size_code;
    return true;
}

enum {
    LINX_TILE_IOT_SRC0_SHIFT = 0,
    LINX_TILE_IOT_SRC1_SHIFT = 5,
    LINX_TILE_IOT_DST_SHIFT = 10,
    LINX_TILE_IOT_GRP_SHIFT = 13,
    LINX_TILE_IOT_FLAGS_SHIFT = 14,
    LINX_TILE_IOT_REG_SHIFT = 18,
    LINX_TILE_IOT_SIZE_SHIFT = 23,
    LINX_TILE_IOT_HAS_SIZE_SHIFT = 28,
};

typedef struct LinxTileIOTDesc {
    uint32_t src0;
    uint32_t src1;
    uint32_t dst;
    uint32_t grp;
    uint32_t flags;
    uint32_t reg;
    uint32_t size;
    bool has_size;
} LinxTileIOTDesc;

static inline LinxTileIOTDesc linx_tile_decode_iot(uint64_t packed)
{
    LinxTileIOTDesc d;
    d.src0 = (packed >> LINX_TILE_IOT_SRC0_SHIFT) & 0x1fu;
    d.src1 = (packed >> LINX_TILE_IOT_SRC1_SHIFT) & 0x1fu;
    d.dst = (packed >> LINX_TILE_IOT_DST_SHIFT) & 0x7u;
    d.grp = (packed >> LINX_TILE_IOT_GRP_SHIFT) & 0x1u;
    d.flags = (packed >> LINX_TILE_IOT_FLAGS_SHIFT) & 0xfu;
    d.reg = (packed >> LINX_TILE_IOT_REG_SHIFT) & 0x1fu;
    d.size = (packed >> LINX_TILE_IOT_SIZE_SHIFT) & 0x1fu;
    d.has_size = ((packed >> LINX_TILE_IOT_HAS_SIZE_SHIFT) & 0x1u) != 0;
    return d;
}

static bool linx_tile_desc_has_explicit_size(const LinxTileIOTDesc *d)
{
    return d->has_size || d->reg != 0u;
}

static bool linx_tile_desc_get_size_code(const CPULinxState *env,
                                         const LinxTileIOTDesc *d,
                                         uint32_t *size_code_out)
{
    if (d->has_size) {
        uint64_t bytes = 0u;
        if (!linx_tile_size_code_to_bytes(d->size & 0x1fu, &bytes)) {
            return false;
        }
        *size_code_out = d->size & 0x1fu;
        return true;
    }

    if (d->reg == 0u || d->reg >= LINX_GPR_COUNT) {
        return false;
    }
    return linx_tile_size_code_from_raw(env->gpr[d->reg], size_code_out);
}

static bool linx_tile_desc_get_size_bytes(const CPULinxState *env,
                                          const LinxTileIOTDesc *d,
                                          uint64_t *bytes_out)
{
    uint32_t size_code = 0u;
    if (!linx_tile_desc_get_size_code(env, d, &size_code)) {
        return false;
    }
    return linx_tile_size_code_to_bytes(size_code, bytes_out);
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

static inline unsigned linx_tile_dtype_elem_bits(uint32_t dtype)
{
    switch (dtype & 0x1fu) {
    case 0u:  /* FP64 */
    case 16u: /* INT64 */
    case 24u: /* UINT64 */
        return 64u;
    case 1u:  /* FP32 */
    case 17u: /* INT32 */
    case 25u: /* UINT32 */
        return 32u;
    case 2u:  /* FP16 */
    case 6u:  /* BF16 */
    case 18u: /* INT16 */
    case 26u: /* UINT16 */
        return 16u;
    case 3u:  /* FP8 */
    case 7u:  /* FPL8 */
    case 19u: /* INT8 */
    case 27u: /* UINT8 */
        return 8u;
    case 11u: /* FP4 */
    case 12u: /* FPL4 */
    case 20u: /* INT4 */
    case 28u: /* UINT4 */
        return 4u;
    default:
        return 32u;
    }
}

static inline unsigned linx_tile_dtype_elem_bytes(uint32_t dtype)
{
    const unsigned bits = linx_tile_dtype_elem_bits(dtype);
    return (bits + 7u) / 8u;
}

static inline uint64_t linx_tile_pad_value(uint32_t pad_mode, uint32_t dtype,
                                           unsigned elem_bytes, uint32_t seed)
{
    uint64_t value = 0;
    const uint32_t mode = pad_mode & 0x1fu;
    const uint32_t dt = dtype & 0x1fu;

    switch (mode) {
    case 1u: /* Zero */
        value = 0u;
        break;
    case 2u: /* Max */
        if (elem_bytes == 2u) {
            value = (dt == 2u) ? 0x7bffu : ((dt == 6u) ? 0x7f7fu : 0xffffu);
        } else if (elem_bytes == 8u) {
            value = (dt == 0u) ? UINT64_C(0x7fefffffffffffff)
                               : UINT64_C(0x7fffffffffffffff);
        } else {
            value = (dt == 1u) ? 0x7f7fffffu : 0x7fffffffu;
        }
        break;
    case 3u: /* Min */
        if (elem_bytes == 2u) {
            value = (dt == 2u) ? 0xfbffu : ((dt == 6u) ? 0xff7fu : 0x8000u);
        } else if (elem_bytes == 8u) {
            value = (dt == 0u) ? UINT64_C(0xffefffffffffffff)
                               : UINT64_C(0x8000000000000000);
        } else {
            value = (dt == 1u) ? 0xff7fffffu : 0x80000000u;
        }
        break;
    case 0u: /* Null / unspecified */
    default:
        /*
         * Strict PTO parity mode uses zero-fill for unspecified padding.
         * This keeps host-sim and QEMU tile inactive lanes bit-exact.
         */
        value = 0u;
        break;
    }

    if (elem_bytes == 2u) {
        return value & 0xffffu;
    }
    if (elem_bytes == 4u) {
        return value & 0xffffffffu;
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
        case LINX_TEMPLATE_FENTRY:
        case LINX_TEMPLATE_FEXIT:
        case LINX_TEMPLATE_FRET_RA:
        case LINX_TEMPLATE_FRET_STK:
            env->tmpl_reg_begin = op0;
            env->tmpl_reg_end = op1;
            env->tmpl_reg_cur = op0;
            env->tmpl_stacksize = op2;
            break;

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
    case LINX_TEMPLATE_FENTRY: {
        const uint64_t stacksize = env->tmpl_stacksize;
        const uint64_t adj = stacksize + linx_callframe_size;
        const int begin = (int)env->tmpl_reg_begin;
        const int end = (int)env->tmpl_reg_end;
        const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
        const uint32_t step = env->tmpl_step;

        if (step == 0) {
            if (adj) {
                const uint64_t old_sp = env->gpr[LINX_REG_SP];
                env->gpr[LINX_REG_SP] -= adj;
                linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
                if (linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 2, old_sp, env->gpr[LINX_REG_SP]);
                }
            }
            env->tmpl_step = 1;

            if (stacksize == 0 || count == 0) {
                linx_template_clear(env);
                env->pc = next_pc;
            } else {
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* step >= 1: save one register per step. */
        {
            const int64_t off = (int64_t)stacksize - ((int64_t)step * 8);
            const int reg = (int)env->tmpl_reg_cur;

            if (off < 0) {
                linx_template_clear(env);
                env->pc = next_pc;
                linx_template_commit_and_exit(env, cs, env->pc);
            }

            if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t addr = env->gpr[LINX_REG_SP] + (uint64_t)off;
                const uint64_t v = env->gpr[reg];
                linx_trace_mem(env, true, addr, v, 0, 8);
                cpu_stq_le_data(env, (abi_ptr)addr, env->gpr[reg]);
                if (reg == LINX_REG_RA && linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 2, addr, v);
                }
            }

            if (reg == end) {
                linx_template_clear(env);
                env->pc = next_pc;
            } else {
                env->tmpl_reg_cur = (uint32_t)linx_next_fentry_reg(reg);
                env->tmpl_step = step + 1;
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }
        break;
    }

    case LINX_TEMPLATE_FEXIT: {
        const uint64_t stacksize = env->tmpl_stacksize;
        const uint64_t adj = stacksize + linx_callframe_size;
        const int begin = (int)env->tmpl_reg_begin;
        const int end = (int)env->tmpl_reg_end;
        const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
        const uint32_t step = env->tmpl_step;

        /* f.exit: addi sp, stack -> sp */
        if (step == 0) {
            if (adj) {
                env->gpr[LINX_REG_SP] += adj;
                linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
            }
            if (count == 0) {
                linx_template_clear(env);
                env->pc = next_pc;
            } else {
                env->tmpl_step = 1;
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* step >= 1: ldi [sp, -8 * step], -> reg */
        {
            const uint32_t load_idx = step;
            const int reg = (int)env->tmpl_reg_cur;
            const uint64_t addr = env->gpr[LINX_REG_SP] - ((uint64_t)load_idx * 8ull);

            if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t v = cpu_ldq_le_data(env, (abi_ptr)addr);
                env->gpr[reg] = v;
                linx_trace_mem(env, false, addr, 0, v, 8);
                linx_trace_wb(env, (uint32_t)reg, v);
                if (reg == LINX_REG_RA && linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 3, addr, v);
                }
            }

            if (reg == end) {
                linx_template_clear(env);
                env->pc = next_pc;
            } else {
                env->tmpl_reg_cur = (uint32_t)linx_next_fentry_reg(reg);
                env->tmpl_step = step + 1;
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }
        break;
    }

    case LINX_TEMPLATE_FRET_STK: {
        const uint64_t stacksize = env->tmpl_stacksize;
        const uint64_t adj = stacksize + linx_callframe_size;
        const int begin = (int)env->tmpl_reg_begin;
        const int end = (int)env->tmpl_reg_end;
        const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
        const uint32_t step = env->tmpl_step;

        /* f.ret.stk: addi sp, stack -> sp */
        if (step == 0) {
            if (adj) {
                env->gpr[LINX_REG_SP] += adj;
                linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
            }
            if (count == 0) {
                const uint64_t ra = env->gpr[LINX_REG_RA];
                HELPER(linx_check_bstart_target)(env, ra);
                linx_template_clear(env);
                env->pc = ra;
            } else {
                env->tmpl_step = 1;
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* Emit setc.tgt metadata immediately after restoring RA. */
        if (env->tmpl_mem_src != 0) {
            env->tmpl_mem_src = 0;
            env->pc = cur_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* step >= 1: ldi [sp, -8 * step], -> reg */
        {
            const uint32_t load_idx = step;
            const int reg = (int)env->tmpl_reg_cur;
            const uint64_t addr = env->gpr[LINX_REG_SP] - ((uint64_t)load_idx * 8ull);

            if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t v = cpu_ldq_le_data(env, (abi_ptr)addr);
                env->gpr[reg] = v;
                linx_trace_mem(env, false, addr, 0, v, 8);
                linx_trace_wb(env, (uint32_t)reg, v);
                if (reg == LINX_REG_RA && linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 3, addr, v);
                }
            }

            if (reg == end) {
                const uint64_t ra = env->gpr[LINX_REG_RA];
                HELPER(linx_check_bstart_target)(env, ra);
                linx_template_clear(env);
                env->pc = ra;
            } else {
                env->tmpl_reg_cur = (uint32_t)linx_next_fentry_reg(reg);
                env->tmpl_step = step + 1;
                if (reg == LINX_REG_RA) {
                    env->tmpl_mem_src = 1;
                }
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }
        break;
    }

    case LINX_TEMPLATE_FRET_RA: {
        const uint64_t stacksize = env->tmpl_stacksize;
        const uint64_t adj = stacksize + linx_callframe_size;
        const int begin = (int)env->tmpl_reg_begin;
        const int end = (int)env->tmpl_reg_end;
        const int count = (stacksize > 0) ? linx_fentry_reg_count(begin, end) : 0;
        const uint32_t step = env->tmpl_step;

        /* f.ret.ra step0: setc.tgt ra (metadata; no wb/mem trace fields). */
        if (step == 0) {
            env->tmpl_mem_value = env->gpr[LINX_REG_RA];
            env->tmpl_step = 1;
            env->pc = cur_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* step1: addi sp, stack -> sp */
        if (step == 1) {
            if (adj) {
                env->gpr[LINX_REG_SP] += adj;
                linx_trace_wb(env, LINX_REG_SP, env->gpr[LINX_REG_SP]);
            }
            if (count == 0) {
                const uint64_t ra = env->tmpl_mem_value;
                HELPER(linx_check_bstart_target)(env, ra);
                linx_template_clear(env);
                env->pc = ra;
            } else {
                env->tmpl_step = 2;
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }

        /* step >= 2: ldi [sp, -8 * (step - 1)], -> reg */
        {
            const uint32_t load_idx = step - 1;
            const int reg = (int)env->tmpl_reg_cur;
            const uint64_t addr = env->gpr[LINX_REG_SP] - ((uint64_t)load_idx * 8ull);

            if (reg != LINX_REG_ZERO && reg < LINX_GPR_COUNT) {
                const uint64_t v = cpu_ldq_le_data(env, (abi_ptr)addr);
                env->gpr[reg] = v;
                linx_trace_mem(env, false, addr, 0, v, 8);
                linx_trace_wb(env, (uint32_t)reg, v);
                if (reg == LINX_REG_RA && linx_trace_ra_match(cur_pc)) {
                    HELPER(linx_trace_ra)(env, cur_pc, 3, addr, v);
                }
            }

            if (reg == end) {
                const uint64_t ra = env->tmpl_mem_value;
                HELPER(linx_check_bstart_target)(env, ra);
                linx_template_clear(env);
                env->pc = ra;
            } else {
                env->tmpl_reg_cur = (uint32_t)linx_next_fentry_reg(reg);
                env->tmpl_step = step + 1;
                env->pc = cur_pc;
            }
            linx_template_commit_and_exit(env, cs, env->pc);
        }
        break;
    }

    case LINX_TEMPLATE_MCOPY: {
        uint64_t dst = env->tmpl_mem_dst;
        uint64_t src = env->tmpl_mem_src;
        uint64_t remaining = env->tmpl_mem_remaining;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
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
        } else {
            env->pc = cur_pc;
        }
        linx_template_commit_and_exit(env, cs, env->pc);
        break;
    }

    case LINX_TEMPLATE_MSET: {
        uint64_t dst = env->tmpl_mem_dst;
        uint64_t remaining = env->tmpl_mem_remaining;
        const uint8_t v = (uint8_t)env->tmpl_mem_value;

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
            linx_template_commit_and_exit(env, cs, env->pc);
        }

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

        if (remaining == 0) {
            linx_template_clear(env);
            env->pc = next_pc;
        } else {
            env->pc = cur_pc;
        }
        linx_template_commit_and_exit(env, cs, env->pc);
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
        } else {
            env->pc = cur_pc;
        }
        linx_template_commit_and_exit(env, cs, env->pc);
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

static inline uint64_t linx_tile_mem_read(CPULinxState *env, uint64_t addr,
                                          unsigned elem_bytes)
{
    hwaddr pa;
    if (!linx_iommu_translate(env, addr, false, &pa)) {
        env->pending_trap_arg0 = addr;
        env->pending_trap_cause = (uint32_t)((LINX_TRAPCAUSE_CAT_IOMMU_PF << 4) | LINX_TRAPCAUSE_ACC_LOAD);
        helper_raise_exception(env, LINX_EXCP_LOAD_ACCESS_FAULT);
    }

    MemTxResult result = MEMTX_OK;
    uint64_t v = 0;
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
    case 8u:
        v = address_space_ldq_le(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &result);
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
                                       unsigned elem_bytes, uint64_t v)
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
        address_space_stl_le(&address_space_memory, pa, v & 0xffffffffu,
                             MEMTXATTRS_UNSPECIFIED, &result);
        break;
    case 8u:
        address_space_stq_le(&address_space_memory, pa, v,
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
                                      uint32_t elem_idx, unsigned elem_bits,
                                      uint64_t value)
{
    uint8_t *buf = (uint8_t *)env->tile_reg[tile];
    if (elem_bits == 4u) {
        const uint64_t off_bits = (uint64_t)elem_idx * 4u;
        const size_t off = (size_t)(off_bits >> 3);
        if (off >= LINX_TILE_MAX_BYTES) {
            return false;
        }
        const unsigned shift = (unsigned)(off_bits & 0x7u);
        const uint8_t mask = (uint8_t)(0x0fu << shift);
        const uint8_t nibble = (uint8_t)((value & 0x0fu) << shift);
        buf[off] = (uint8_t)((buf[off] & ~mask) | nibble);
        return true;
    }
    const unsigned elem_bytes = (elem_bits + 7u) / 8u;
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
        stl_le_p(buf + off, (uint32_t)(value & 0xffffffffu));
        return true;
    case 8u:
        stq_le_p(buf + off, value);
        return true;
    default:
        return false;
    }
}

static inline bool linx_tile_get_elem(const CPULinxState *env, unsigned tile,
                                      uint32_t elem_idx, unsigned elem_bits,
                                      uint64_t *value_out)
{
    const uint8_t *buf = (const uint8_t *)env->tile_reg[tile];
    if (elem_bits == 4u) {
        const uint64_t off_bits = (uint64_t)elem_idx * 4u;
        const size_t off = (size_t)(off_bits >> 3);
        if (off >= LINX_TILE_MAX_BYTES) {
            return false;
        }
        const unsigned shift = (unsigned)(off_bits & 0x7u);
        *value_out = (uint64_t)((buf[off] >> shift) & 0x0fu);
        return true;
    }
    const unsigned elem_bytes = (elem_bits + 7u) / 8u;
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
    case 8u:
        *value_out = ldq_le_p(buf + off);
        return true;
    default:
        return false;
    }
}

static bool linx_tile_get_stride_bytes(const CPULinxState *env,
                                       uint64_t *stride_out)
{
    if (env->tile_ior_count != 0) {
        const uint64_t desc = env->tile_ior_desc[env->tile_ior_count - 1u];
        const unsigned src0 = (unsigned)((desc >> 5) & 0x1fu);
        if (src0 != 0u) {
            if (src0 >= LINX_GPR_COUNT) {
                return false;
            }
            *stride_out = env->gpr[src0];
            return true;
        }
    }

    if (env->lb[2] != 0u) {
        *stride_out = env->lb[2];
        return true;
    }

    return false;
}

static void linx_tile_load(CPULinxState *env, unsigned dst_tile, unsigned addr_reg,
                           unsigned size_code)
{
    if (dst_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (!linx_tile_bytes_in_strict_range(bytes64)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const size_t bytes = (size_t)bytes64;
    const uint32_t dtype = env->tile_dtype;
    const unsigned elem_bits = linx_tile_dtype_elem_bits(dtype);
    const unsigned elem_bytes = (elem_bits + 7u) / 8u;
    if (((bytes * 8u) % elem_bits) != 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t tile_elems = (bytes * 8u) / elem_bits;
    const uint64_t cols = env->lb[0] ? (env->lb[0] & 0xffffffffu) : tile_elems;
    const uint64_t rows = env->lb[1] ? (env->lb[1] & 0xffffffffu) : 1u;
    if (rows == 0u || cols == 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (rows > UINT64_MAX / cols) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t active_elems = rows * cols;
    if (active_elems > tile_elems) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (cols > UINT64_MAX / elem_bits) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t row_span_bits = cols * elem_bits;
    const uint64_t row_span_bytes = (row_span_bits + 7u) / 8u;
    uint64_t stride_bytes = 0u;
    if (!linx_tile_get_stride_bytes(env, &stride_bytes)) {
        stride_bytes = row_span_bytes;
    }
    if (stride_bytes < row_span_bytes ||
        (elem_bytes != 0u && (stride_bytes % elem_bytes) != 0u)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t base = env->gpr[addr_reg];
    memset(env->tile_reg[dst_tile], 0, LINX_TILE_MAX_BYTES);
    for (uint64_t r = 0; r < rows; ++r) {
        const uint64_t row_base = base + r * stride_bytes;
        for (uint64_t c = 0; c < cols; ++c) {
            const uint64_t idx = r * cols + c;
            uint64_t value = 0u;
            if (elem_bits == 4u) {
                const uint64_t byte_addr = row_base + (c >> 1u);
                const uint64_t packed = linx_tile_mem_read(env, byte_addr, 1u);
                value = ((c & 1u) == 0u) ? (packed & 0x0fu)
                                         : ((packed >> 4u) & 0x0fu);
            } else {
                value = linx_tile_mem_read(env, row_base + c * elem_bytes, elem_bytes);
            }
            if (!linx_tile_set_elem(env, dst_tile, (uint32_t)idx, elem_bits, value)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
        }
    }
    for (uint64_t idx = active_elems; idx < tile_elems; idx++) {
        const uint32_t seed = (uint32_t)idx ^ (uint32_t)base ^ (env->tile_arg_format << 8);
        const uint64_t value = linx_tile_pad_value(linx_tile_arg_pad(env->tile_arg_format),
                                                   dtype, elem_bytes, seed);
        if (!linx_tile_set_elem(env, dst_tile, (uint32_t)idx, elem_bits, value)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)bytes;
}

static void linx_tile_store(CPULinxState *env, unsigned src_tile, unsigned addr_reg,
                            unsigned size_code)
{
    if (linx_tile_debug()) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "linx_tile_store: pc=0x%" PRIx64 " src_tile=%u addr_reg=%u size_code=%u "
                      "tile_bytes=%u lb0=%" PRIu64 " lb1=%" PRIu64 " lb2=%" PRIu64 " arg=0x%x dtype=%u\n",
                      env->pc, src_tile, addr_reg, size_code, env->tile_reg_bytes[src_tile],
                      (uint64_t)env->lb[0], (uint64_t)env->lb[1], (uint64_t)env->lb[2],
                      env->tile_arg_format, env->tile_dtype);
    }

    if (src_tile >= 32 || addr_reg >= LINX_GPR_COUNT) {
        if (linx_tile_debug()) {
            qemu_log_mask(LOG_GUEST_ERROR, "linx_tile_store: invalid src_tile/addr_reg\n");
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (!linx_tile_bytes_in_strict_range(bytes64)) {
        if (linx_tile_debug()) {
            qemu_log_mask(LOG_GUEST_ERROR, "linx_tile_store: invalid bytes64=%" PRIu64 "\n", bytes64);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const size_t bytes = (size_t)bytes64;
    const uint32_t dtype = env->tile_dtype;
    const unsigned elem_bits = linx_tile_dtype_elem_bits(dtype);
    const unsigned elem_bytes = (elem_bits + 7u) / 8u;
    if (((bytes * 8u) % elem_bits) != 0u) {
        if (linx_tile_debug()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "linx_tile_store: bytes %zu not divisible by elem_bits=%u (dtype=%u)\n",
                          bytes, elem_bits, dtype);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (env->tile_reg_bytes[src_tile] < bytes) {
        if (linx_tile_debug()) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "linx_tile_store: tile_reg_bytes[%u]=%u < bytes=%zu\n",
                          src_tile, env->tile_reg_bytes[src_tile], bytes);
        }
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t tile_elems = (bytes * 8u) / elem_bits;
    const uint64_t cols = env->lb[0] ? (env->lb[0] & 0xffffffffu) : tile_elems;
    const uint64_t rows = env->lb[1] ? (env->lb[1] & 0xffffffffu) : 1u;
    if (rows == 0u || cols == 0u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (rows > UINT64_MAX / cols) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t active_elems = rows * cols;
    if (active_elems > tile_elems) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    if (cols > UINT64_MAX / elem_bits) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t row_span_bits = cols * elem_bits;
    const uint64_t row_span_bytes = (row_span_bits + 7u) / 8u;
    uint64_t stride_bytes = 0u;
    if (!linx_tile_get_stride_bytes(env, &stride_bytes)) {
        stride_bytes = row_span_bytes;
    }
    if (stride_bytes < row_span_bytes ||
        (elem_bytes != 0u && (stride_bytes % elem_bytes) != 0u)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t base = env->gpr[addr_reg];
    for (uint64_t r = 0; r < rows; ++r) {
        const uint64_t row_base = base + r * stride_bytes;
        for (uint64_t c = 0; c < cols; ++c) {
            const uint64_t idx = r * cols + c;
            uint64_t value = 0;
            if (!linx_tile_get_elem(env, src_tile, (uint32_t)idx, elem_bits, &value)) {
                if (linx_tile_debug()) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "linx_tile_store: get_elem failure idx=%" PRIu64 " elem_bits=%u\n",
                                  idx, elem_bits);
                }
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (elem_bits == 4u) {
                const uint64_t byte_addr = row_base + (c >> 1u);
                uint64_t packed = linx_tile_mem_read(env, byte_addr, 1u);
                const uint8_t nibble = (uint8_t)(value & 0x0fu);
                if ((c & 1u) == 0u) {
                    packed = (packed & 0xf0u) | nibble;
                } else {
                    packed = (packed & 0x0fu) | ((uint64_t)nibble << 4u);
                }
                linx_tile_mem_write(env, byte_addr, 1u, packed);
            } else {
                linx_tile_mem_write(env, row_base + c * elem_bytes, elem_bytes, value);
            }
        }
    }
}

static void linx_tile_mamulb(CPULinxState *env, unsigned src_a, unsigned src_b,
                             unsigned size_code)
{
    if (src_a >= 32 || src_b >= 32) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned m = env->lb[0] ? (unsigned)env->lb[0] : 8u;
    const unsigned n = env->lb[1] ? (unsigned)env->lb[1] : 8u;
    const unsigned kdim = env->lb[2] ? (unsigned)env->lb[2] : 8u;

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (!linx_tile_bytes_in_strict_range(bytes64)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (m > 0u && n > 0u && kdim > 0u) {
        const uint64_t mn = (uint64_t)m * (uint64_t)n;
        if (mn > UINT64_MAX / (uint64_t)kdim ||
            (mn * (uint64_t)kdim) > (bytes64 / 4u)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }
    env->tile_acc_bytes = (uint32_t)bytes64;
    const unsigned words = (unsigned)(bytes64 / 4u);

    if (m == 0u || n == 0u || kdim == 0u ||
        (uint64_t)m * (uint64_t)n > words ||
        (uint64_t)m * (uint64_t)kdim > LINX_TILE_MAX_WORDS ||
        (uint64_t)kdim * (uint64_t)n > LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            int64_t acc = 0;
            for (unsigned k = 0; k < kdim; k++) {
                const uint64_t a_idx = (uint64_t)i * (uint64_t)kdim + (uint64_t)k;
                const uint64_t b_idx = (uint64_t)k * (uint64_t)n + (uint64_t)j;
                if (a_idx >= LINX_TILE_MAX_WORDS || b_idx >= LINX_TILE_MAX_WORDS) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    return;
                }
                const int32_t a = (int32_t)env->tile_reg[src_a][a_idx];
                const int32_t b = (int32_t)env->tile_reg[src_b][b_idx];
                acc += (int64_t)a * (int64_t)b;
            }
            const uint64_t out_idx = (uint64_t)i * (uint64_t)n + (uint64_t)j;
            if (out_idx >= LINX_TILE_MAX_WORDS) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            env->tile_acc[out_idx] = (uint32_t)(int32_t)acc;
        }
    }

    /* Zero the rest of the accumulator for determinism. */
    const unsigned active_words = (unsigned)MIN((uint64_t)words, (uint64_t)m * (uint64_t)n);
    for (unsigned i = active_words; i < words && i < LINX_TILE_MAX_WORDS; i++) {
        env->tile_acc[i] = 0;
    }
}

static void linx_tile_mamulb_acc(CPULinxState *env, unsigned src_a, unsigned src_b,
                                 unsigned size_code)
{
    if (src_a >= 32 || src_b >= 32) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned m = env->lb[0] ? (unsigned)env->lb[0] : 8u;
    const unsigned n = env->lb[1] ? (unsigned)env->lb[1] : 8u;
    const unsigned kdim = env->lb[2] ? (unsigned)env->lb[2] : 8u;

    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (!linx_tile_bytes_in_strict_range(bytes64)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (m > 0u && n > 0u && kdim > 0u) {
        const uint64_t mn = (uint64_t)m * (uint64_t)n;
        if (mn > UINT64_MAX / (uint64_t)kdim ||
            (mn * (uint64_t)kdim) > (bytes64 / 4u)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }
    env->tile_acc_bytes = (uint32_t)bytes64;

    const unsigned words = (unsigned)(bytes64 / 4u);
    if (m == 0u || n == 0u || kdim == 0u ||
        (uint64_t)m * (uint64_t)n > words ||
        (uint64_t)m * (uint64_t)kdim > LINX_TILE_MAX_WORDS ||
        (uint64_t)kdim * (uint64_t)n > LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    for (unsigned i = 0; i < m; i++) {
        for (unsigned j = 0; j < n; j++) {
            const uint64_t out_idx = (uint64_t)i * (uint64_t)n + (uint64_t)j;
            if (out_idx >= LINX_TILE_MAX_WORDS) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            int64_t acc = (int32_t)env->tile_acc[out_idx];
            for (unsigned k = 0; k < kdim; k++) {
                const uint64_t a_idx = (uint64_t)i * (uint64_t)kdim + (uint64_t)k;
                const uint64_t b_idx = (uint64_t)k * (uint64_t)n + (uint64_t)j;
                if (a_idx >= LINX_TILE_MAX_WORDS || b_idx >= LINX_TILE_MAX_WORDS) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    return;
                }
                const int32_t a = (int32_t)env->tile_reg[src_a][a_idx];
                const int32_t b = (int32_t)env->tile_reg[src_b][b_idx];
                acc += (int64_t)a * (int64_t)b;
            }
            env->tile_acc[out_idx] = (uint32_t)(int32_t)acc;
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
    if (!linx_tile_bytes_in_strict_range(bytes64)) {
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
}

static bool linx_tile_resolve_ior(const CPULinxState *env, unsigned slot,
                                  unsigned *addr_reg_out)
{
    /*
     * v0.3 SIMT contract: RI registers are an ordered namespace bound by
     * header B.IOR descriptors.
     *
     * Encoding still carries a RegDst field, but bring-up streams keep it zero
     * and rely on order rather than explicit slot IDs.
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

        /*
         * v0.3 bring-up disassembly contract prints B.IOR sources as
         * `[RegSrc1, RegSrc0]` (base, stride), with an optional third source
         * (`RegSrc2`) printed in the second bracket list.
         *
         * For SIMT bodies, `ri*` maps to this ordered stream of non-zero
         * sources (RegSrc1 then RegSrc0 then RegSrc2) across the header's
         * descriptor sequence.
         */
        const unsigned srcs[3] = { src1, src0, src2 };
        for (unsigned s = 0; s < 3; s++) {
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
        const unsigned srcs[3] = { src1, src0, src2 };

        for (unsigned s = 0; s < 3; s++) {
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

static inline float linx_u32_to_f32(uint32_t bits)
{
    union {
        uint32_t u;
        float f;
    } cvt = { .u = bits };
    return cvt.f;
}

static inline uint32_t linx_f32_to_u32(float value)
{
    union {
        uint32_t u;
        float f;
    } cvt = { .f = value };
    return cvt.u;
}

static bool linx_tile_resolve_desc_dst(const LinxTileIOTDesc *d, unsigned *dst_tile_out)
{
    const bool src0_present = (d->flags & LINX_IOT_S0V) == 0;
    const bool src1_present = (d->flags & LINX_IOT_S1V) == 0;

    if (!src1_present) {
        *dst_tile_out = d->src1 & 0x1fu;
        return true;
    }
    if (!src0_present) {
        *dst_tile_out = d->src0 & 0x1fu;
        return true;
    }
    return false;
}

static bool linx_tile_resolve_scalar_word(const CPULinxState *env, uint32_t *scalar_word_out)
{
    if (env->tile_ior_count == 0) {
        return false;
    }
    const uint64_t desc = env->tile_ior_desc[env->tile_ior_count - 1u];
    const unsigned src0 = (unsigned)((desc >> 5) & 0x1fu);
    const unsigned src1 = (unsigned)((desc >> 10) & 0x1fu);
    const unsigned scalar_reg = (src0 != 0u) ? src0 : src1;
    if (scalar_reg >= LINX_GPR_COUNT) {
        return false;
    }
    *scalar_word_out = (uint32_t)(env->gpr[scalar_reg] & 0xffffffffu);
    return true;
}

static void linx_tile_tepl_exec(CPULinxState *env,
                                const LinxTileIOTDesc *in_desc,
                                const LinxTileIOTDesc *out_desc,
                                unsigned size_code)
{
    const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
    if (!linx_tile_bytes_in_strict_range(bytes64)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const unsigned words = (unsigned)(bytes64 / 4u);
    if (words > LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned tile_op = env->tile_func & 0x3ffu;
    const unsigned mode = env->tile_arg_format & 0x3u;

    unsigned dst_tile = 0;
    if (!linx_tile_resolve_desc_dst(out_desc, &dst_tile) || dst_tile >= 32u) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const bool src0_present = (in_desc->flags & LINX_IOT_S0V) == 0;
    const bool src1_present = (in_desc->flags & LINX_IOT_S1V) == 0;
    const unsigned src0_tile = in_desc->src0 & 0x1fu;
    const unsigned src1_tile = in_desc->src1 & 0x1fu;

    if (src0_present && (src0_tile >= 32u || env->tile_reg_bytes[src0_tile] < bytes64)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if (src1_present && (src1_tile >= 32u || env->tile_reg_bytes[src1_tile] < bytes64)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    uint32_t scalar_word = 0u;
    if (mode != 0u && !linx_tile_resolve_scalar_word(env, &scalar_word)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    switch (tile_op) {
    case LINX_TEPL_TADD:
        if (mode != 0u || !src0_present || !src1_present) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            env->tile_reg[dst_tile][i] =
                env->tile_reg[src0_tile][i] + env->tile_reg[src1_tile][i];
        }
        break;
    case LINX_TEPL_TSUB:
        if (mode != 0u || !src0_present || !src1_present) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            env->tile_reg[dst_tile][i] =
                env->tile_reg[src0_tile][i] - env->tile_reg[src1_tile][i];
        }
        break;
    case LINX_TEPL_TMUL:
        if (!src0_present || (mode == 0u && !src1_present) || (mode != 0u && mode != 1u)) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            const float lhs = linx_u32_to_f32(env->tile_reg[src0_tile][i]);
            const float rhs = (mode == 1u)
                                  ? linx_u32_to_f32(scalar_word)
                                  : linx_u32_to_f32(env->tile_reg[src1_tile][i]);
            env->tile_reg[dst_tile][i] = linx_f32_to_u32(lhs * rhs);
        }
        break;
    case LINX_TEPL_TMAX:
        if (mode != 0u || !src0_present || !src1_present) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            const float lhs = linx_u32_to_f32(env->tile_reg[src0_tile][i]);
            const float rhs = linx_u32_to_f32(env->tile_reg[src1_tile][i]);
            env->tile_reg[dst_tile][i] = linx_f32_to_u32(lhs > rhs ? lhs : rhs);
        }
        break;
    case LINX_TEPL_TCVT:
    case LINX_TEPL_TROWMAX:
    case LINX_TEPL_TROWSUM:
    case LINX_TEPL_TCOLEXPAND:
        if (mode != 0u || !src0_present) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            env->tile_reg[dst_tile][i] = env->tile_reg[src0_tile][i];
        }
        break;
    case LINX_TEPL_TEXP:
        if (mode != 0u || !src0_present) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            const float v = linx_u32_to_f32(env->tile_reg[src0_tile][i]);
            env->tile_reg[dst_tile][i] = linx_f32_to_u32(expf(v));
        }
        break;
    case LINX_TEPL_TRECIP:
        if (mode != 0u || !src0_present) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            const float v = linx_u32_to_f32(env->tile_reg[src0_tile][i]);
            const float inv = (v == 0.0f) ? 0.0f : (1.0f / v);
            env->tile_reg[dst_tile][i] = linx_f32_to_u32(inv);
        }
        break;
    case LINX_TEPL_TEXPANDS:
        if (mode != 2u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
        for (unsigned i = 0; i < words; ++i) {
            env->tile_reg[dst_tile][i] = scalar_word;
        }
        break;
    default:
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    for (unsigned i = words; i < LINX_TILE_MAX_WORDS; ++i) {
        env->tile_reg[dst_tile][i] = 0u;
    }
    env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
}

void HELPER(linx_tile_reset_block)(CPULinxState *env)
{
    env->tile_arg_format = 0;
    env->tile_attr_raw = 0;
    env->tile_attr_pad = 0;
    env->tile_attr_dtype = 0;
    env->tile_ior_count = 0;
    env->vec_ri_count = 0;
    env->tile_iot_count = 0;
}

void HELPER(linx_tile_set_arg)(CPULinxState *env, uint32_t format)
{
    env->tile_arg_format = format & 0x1fu;
}

void HELPER(linx_tile_set_attr)(CPULinxState *env, uint32_t raw)
{
    const uint32_t bits = raw & 0x003fffffu;
    env->tile_attr_raw = bits;
    env->tile_attr_dtype = (bits >> 7) & 0x1fu;
    env->tile_attr_pad = (bits >> 12) & 0x1fu;
}

void HELPER(linx_tile_append_ior)(CPULinxState *env, uint64_t packed)
{
    if (env->tile_ior_count >= LINX_TILE_MAX_IOR) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    env->tile_ior_desc[env->tile_ior_count++] = packed;
}

static inline void linx_tile_accumulate_iot_sets(const LinxTileIOTDesc *d,
                                                 uint32_t *input_mask,
                                                 uint32_t *output_mask)
{
    const bool src0_present = (d->flags & LINX_IOT_S0V) == 0;
    const bool src1_present = (d->flags & LINX_IOT_S1V) == 0;

    if (src0_present) {
        *input_mask |= 1u << (d->src0 & 0x1fu);
    }
    if (src1_present) {
        *input_mask |= 1u << (d->src1 & 0x1fu);
    }

    if (!src1_present) {
        *output_mask |= 1u << (d->src1 & 0x1fu);
    } else if (!src0_present) {
        *output_mask |= 1u << (d->src0 & 0x1fu);
    }
}

static bool linx_tile_validate_simt_body_iot_limits(const CPULinxState *env,
                                                    const LinxTileIOTDesc *new_desc)
{
    if (!linx_block_has_simt_body(env->blocktype)) {
        return true;
    }

    uint32_t input_mask = 0u;
    uint32_t output_mask = 0u;
    for (unsigned i = 0; i < env->tile_iot_count; ++i) {
        const LinxTileIOTDesc d = linx_tile_decode_iot(env->tile_iot_desc[i]);
        linx_tile_accumulate_iot_sets(&d, &input_mask, &output_mask);
    }
    linx_tile_accumulate_iot_sets(new_desc, &input_mask, &output_mask);

    return __builtin_popcount(input_mask) <= 3 &&
           __builtin_popcount(output_mask) <= 1;
}

static bool linx_tile_validate_dynamic_iot_sizes(const CPULinxState *env,
                                                 const LinxTileIOTDesc *d)
{
    if (!d->has_size && d->reg != 0u) {
        uint32_t dyn_size_code = 0u;
        if (d->reg >= LINX_GPR_COUNT ||
            !linx_tile_size_code_from_raw(env->gpr[d->reg], &dyn_size_code)) {
            return false;
        }
        (void)dyn_size_code;
    }

    const bool src0_present = (d->flags & LINX_IOT_S0V) == 0;
    const bool src1_present = (d->flags & LINX_IOT_S1V) == 0;
    if (src0_present) {
        const uint64_t bytes = env->tile_reg_bytes[d->src0 & 0x1fu];
        if (bytes != 0u && !linx_tile_bytes_in_strict_range(bytes)) {
            return false;
        }
    }
    if (src1_present) {
        const uint64_t bytes = env->tile_reg_bytes[d->src1 & 0x1fu];
        if (bytes != 0u && !linx_tile_bytes_in_strict_range(bytes)) {
            return false;
        }
    }
    return true;
}

void HELPER(linx_tile_append_iot)(CPULinxState *env, uint64_t packed)
{
    const LinxTileIOTDesc desc = linx_tile_decode_iot(packed);

    if (desc.has_size && env->tile_iot_count > 0) {
        const unsigned prev_idx = env->tile_iot_count - 1;
        LinxTileIOTDesc prev = linx_tile_decode_iot(env->tile_iot_desc[prev_idx]);
        const bool reg_compatible = (desc.reg == 0) || (prev.reg == desc.reg);
        if (!prev.has_size &&
            prev.src0 == desc.src0 &&
            prev.src1 == desc.src1 &&
            prev.dst == desc.dst &&
            prev.grp == desc.grp &&
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
    if (!linx_tile_validate_simt_body_iot_limits(env, &desc) ||
        !linx_tile_validate_dynamic_iot_sizes(env, &desc)) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    env->tile_iot_desc[env->tile_iot_count++] = packed;

    /*
     * v0.3 bring-up: allocate/size the destination tile when it is encoded in
     * an absent source slot and a size code is present (B.IOTI).
     *
     * This supports `.local` vector accesses using TO/TS bases, which rely on
     * B.IOTI to establish tile footprints.
     */
    if (linx_tile_desc_has_explicit_size(&desc)) {
        const bool allow_tile_alloc =
            /* TLOAD writes an output tile. */
            (env->blocktype == LINX_BLOCK_TMA &&
             (env->tile_func & 0x1f) == LINX_TMA_TLOAD) ||
            /* TEPL writes an output tile. */
            (env->blocktype == LINX_BLOCK_TEPL) ||
            /* ACCCVT writes an output tile. */
            (env->blocktype == LINX_BLOCK_CUBE &&
             (env->tile_func & 0x1f) == LINX_CUBE_ACCCVT) ||
            /* Vector blocks use B.IOTI to establish TO/TS footprints. */
            (env->blocktype == 0 || env->blocktype == 1 ||
             env->blocktype == 4 || env->blocktype == 5);
        if (!allow_tile_alloc) {
            return;
        }

        /*
         * Only allocate/clear destination footprints on output-group
         * descriptors (group=1). TEPL unary/scalar input descriptors may
         * intentionally mark one source slot absent, and treating those as
         * destination bindings would clobber live source tiles.
         */
        if ((desc.grp & 0x1u) == 0u) {
            return;
        }

        const bool src0_present = (desc.flags & LINX_IOT_S0V) == 0;
        const bool src1_present = (desc.flags & LINX_IOT_S1V) == 0;

        bool have_dst_tile = false;
        unsigned dst_tile = 0;
        if (!src1_present) {
            dst_tile = desc.src1 & 0x1f;
            have_dst_tile = true;
        } else if (!src0_present) {
            dst_tile = desc.src0 & 0x1f;
            have_dst_tile = true;
        }

        if (have_dst_tile && dst_tile < 32) {
            uint64_t bytes64 = 0u;
            if (!linx_tile_desc_get_size_bytes(env, &desc, &bytes64)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                return;
            }
            if (linx_tile_bytes_in_strict_range(bytes64)) {
                bool dst_aliases_input = false;

                if (src0_present && ((desc.src0 & 0x1fu) == dst_tile)) {
                    dst_aliases_input = true;
                }
                if (src1_present && ((desc.src1 & 0x1fu) == dst_tile)) {
                    dst_aliases_input = true;
                }

                /*
                 * Do not clobber in-place inputs while parsing descriptors.
                 * This is required for VPAR/VSEQ in-place tile binops where
                 * descriptor 0 binds inputs and descriptor 1 binds output.
                 */
                for (unsigned i = 0; i + 1u < env->tile_iot_count &&
                                      !dst_aliases_input;
                     ++i) {
                    const LinxTileIOTDesc prev =
                        linx_tile_decode_iot(env->tile_iot_desc[i]);
                    const bool prev_src0_present =
                        (prev.flags & LINX_IOT_S0V) == 0;
                    const bool prev_src1_present =
                        (prev.flags & LINX_IOT_S1V) == 0;
                    if (prev_src0_present && ((prev.src0 & 0x1fu) == dst_tile)) {
                        dst_aliases_input = true;
                    }
                    if (prev_src1_present && ((prev.src1 & 0x1fu) == dst_tile)) {
                        dst_aliases_input = true;
                    }
                }

                if (dst_aliases_input) {
                    const uint32_t cur_bytes = env->tile_reg_bytes[dst_tile];
                    if (cur_bytes != 0u && cur_bytes < bytes64) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    }
                    return;
                }

                const unsigned words = (unsigned)(bytes64 / 4u);
                for (unsigned w = 0; w < words && w < LINX_TILE_MAX_WORDS; w++) {
                    env->tile_reg[dst_tile][w] = 0;
                }
                for (unsigned w = words; w < LINX_TILE_MAX_WORDS; w++) {
                    env->tile_reg[dst_tile][w] = 0;
                }
                env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
            }
        }
    }
}

void HELPER(linx_tile_commit)(CPULinxState *env)
{
    if (env->tile_iot_count == 0 && env->tile_iot_valid == 0) {
        return;
    }

    switch (env->blocktype) {
    case LINX_BLOCK_TMA:
        switch (env->tile_func & 0x1f) {
        case LINX_TMA_TLOAD: {
            const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
            for (unsigned i = 0; i < count; i++) {
                LinxTileIOTDesc d;
                if (env->tile_iot_count) {
                    d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                } else {
                    d.src0 = env->tile_iot_src0 & 0x1f;
                    d.src1 = env->tile_iot_src1 & 0x1f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.grp = env->tile_iot_grp & 0x1;
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
                uint32_t size_code = 0u;
                if (!linx_tile_desc_get_size_code(env, &d, &size_code)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }

                const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
                const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
                unsigned dst_tile = 0;
                if (!src1_present) {
                    dst_tile = d.src1 & 0x1f;
                } else if (!src0_present) {
                    dst_tile = d.src0 & 0x1f;
                } else {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_load(env, dst_tile, addr_reg, size_code);
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
                    d.src0 = env->tile_iot_src0 & 0x1f;
                    d.src1 = env->tile_iot_src1 & 0x1f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.grp = env->tile_iot_grp & 0x1;
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
                    src_tile = d.src0 & 0x1f;
                } else if (src1_present) {
                    src_tile = d.src1 & 0x1f;
                } else {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                uint32_t size_code = 0u;
                if (!linx_tile_desc_get_size_code(env, &d, &size_code)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                linx_tile_store(env, src_tile, addr_reg, size_code);
            }
            break;
        }
        case LINX_TMA_TMOV: {
            const unsigned count = env->tile_iot_count ? env->tile_iot_count : 1u;
            const unsigned mode = env->tile_arg_format & 0x1u; /* V2V=0, A2V=1 */
            for (unsigned i = 0; i < count; i++) {
                LinxTileIOTDesc d;
                if (env->tile_iot_count) {
                    d = linx_tile_decode_iot(env->tile_iot_desc[i]);
                } else {
                    d.src0 = env->tile_iot_src0 & 0x1f;
                    d.src1 = env->tile_iot_src1 & 0x1f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.grp = env->tile_iot_grp & 0x1;
                    d.flags = env->tile_iot_flags & 0xf;
                    d.reg = env->tile_iot_reg & 0x1f;
                    d.size = env->tile_iot_size & 0x1f;
                    d.has_size = env->tile_iot_size != 0;
                }

                unsigned dst_tile = 0;
                if (!linx_tile_resolve_desc_dst(&d, &dst_tile) || dst_tile >= 32u) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }

                uint64_t bytes64 = 0u;
                if (mode == 0u) {
                    const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
                    const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
                    unsigned src_tile = 0;
                    if (src0_present) {
                        src_tile = d.src0 & 0x1f;
                    } else if (src1_present) {
                        src_tile = d.src1 & 0x1f;
                    } else {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                    if (src_tile >= 32u) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                    if (linx_tile_desc_has_explicit_size(&d)) {
                        if (!linx_tile_desc_get_size_bytes(env, &d, &bytes64)) {
                            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                            break;
                        }
                    } else {
                        bytes64 = env->tile_reg_bytes[src_tile];
                    }
                    if (!linx_tile_bytes_in_strict_range(bytes64) ||
                        env->tile_reg_bytes[src_tile] < bytes64) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                    const unsigned words = (unsigned)(bytes64 / 4u);
                    for (unsigned w = 0; w < words; ++w) {
                        env->tile_reg[dst_tile][w] = env->tile_reg[src_tile][w];
                    }
                } else {
                    if (linx_tile_desc_has_explicit_size(&d)) {
                        if (!linx_tile_desc_get_size_bytes(env, &d, &bytes64)) {
                            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                            break;
                        }
                    } else {
                        bytes64 = env->tile_acc_bytes;
                    }
                    if (!linx_tile_bytes_in_strict_range(bytes64) ||
                        env->tile_acc_bytes < bytes64) {
                        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                        break;
                    }
                    const unsigned words = (unsigned)(bytes64 / 4u);
                    for (unsigned w = 0; w < words; ++w) {
                        env->tile_reg[dst_tile][w] = env->tile_acc[w];
                    }
                }

                const unsigned words = (unsigned)(bytes64 / 4u);
                for (unsigned w = words; w < LINX_TILE_MAX_WORDS; ++w) {
                    env->tile_reg[dst_tile][w] = 0u;
                }
                env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
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
                    d.src0 = env->tile_iot_src0 & 0x1f;
                    d.src1 = env->tile_iot_src1 & 0x1f;
                    d.dst = env->tile_iot_dst & 0x7;
                    d.grp = env->tile_iot_grp & 0x1;
                    d.flags = env->tile_iot_flags & 0xf;
                    d.reg = env->tile_iot_reg & 0x1f;
                    d.size = env->tile_iot_size & 0x1f;
                    d.has_size = env->tile_iot_size != 0;
                }

                const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
                const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;

                unsigned src_tile = 0;
                if (src0_present) {
                    src_tile = d.src0 & 0x1f;
                } else if (src1_present) {
                    src_tile = d.src1 & 0x1f;
                } else {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }

                unsigned dst_tile = 0;
                if (!src1_present) {
                    dst_tile = d.src1 & 0x1f;
                } else if (!src0_present) {
                    dst_tile = d.src0 & 0x1f;
                } else {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }

                uint32_t size_code = 0u;
                if (!linx_tile_desc_get_size_code(env, &d, &size_code)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const uint64_t bytes64 = (size_code < 60u) ? (1ull << (size_code + 4u)) : 0ull;
                if (!linx_tile_bytes_in_strict_range(bytes64)) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                const unsigned words = (unsigned)(bytes64 / 4u);
                if (src_tile >= 32 || dst_tile >= 32 ||
                    env->tile_reg_bytes[src_tile] < bytes64) {
                    helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                    break;
                }
                for (unsigned w = 0; w < words && w < LINX_TILE_MAX_WORDS; w++) {
                    env->tile_reg[dst_tile][w] = env->tile_reg[src_tile][w];
                }
                for (unsigned w = words; w < LINX_TILE_MAX_WORDS; w++) {
                    env->tile_reg[dst_tile][w] = 0;
                }
                env->tile_reg_bytes[dst_tile] = (uint32_t)bytes64;
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
        case LINX_CUBE_MAMULB: {
            LinxTileIOTDesc d;
            if (env->tile_iot_count) {
                d = linx_tile_decode_iot(env->tile_iot_desc[0]);
            } else {
                d.src0 = env->tile_iot_src0 & 0x1f;
                d.src1 = env->tile_iot_src1 & 0x1f;
                d.flags = env->tile_iot_flags & 0xf;
                d.grp = env->tile_iot_grp & 0x1;
                d.dst = env->tile_iot_dst & 0x7;
                d.reg = env->tile_iot_reg & 0x1f;
                d.size = env->tile_iot_size & 0x1f;
                d.has_size = env->tile_iot_size != 0;
            }
            if ((d.flags & (LINX_IOT_S0V | LINX_IOT_S1V)) != 0) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            uint32_t size_code = 0u;
            if (!linx_tile_desc_get_size_code(env, &d, &size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            linx_tile_mamulb(env, d.src0 & 0x1f, d.src1 & 0x1f, size_code);
            break;
        }
        case LINX_CUBE_MAMULB_ACC: {
            LinxTileIOTDesc d;
            if (env->tile_iot_count) {
                d = linx_tile_decode_iot(env->tile_iot_desc[0]);
            } else {
                d.src0 = env->tile_iot_src0 & 0x1f;
                d.src1 = env->tile_iot_src1 & 0x1f;
                d.flags = env->tile_iot_flags & 0xf;
                d.grp = env->tile_iot_grp & 0x1;
                d.dst = env->tile_iot_dst & 0x7;
                d.reg = env->tile_iot_reg & 0x1f;
                d.size = env->tile_iot_size & 0x1f;
                d.has_size = env->tile_iot_size != 0;
            }
            if ((d.flags & (LINX_IOT_S0V | LINX_IOT_S1V)) != 0) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            uint32_t size_code = 0u;
            if (!linx_tile_desc_get_size_code(env, &d, &size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }

            linx_tile_mamulb_acc(env, d.src0 & 0x1f, d.src1 & 0x1f, size_code);
            break;
        }
        case LINX_CUBE_ACCCVT: {
            LinxTileIOTDesc d;
            if (env->tile_iot_count) {
                d = linx_tile_decode_iot(env->tile_iot_desc[0]);
            } else {
                d.src0 = env->tile_iot_src0 & 0x1f;
                d.src1 = env->tile_iot_src1 & 0x1f;
                d.flags = env->tile_iot_flags & 0xf;
                d.reg = env->tile_iot_reg & 0x1f;
                d.size = env->tile_iot_size & 0x1f;
                d.has_size = env->tile_iot_size != 0;
            }

            const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
            const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
            unsigned dst_tile = 0;
            if (!src1_present) {
                dst_tile = d.src1 & 0x1f;
            } else if (!src0_present) {
                dst_tile = d.src0 & 0x1f;
            } else {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            uint32_t size_code = 0u;
            if (!linx_tile_desc_get_size_code(env, &d, &size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
            linx_tile_acccvt(env, dst_tile, size_code);
            break;
        }
        default:
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            break;
        }
        break;
    case LINX_BLOCK_TEPL: {
        LinxTileIOTDesc in_desc;
        LinxTileIOTDesc out_desc;

        if (env->tile_iot_count) {
            in_desc = linx_tile_decode_iot(env->tile_iot_desc[0]);
            out_desc = linx_tile_decode_iot(
                env->tile_iot_desc[env->tile_iot_count - 1u]);
        } else {
            in_desc.src0 = env->tile_iot_src0 & 0x1f;
            in_desc.src1 = env->tile_iot_src1 & 0x1f;
            in_desc.dst = env->tile_iot_dst & 0x7;
            in_desc.grp = env->tile_iot_grp & 0x1;
            in_desc.flags = env->tile_iot_flags & 0xf;
            in_desc.reg = env->tile_iot_reg & 0x1f;
            in_desc.size = env->tile_iot_size & 0x1f;
            in_desc.has_size = env->tile_iot_size != 0;
            out_desc = in_desc;
        }

        uint32_t size_code = 0u;
        if (!linx_tile_desc_get_size_code(env, &out_desc, &size_code)) {
            if (!linx_tile_desc_get_size_code(env, &in_desc, &size_code)) {
                helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
                break;
            }
        }
        linx_tile_tepl_exec(env, &in_desc, &out_desc, size_code);
        break;
    }
    default:
        /* Non-tile blocks: nothing to do. */
        break;
    }

    /* Consume the per-block descriptor. */
    env->tile_iot_valid = 0;
    env->tile_iot_size = 0;
    env->tile_iot_grp = 0;
    env->tile_arg_format = 0;
    env->tile_attr_raw = 0;
    env->tile_attr_pad = 0;
    env->tile_attr_dtype = 0;
    env->tile_ior_count = 0;
    env->tile_iot_count = 0;
}

/* ------------------------------------------------------------------------- */
/* v0.3 SIMT/vector helpers (bring-up subset)                                */
/* ------------------------------------------------------------------------- */

enum {
    LINX_VEC_REGCLASS_RI = 1,
    LINX_VEC_REGCLASS_LC = 3,
    LINX_VEC_REGCLASS_VT = 4,
    LINX_VEC_REGCLASS_VU = 5,
    LINX_VEC_REGCLASS_VM = 6,
    LINX_VEC_REGCLASS_VN = 7,
    LINX_VEC_REGCLASS_TBASE = 8,
};

static inline unsigned linx_vec_reg_class(uint32_t code)
{
    return (unsigned)((code >> 5) & 0x1fu);
}

static inline unsigned linx_vec_reg_index(uint32_t code)
{
    return (unsigned)(code & 0x1fu);
}

static bool linx_vec_resolve_tile_base(const CPULinxState *env, unsigned base_idx,
                                       unsigned *tile_out)
{
    /*
     * Strict v0.3 bring-up mapping:
     * - TA/TB/TC/TD: first four input tiles in header order
     * - TO/TS: first two output tiles (dst encoded in absent source slot)
     *
     * Inputs/outputs are derived from the active header's B.IOT/B.IOTI
     * descriptors.
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
            d.src0 = env->tile_iot_src0 & 0x1f;
            d.src1 = env->tile_iot_src1 & 0x1f;
            d.dst = env->tile_iot_dst & 0x7;
            d.grp = env->tile_iot_grp & 0x1;
            d.flags = env->tile_iot_flags & 0xf;
            d.reg = env->tile_iot_reg & 0x1f;
            d.size = env->tile_iot_size & 0x1f;
            d.has_size = env->tile_iot_size != 0;
        }

        const bool src0_present = (d.flags & LINX_IOT_S0V) == 0;
        const bool src1_present = (d.flags & LINX_IOT_S1V) == 0;
        if (src0_present && input_count < 4) {
            inputs[input_count++] = d.src0 & 0x1f;
        }
        if (src1_present && input_count < 4) {
            inputs[input_count++] = d.src1 & 0x1f;
        }

        bool have_dst_tile = false;
        unsigned dst_tile = 0;
        if (!src1_present) {
            dst_tile = d.src1 & 0x1f;
            have_dst_tile = true;
        } else if (!src0_present) {
            dst_tile = d.src0 & 0x1f;
            have_dst_tile = true;
        }
        if (have_dst_tile && output_count < 2) {
            outputs[output_count++] = dst_tile & 0x1f;
        }
    }

    if (base_idx < 4) {
        if (base_idx < input_count) {
            *tile_out = inputs[base_idx] & 0x1f;
            return true;
        }
        return false;
    }
    if (base_idx == 4) { /* TO */
        if (output_count >= 1) {
            *tile_out = outputs[0] & 0x1f;
            return true;
        }
        return false;
    }
    if (base_idx == 5) { /* TS */
        if (output_count >= 2) {
            *tile_out = outputs[1] & 0x1f;
            return true;
        }
        return false;
    }
    return false;
}

static uint64_t linx_vec_read_reg(CPULinxState *env, uint32_t code)
{
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
    const uint32_t attr = env->tile_attr_raw;
    const uint32_t aqrl_mask = (1u << 18) | (1u << 21);
    if (env->blocktype == LINX_BLOCK_VPAR || env->blocktype == LINX_BLOCK_VSEQ) {
        if (attr != 0u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    } else if (env->blocktype == LINX_BLOCK_MPAR || env->blocktype == LINX_BLOCK_MSEQ) {
        if ((attr & ~aqrl_mask) != 0u) {
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }

    /* v0.3 bring-up: initialize loop counters and clear transient VT state. */
    env->lc[0] = 0;
    env->lc[1] = 0;
    env->lc[2] = 0;
    if (linx_trace_vbrg()) {
        linx_trace_vbrg_body_count++;
    }
    linx_vec_capture_ri_values(env);
    for (unsigned i = 0; i < LINX_VEC_QUEUE_DEPTH; i++) {
        env->vtq[i] = 0;
        env->vuq[i] = 0;
        env->vmq[i] = 0;
        env->vnq[i] = 0;
    }
}

uint32_t HELPER(linx_vec_body_next)(CPULinxState *env)
{
    const uint64_t lb0 = env->lb[0];
    const uint64_t lb1 = env->lb[1] ? env->lb[1] : 1;
    const uint64_t lb2 = env->lb[2] ? env->lb[2] : 1;

    if (lb0 == 0) {
        return 0;
    }

    env->lc[0]++;
    if (env->lc[0] < lb0) {
        return 1;
    }
    env->lc[0] = 0;

    env->lc[1]++;
    if (env->lc[1] < lb1) {
        return 1;
    }
    env->lc[1] = 0;

    env->lc[2]++;
    if (env->lc[2] < lb2) {
        return 1;
    }
    env->lc[2] = 0;
    return 0;
}

static void linx_vec_write_dst(CPULinxState *env, uint32_t dst, uint64_t value)
{
    const unsigned cls = linx_vec_reg_class(dst);
    const unsigned didx = linx_vec_reg_index(dst);

    switch (cls) {
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

void HELPER(linx_v_add)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR, uint32_t srctype, uint32_t shamt)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    uint64_t rhs = linx_vec_read_reg(env, srcR);

    switch (srctype & 0x3u) {
    case 0: /* .sw */
        rhs = (uint64_t)(int64_t)(int32_t)rhs;
        break;
    case 1: /* .uw */
        rhs = (uint64_t)(uint32_t)rhs;
        break;
    case 2: /* .neg */
        rhs = (uint64_t)(-(int64_t)rhs);
        break;
    default:
        break;
    }

    if (shamt) {
        rhs <<= (shamt & 0x3fu);
    }

    const uint64_t res = lhs + rhs;
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_sub)(CPULinxState *env, uint32_t dst, uint32_t srcL,
                        uint32_t srcR, uint32_t srctype, uint32_t shamt)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    uint64_t rhs = linx_vec_read_reg(env, srcR);

    switch (srctype & 0x3u) {
    case 0: /* .sw */
        rhs = (uint64_t)(int64_t)(int32_t)rhs;
        break;
    case 1: /* .uw */
        rhs = (uint64_t)(uint32_t)rhs;
        break;
    case 2: /* .neg */
        rhs = (uint64_t)(-(int64_t)rhs);
        break;
    default:
        break;
    }

    if (shamt) {
        rhs <<= (shamt & 0x3fu);
    }

    const uint64_t res = lhs - rhs;
    linx_vec_write_dst(env, dst, res);
}

void HELPER(linx_v_mul)(CPULinxState *env, uint32_t dst, uint32_t srcL, uint32_t srcR)
{
    const uint64_t lhs = linx_vec_read_reg(env, srcL);
    const uint64_t rhs = linx_vec_read_reg(env, srcR);
    const uint64_t res = lhs * rhs;
    linx_vec_write_dst(env, dst, res);
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

void HELPER(linx_v_rdadd)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t acc = linx_vec_read_reduce_dst(env, dst);
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, acc + src);
}

void HELPER(linx_v_rdand)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t acc = linx_vec_read_reduce_dst(env, dst);
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, acc & src);
}

void HELPER(linx_v_rdfadd)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t acc = linx_vec_read_reduce_dst(env, dst);
    const uint64_t src = linx_vec_read_reg(env, srcL);
    const uint64_t res = linx_fp_binop_add(env, acc, src, /*srctype=*/1);
    linx_vec_write_reduce_dst(env, dst, res);
}

void HELPER(linx_v_rdfmax)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t acc = linx_vec_read_reduce_dst(env, dst);
    const uint64_t src = linx_vec_read_reg(env, srcL);
    const uint64_t res = linx_fp_cmp_lt(env, acc, src, /*srctype=*/1) ? src : acc;
    linx_vec_write_reduce_dst(env, dst, res);
}

void HELPER(linx_v_rdfmin)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t acc = linx_vec_read_reduce_dst(env, dst);
    const uint64_t src = linx_vec_read_reg(env, srcL);
    const uint64_t res = linx_fp_cmp_lt(env, src, acc, /*srctype=*/1) ? src : acc;
    linx_vec_write_reduce_dst(env, dst, res);
}

void HELPER(linx_v_rdmax)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const int64_t acc = (int64_t)linx_vec_read_reduce_dst(env, dst);
    const int64_t src = (int64_t)linx_vec_read_reg(env, srcL);
    const uint64_t res = (uint64_t)(acc > src ? acc : src);
    linx_vec_write_reduce_dst(env, dst, res);
}

void HELPER(linx_v_rdmin)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const int64_t acc = (int64_t)linx_vec_read_reduce_dst(env, dst);
    const int64_t src = (int64_t)linx_vec_read_reg(env, srcL);
    const uint64_t res = (uint64_t)(acc < src ? acc : src);
    linx_vec_write_reduce_dst(env, dst, res);
}

void HELPER(linx_v_rdor)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t acc = linx_vec_read_reduce_dst(env, dst);
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, acc | src);
}

void HELPER(linx_v_rdxor)(CPULinxState *env, uint32_t dst, uint32_t srcL)
{
    const uint64_t acc = linx_vec_read_reduce_dst(env, dst);
    const uint64_t src = linx_vec_read_reg(env, srcL);
    linx_vec_write_reduce_dst(env, dst, acc ^ src);
}

void HELPER(linx_v_sw_brg)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
                           uint32_t srcR, uint32_t shamt, uint32_t local)
{
    if (local != 0) {
        HELPER(linx_v_sw_local)(env, srcD, srcL, srcR, shamt, local);
        return;
    }
    if (linx_vec_reg_class(srcL) != LINX_VEC_REGCLASS_RI) {
        /* Strict v0.3: bridged/global accesses must use ri* base operands. */
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint64_t addr = base + (lane << 2) + (idx << (2u + (shamt & 0x1fu)));
    const uint32_t value = (uint32_t)linx_vec_read_reg(env, srcD);

    if (linx_trace_vbrg() &&
        linx_trace_vbrg_body_count > 0 &&
        linx_trace_vbrg_body_count <= linx_trace_vbrg_max_bodies &&
        (lane < 4 || (lane + 1) >= env->lb[0])) {
        fprintf(stderr,
                "Linx: VBRG.SW pc=0x%016" PRIx64 " body=%" PRIu64
                " lane=%" PRIu64 " base=0x%016" PRIx64 " idx=%" PRId64
                " shamt=%u addr=0x%016" PRIx64 " val=0x%08x\n",
                env->pc, linx_trace_vbrg_body_count, lane, base, (int64_t)idx,
                (unsigned)(shamt & 0x3fu), addr, value);
        fflush(stderr);
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
    if (linx_vec_reg_class(srcL) != LINX_VEC_REGCLASS_RI) {
        /* Strict v0.3: bridged/global accesses must use ri* base operands. */
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const uint64_t base = linx_vec_read_reg(env, srcL);
    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint64_t addr = base + (lane << 2) + (idx << (shamt & 0x3fu));

    linx_lr_clear(env);
    const uint32_t raw =
        cpu_ldl_mmu((CPUArchState *)env, addr, linx_oi_le(MO_UL), GETPC());
    const uint64_t value = (uint64_t)(int64_t)(int32_t)raw;

    if (linx_trace_vbrg() &&
        linx_trace_vbrg_body_count > 0 &&
        linx_trace_vbrg_body_count <= linx_trace_vbrg_max_bodies &&
        (lane < 4 || (lane + 1) >= env->lb[0])) {
        fprintf(stderr,
                "Linx: VBRG.LW pc=0x%016" PRIx64 " body=%" PRIu64
                " lane=%" PRIu64 " base=0x%016" PRIx64 " idx=%" PRId64
                " shamt=%u addr=0x%016" PRIx64 " val=0x%08x\n",
                env->pc, linx_trace_vbrg_body_count, lane, base, (int64_t)idx,
                (unsigned)(shamt & 0x3fu), addr, raw);
        fflush(stderr);
    }

    const unsigned cls = linx_vec_reg_class(dst);
    const unsigned didx = linx_vec_reg_index(dst);
    switch (cls) {
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

static bool linx_vec_resolve_local_tile(CPULinxState *env, uint32_t base_code,
                                        unsigned *tile_out)
{
    if (linx_vec_reg_class(base_code) != LINX_VEC_REGCLASS_TBASE) {
        return false;
    }
    unsigned idx = linx_vec_reg_index(base_code);
    unsigned tile = 0;
    if (!linx_vec_resolve_tile_base(env, idx, &tile)) {
        return false;
    }
    if (tile >= 32) {
        return false;
    }
    *tile_out = tile;
    return true;
}

void HELPER(linx_v_sw_local)(CPULinxState *env, uint32_t srcD, uint32_t srcL,
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
    {
        const unsigned base_idx = linx_vec_reg_index(srcL);
        if (base_idx != 4u && base_idx != 5u) {
            /* Strict-v0.3: local stores may only target TO/TS outputs. */
            helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
            return;
        }
    }

    const uint64_t idx = linx_vec_read_reg(env, srcR);
    const uint64_t lane = env->lc[0];
    const uint64_t off = (lane << 2) + (idx << (2u + (shamt & 0x1fu)));
    const uint32_t value = (uint32_t)linx_vec_read_reg(env, srcD);

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (bytes == 0 || off + 4u > bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((off & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    env->tile_reg[tile][word] = value;
}

void HELPER(linx_v_lw_local)(CPULinxState *env, uint32_t dst, uint32_t srcL,
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
    const uint64_t off = (lane << 2) + (idx << (shamt & 0x3fu));

    const uint32_t bytes = env->tile_reg_bytes[tile];
    if (bytes == 0 || off + 4u > bytes) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    if ((off & 3u) != 0) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }

    const unsigned word = (unsigned)(off >> 2);
    if (word >= LINX_TILE_MAX_WORDS) {
        helper_raise_exception(env, LINX_EXCP_ILLEGAL_INST);
        return;
    }
    const uint32_t raw = env->tile_reg[tile][word];
    const uint64_t value = (uint64_t)(int64_t)(int32_t)raw;

    const unsigned cls = linx_vec_reg_class(dst);
    const unsigned didx = linx_vec_reg_index(dst);
    switch (cls) {
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


static unsigned linx_insn_len(uint16_t hw)
{
    if ((hw & 0x1) == 0) {
        return ((hw & 0xf) == 0xe) ? 6 : 2;
    }
    return ((hw & 0xf) == 0xf) ? 8 : 4;
}

static bool linx_is_bstart_at_addr(CPULinxState *env, uint64_t pc)
{
    CPUState *cs = env_cpu(env);
    uint8_t buf[8];

    if (cpu_memory_rw_debug(cs, pc, buf, 2, 0) != 0) {
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
        if (cpu_memory_rw_debug(cs, pc, buf, 4, 0) != 0) {
            return false;
        }
        const uint32_t insn = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                              ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

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
        if (cpu_memory_rw_debug(cs, pc, buf, 6, 0) != 0) {
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

    return false;
}

void HELPER(linx_check_bstart_target)(CPULinxState *env, uint64_t target)
{
    /*
     * This helper is on the hot path for indirect control flow (RET/IND/ICALL
     * and template returns). Cache the most recently-validated targets to avoid
     * re-reading guest memory for tight call/return loops.
     *
     * Note: This cache is conservative for typical bare-metal workloads (code
     * is not self-modifying). If guest code changes, TB invalidation will
     * naturally trigger re-translation, but this cache may still accept a
     * previously-validated address until reset.
     */
    for (size_t i = 0; i < ARRAY_SIZE(env->bstart_cache); i++) {
        if (env->bstart_cache[i] == target) {
            return;
        }
    }

    if (linx_is_bstart_at_addr(env, target)) {
        env->bstart_cache[env->bstart_cache_next & (ARRAY_SIZE(env->bstart_cache) - 1)] = target;
        env->bstart_cache_next++;
        return;
    }

    CPUState *cs = env_cpu(env);

    {
        uint8_t buf[8] = { 0 };
        int rc = cpu_memory_rw_debug(cs, target, buf, sizeof(buf), 0);
        if (rc == 0) {
            const uint16_t hw = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            const unsigned len = linx_insn_len(hw);
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
    env->pending_trap_arg0 = target;
    env->pending_trap_cause = LINX_EBLOCK_CAUSE_BAD_BRANCH_TARGET;
    cs->exception_index = LINX_EXCP_BAD_BRANCH_TARGET;
    cpu_loop_exit_restore(cs, GETPC());
}

void HELPER(linx_watch_store)(CPULinxState *env, uint64_t pc, uint64_t addr,
                              uint64_t val, uint32_t size)
{
    CPUState *cs = env_cpu(env);
    static int do_abort = -1;
    static unsigned dump_count;

    if (do_abort < 0) {
        const char *mode = getenv("LINX_WATCH_STORE_MODE");
        do_abort = 1;
        if (mode && mode[0] && strcmp(mode, "0") != 0 &&
            (strcmp(mode, "log") == 0 || strcmp(mode, "LOG") == 0)) {
            do_abort = 0;
        }
    }

    fprintf(stderr,
            "Linx: WATCH store pc=0x%016" PRIx64 " ra=0x%016" PRIx64
            " sp=0x%016" PRIx64 " addr=0x%016" PRIx64
            " size=%u val=0x%016" PRIx64 "\n",
            pc, env->gpr[LINX_REG_RA], env->gpr[LINX_REG_SP], addr, size, val);
    if (dump_count < 4) {
        uint8_t raw[32 * 8];
        if (cpu_memory_rw_debug(cs, env->gpr[LINX_REG_SP], raw, sizeof(raw), 0) == 0) {
            for (int i = 0; i < 32; i++) {
                uint64_t w = ldl_le_p(raw + i * 8);
                w |= ((uint64_t)ldl_le_p(raw + i * 8 + 4) << 32);
                fprintf(stderr, "Linx: WATCH stack[%d]=0x%016" PRIx64 "\n", i, w);
            }
        }
        dump_count++;
    }
    fflush(stderr);

    if (do_abort) {
        cpu_abort(cs, "Linx: watched store");
    }
}

void HELPER(linx_watch_load)(CPULinxState *env, uint64_t pc, uint64_t addr,
                             uint64_t val, uint32_t size)
{
    CPUState *cs = env_cpu(env);
    static int do_abort = -1;

    if (do_abort < 0) {
        const char *mode = getenv("LINX_WATCH_LOAD_MODE");
        do_abort = 1;
        if (mode && mode[0] && strcmp(mode, "0") != 0 &&
            (strcmp(mode, "log") == 0 || strcmp(mode, "LOG") == 0)) {
            do_abort = 0;
        }
    }

    fprintf(stderr,
            "Linx: WATCH load pc=0x%016" PRIx64 " addr=0x%016" PRIx64
            " size=%u val=0x%016" PRIx64 "\n",
            pc, addr, size, val);
    fflush(stderr);

    if (do_abort) {
        cpu_abort(cs, "Linx: watched load");
    }
}

void HELPER(linx_trace_ra)(CPULinxState *env, uint64_t pc, uint32_t what,
                           uint64_t v0, uint64_t v1)
{
    const char *tag = "ra";
    switch (what) {
    case 1:
        tag = "setret";
        break;
    case 2:
        tag = "fentry-save";
        break;
    case 3:
        tag = "fret-restore";
        break;
    case 4:
        tag = "call-commit";
        break;
    default:
        break;
    }

    fprintf(stderr,
            "Linx: TRACE %s pc=0x%016" PRIx64 " sp=0x%016" PRIx64
            " ra=0x%016" PRIx64 " brtype=%u cond=%u carg=0x%08x tgt=0x%016" PRIx64
            " v0=0x%016" PRIx64 " v1=0x%016" PRIx64 "\n",
            tag, pc, env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->brtype & 0x7u, env->cond, env->carg, env->tgt, v0, v1);
    fflush(stderr);
}

void HELPER(linx_trace_reg)(CPULinxState *env, uint64_t pc, uint32_t what,
                            uint32_t reg, uint64_t v0, uint64_t v1)
{
    const char *tag = "reg";
    switch (what) {
    case 1:
        tag = "set";
        break;
    case 2:
        tag = "fentry-save";
        break;
    case 3:
        tag = "fret-restore";
        break;
    default:
        break;
    }

    fprintf(stderr,
            "Linx: TRACE %s r%u pc=0x%016" PRIx64 " sp=0x%016" PRIx64
            " ra=0x%016" PRIx64 " brtype=%u cond=%u carg=0x%08x tgt=0x%016" PRIx64
            " v0=0x%016" PRIx64 " v1=0x%016" PRIx64 "\n",
            tag, reg, pc, env->gpr[LINX_REG_SP], env->gpr[LINX_REG_RA],
            env->brtype & 0x7u, env->cond, env->carg, env->tgt, v0, v1);
    fflush(stderr);
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
