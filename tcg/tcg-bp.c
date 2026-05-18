/*
 * TCG breakpoint support.
 *
 * Copyright (c) 2022 Kenneth Lee
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "exec/exec-all.h"
#include "hw/core/cpu.h"
#include "sysemu/cpus.h"
#include "sysemu/tcg.h"
#include "sysemu/tcg-bp.h"
#include "tcg/tcg.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "sysemu/runstate.h"
enum TCG_BP_TYPE {
    TCG_BPT_PC,         /* break on PC */
    TCG_BPT_PC_COUNT,   /* break after count insn on PC */
    TCG_BPT_DATA,
    TCG_BPT_MAX,        /* not used */
};

static const char *bp_type_str[] = {
    "pc bp",
    "counter bp",
    "data bp",
    "<null>"
};

enum TCG_BP_STATE {
    TCG_BPS_DISABLED,
    TCG_BPS_ENABLED,
    TCG_BPS_ACTIVATED,
};

static const char *bp_state_str[] = {
    "disable",
    "enable",
    "activate",
    "<null>"
};

#define TCG_BP_AT_READ          0x1
#define TCG_BP_AT_WRITE         0x2
#define TCG_BP_AT_ONE_SHOT      0x4
#define TCG_BP_AT_NO_STOP       0x8     /* stop_vm/exit when hit */

struct tcg_bp {
    enum TCG_BP_TYPE type;
    enum TCG_BP_STATE state;
    uint64_t attr;
    uintptr_t ptr;      /* pc or data pointer of the bp */
    int hits;
union {
        struct {
            int count;
            int current_count;
        } bpi_pcc;      /* PC_COUNT info */
        int bpi_dsz;    /* DATA_X info */
    };
};

static inline struct tcg_bp *tcg_bp_new(enum TCG_BP_TYPE type, uintptr_t ptr)
{
    struct tcg_bp *bp = g_new(struct tcg_bp, 1);
    bp->type = type;
    bp->ptr = ptr;
    bp->state = TCG_BPS_ENABLED;
    return bp;
}

bool tcg_bp_add_bp_pc(uintptr_t pc)
{
    CPUState *cpu = current_cpu;
    struct tcg_bp *bp = tcg_bp_new(TCG_BPT_PC, pc);
    g_assert(cpu->stopped);
    g_array_append_val(cpu->bps, bp);
    return true;
}

bool tcg_bp_add_bp_pc_count(uintptr_t pc, int count)
{
    CPUState *cpu = current_cpu;
    struct tcg_bp *bp = tcg_bp_new(TCG_BPT_PC_COUNT, pc);
    g_assert(cpu->stopped);
    bp->bpi_pcc.count = count;
    bp->bpi_pcc.current_count = 0;
    g_array_append_val(cpu->bps, bp);
    return true;
}

static GArray *slist;
static gint slist_sz;

bool tcg_bp_add_bp_event(void)
{
    return false;
}

static bool _fill_bp_by_spec(const char *r, struct tcg_bp *bp)
{
    const char *range_op, *r2, *e;
    uint64_t r1val, r2val;

    enum TCG_BP_TYPE type = TCG_BPT_PC;
    bool done = false;
    uint64_t attr = 0;
    while(!done) {
        switch(r[0]) {
            case 'R':
                type = TCG_BPT_DATA;
                attr |= TCG_BP_AT_READ;
                r++;
                break;

            case 'W':
                type = TCG_BPT_DATA;
                attr |= TCG_BP_AT_WRITE;
                r++;
                break;

            case 'O':
                attr |= TCG_BP_AT_ONE_SHOT;
                r++;
                break;

            case 'N':
                attr |= TCG_BP_AT_NO_STOP;
                r++;
                break;
default:
                done = true;
                break;
        }
    }

    range_op = strstr(r, "+");
    r2 = range_op ? range_op + 1 : NULL;
    if (!range_op) {
        if (qemu_strtou64(r, NULL, 0, &r1val)) {
            error_report("error bp spec: %s\n", r);
            return false;
        }
    } else {

        if (qemu_strtou64(r, &e, 0, &r1val)
            || e != range_op) {
            error_report("Invalid number to the left of %.*s",
                       (int)(r2 - range_op), range_op);
            return false;
        }
        if (qemu_strtou64(r2, NULL, 0, &r2val)) {
            error_report("Invalid number to the right of %.*s",
                       (int)(r2 - range_op), range_op);
            return false;
        }
    }

    bp->type = type;
    bp->ptr = r1val;
    bp->state = TCG_BPS_ENABLED;
    bp->attr = attr;
    bp->hits = 0;
    switch (type) {
        case TCG_BPT_PC:
        case TCG_BPT_PC_COUNT:
            if (range_op) {
                bp->type = TCG_BPT_PC_COUNT;
                bp->bpi_pcc.count = r2val;
                bp->bpi_pcc.current_count = 0;
            }
            break;

        case TCG_BPT_DATA:
            if (range_op)
                bp->bpi_dsz = r2val;
            else
                bp->bpi_dsz = 8; /* default size of 64bit */
            break;

        default:
            g_assert_not_reached();
            break;
    }

    return true;
}

void tcg_bp_set_static_bp(const char *bpspec) {
    gchar **ranges = g_strsplit(bpspec, ",", 0);
    struct tcg_bp bp;
    int i;

    if (slist == NULL) {
        slist = g_array_new(FALSE, FALSE, sizeof(struct tcg_bp));
    }

    for (i = 0; ranges[i]; i++) {
        if(!_fill_bp_by_spec(ranges[i], &bp))
            goto out;
        g_array_append_val(slist, bp);
        slist_sz++;
    }
    g_strfreev(ranges);
    return;

out:
    g_strfreev(ranges);
    exit(-1);
}

GArray *tcg_bps_init(guint *sz)
{
    int i;
    struct tcg_bp bp;

    GArray *bps = g_array_new(FALSE, FALSE, sizeof(struct tcg_bp));
    for (i = 0; i < slist_sz; i++) {
        bp = g_array_index(slist, struct tcg_bp, i);
        //error_report("cpu set %d bp on %lx %d", bp.type, bp.ptr, bp.bpi_dsz);
        g_array_append_val(bps, bp);
        (*sz)++;
    }

    return bps;
}

static void tcg_bp_hit(int cpu_index, struct tcg_bp *bp) {
    bp->hits++;
    qemu_log("cpu%d: %s %lx hit", cpu_index, bp_type_str[bp->type], bp->ptr);
    switch (bp->type) {
        case TCG_BPT_PC_COUNT:
            qemu_log(", accounting on %d/%d  ,trigger count:%d \n", bp->bpi_pcc.current_count,
                    bp->bpi_pcc.count,bp->hits);
            break;

        case TCG_BPT_DATA:
            qemu_log(", see mmu log to address the location ,trigger count:%d \n",
                    bp->hits);
            break;
                        
        case TCG_BPT_PC:
            qemu_log(",trigger count:%d  \n",bp->hits);
            break;

        default:
            g_assert_not_reached();
            break;
    }

    if (bp->attr & TCG_BP_AT_ONE_SHOT) {
        bp->state = TCG_BPS_DISABLED;
    }

    if (!(bp->attr & TCG_BP_AT_NO_STOP)) {
        printf("cpu%d: %s %lx hit\n", cpu_index, bp_type_str[bp->type], bp->ptr);
#ifdef CONFIG_USER_ONLY
        exit(0);
#else
        vm_stop(RUN_STATE_PAUSED);
#endif
    }
}

static void tcg_bp_exec_tb_per_bp(CPUState *cpu, struct tcg_bp *bp,
                                  TranslationBlock *tb)
{
    if (bp->state == TCG_BPS_DISABLED)
        return;

    switch (bp->type) {
        case TCG_BPT_PC:
            if (bp->ptr >= tb->pc && bp->ptr < tb->pc + tb->size)
                tcg_bp_hit(cpu->cpu_index, bp);
            break;

        case TCG_BPT_PC_COUNT:
switch (bp->state) {
                case TCG_BPS_ENABLED:
                    if (bp->ptr >= tb->pc && bp->ptr < tb->pc + tb->size) {
                        qemu_log("cpu%d: bp start count on %lx\n", cpu->cpu_index, bp->ptr);
                        bp->state = TCG_BPS_ACTIVATED;
                    }

                    break;

                case TCG_BPS_ACTIVATED:
                    bp->bpi_pcc.current_count += tb->icount;
                    if (bp->bpi_pcc.current_count >= bp->bpi_pcc.count) {
                        tcg_bp_hit(cpu->cpu_index, bp);
                        bp->bpi_pcc.current_count = 0;
                        bp->state = TCG_BPS_DISABLED;
                    }
                    break;

                default:
                    g_assert_not_reached();
                    break;
            }
            break;

        case TCG_BPT_DATA:
            /* not update for data bp */
            break;

        default:
            g_assert_not_reached();
            break;
    }
}

static inline bool _access_type_match(MMUAccessType access_type, uint64_t attr)
{
    switch (access_type) {
        case MMU_DATA_LOAD:
            return attr & TCG_BP_AT_READ;

        case MMU_DATA_STORE:
            return attr & TCG_BP_AT_WRITE;

        default:
            return false;
    }
}

static void tcg_bp_tbl_fill_per_bp(CPUState *cpu, struct tcg_bp *bp,
                                   target_ulong addr, int size,
                                   MMUAccessType access_type)
{
   if (bp->state == TCG_BPS_DISABLED || bp->type != TCG_BPT_DATA)
        return;

    if ( _access_type_match(access_type, bp->attr) &&
        ((( addr       >= bp->ptr) && ( addr       < (bp->ptr+bp->bpi_dsz))) ||
         (((addr+size) >= bp->ptr) && ((addr+size) < (bp->ptr+bp->bpi_dsz))))) {
        tcg_bp_hit(cpu->cpu_index, bp);
    }
}

void tcg_bp_exec_tb(CPUState *cpu, TranslationBlock *tb)
{
    int i;
    struct tcg_bp *bp;

    for (i = 0; i < cpu->bps_sz; i++) {
        bp = &g_array_index(cpu->bps, struct tcg_bp, i);
        tcg_bp_exec_tb_per_bp(cpu, bp, tb);
    }
}
void tcg_bp_tlb_fill(CPUState *cpu, uint64_t addr, int size,
                     MMUAccessType access_type)
{
    int i;
    struct tcg_bp *bp;

    for (i = 0; i < cpu->bps_sz; i++) {
        bp = &g_array_index(cpu->bps, struct tcg_bp, i);
        tcg_bp_tbl_fill_per_bp(cpu, bp, addr, size, access_type);
    }
}

#define NUM_ATTR 5
static const char *tcg_bp_attr_str(uint64_t attr, gchar str[]) {
    str[0] = attr & TCG_BP_AT_READ ?        'R' : '-';
    str[1] = attr & TCG_BP_AT_WRITE ?       'W' : '-';
    str[2] = attr & TCG_BP_AT_ONE_SHOT ?    'O' : '-';
    str[3] = attr & TCG_BP_AT_NO_STOP ?     'N' : '-';
    str[4] = '\0';
    return str;
}

char *tcg_bp_info(CPUState *cpu, gint i)
{
    gchar buf[NUM_ATTR];
    struct tcg_bp *bp = &g_array_index(cpu->bps, struct tcg_bp, i);
    switch (bp->type) {
        case TCG_BPT_PC:
            return g_strdup_printf("%d: %s(%s): 0x%lx, %s, %d hints\n", i,
                    bp_type_str[bp->type], bp_state_str[bp->state],
                    bp->ptr, tcg_bp_attr_str(bp->attr, buf), bp->hits);
        case TCG_BPT_PC_COUNT:
            return g_strdup_printf("%d: %s(%s): 0x%lx, %s, count=%d/%d, %d hits\n", i,
                    bp_type_str[bp->type], bp_state_str[bp->state], bp->ptr,
                    tcg_bp_attr_str(bp->attr, buf), bp->bpi_pcc.current_count,
                    bp->bpi_pcc.count, bp->hits);
        case TCG_BPT_DATA:
            return g_strdup_printf("%d: %s(%s): 0x%lx+%d, %s, %d hits\n", i,
                    bp_type_str[bp->type], bp_state_str[bp->state],
                    bp->ptr, bp->bpi_dsz, tcg_bp_attr_str(bp->attr, buf), bp->hits);
        default:
            g_assert_not_reached();
    }
}

bool tcg_bp_add(CPUState *cpu, const char *spec)
{
    struct tcg_bp bp;
    bool ret;

     if (!spec)
       return false;

    ret = _fill_bp_by_spec(spec, &bp);
    if (ret) {
        g_array_append_val(cpu->bps, bp);
        cpu->bps_sz++;
    }
return ret;
}

void tcg_bp_remove(CPUState *cpu, gint bp_id)
{
    g_array_remove_index(cpu->bps, bp_id);
    cpu->bps_sz--;
}

void tcg_bp_disable(CPUState *cpu, gint bp_id)
{
    g_array_index(cpu->bps, struct tcg_bp, bp_id).state = TCG_BPS_DISABLED;
}

void tcg_bp_enable(CPUState *cpu, gint bp_id)
{
    struct tcg_bp *bp = &g_array_index(cpu->bps, struct tcg_bp, bp_id);
    bp->state = TCG_BPS_ENABLED;
    if (bp->type == TCG_BPT_PC_COUNT)
        bp->bpi_pcc.current_count = 0;
}
