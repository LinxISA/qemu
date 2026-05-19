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

#ifndef LINX_CPU_H
#define LINX_CPU_H

#include "hw/core/cpu.h"
#include "hw/registerfields.h"
#include "exec/cpu-defs.h"
#include "fpu/softfloat-types.h"
#include "qom/object.h"
#include "cpu_bits.h"

/* v0.50 PRERELEASE FLAG */
#ifndef PRERELEASE
#define PRERELEASE 1
#endif

#define LINX_DEFAULT_TIMEBASE_FREQ 10000000
#define TCG_GUEST_DEFAULT_MO 0

/*
 * Older Linx target code used MO_TEQ for 64-bit target-endian accesses.
 * Current QEMU memop naming spells that form MO_TEUQ.
 */
#ifndef MO_TEQ
#define MO_TEQ MO_TEUQ
#endif

#ifndef MO_Q
#define MO_Q MO_UQ
#endif

#define TYPE_LINX_CPU "linx-cpu"

#define LINX_ILLEGAL_INSTR_ADDR  0xFFFFFFFFFFFFFFFFUL
#define LINX_ILLEGAL_TIDX        0xFFFFFFFF
#define is_valid_linx_addr(addr) ((addr) != LINX_ILLEGAL_INSTR_ADDR)

#define LINX_CPU_TYPE_SUFFIX "-" TYPE_LINX_CPU
#define LINX_CPU_TYPE_NAME(name) (name LINX_CPU_TYPE_SUFFIX)
#define CPU_RESOLVING_TYPE TYPE_LINX_CPU

#define TYPE_LINX_CPU_ANY              LINX_CPU_TYPE_NAME("any")

#define TYPE_LINX_CPU_BASE            TYPE_LINX_CPU_ANY

typedef enum {
    READ,
    WRITE,
} MemType;

/* todo: maybe we do not need this flag as we alway have MMU */
enum {
    LINX_FEATURE_MMU,
};

enum {
    TRANSLATE_SUCCESS,
    TRANSLATE_FAIL,
};

#define MMU_USER_IDX 3

typedef struct CPUArchState CPULINXState;
typedef struct ArchCPU LINXCPU;

#define LINX_EBSTATE_SIZE 64
#define LINX_ACR_NUM  2

#define NUM_GTIMERS 2
#define GTIMER_ACR0 0
#define GTIMER_ACR1 1

struct LinxBArg {
    /* part of BARG */
    uint64_t bpc;
    uint32_t lra_offset;
    uint8_t  block_type;
    uint8_t  branch_type;
    uint8_t  taken;
    uint8_t  aq, rl;
    uint8_t  reg_dst0;
    uint8_t  reg_dst1;
    uint8_t  reg_dst2;
    uint8_t  reg_dst3;
    /* part of TPC */
    uint64_t tpc;
    /* part of lpr */
    uint64_t blk_t, blk_u;
    uint64_t ri, ro;
    uint64_t pred;
    uint64_t lb, lc;
    uint64_t vt[CPU_NB_LANE_NUM];
    uint64_t vu[CPU_NB_LANE_NUM];
    uint64_t vm[CPU_NB_LANE_NUM];
    uint64_t vn[CPU_NB_LANE_NUM];
    uint8_t output_tile[TILE_REG_MEM];
};

typedef struct LinxSYSReg {
    uint64_t ecstate;
    uint64_t evbase;
    uint64_t ecause;
    uint64_t earg0;
    uint64_t etemp;
    uint64_t futo;
    uint64_t econfig;
    uint64_t ipending;
    uint64_t topei;
    uint64_t eoiei;
    uint64_t ebpc;
    uint64_t etpc;
    uint64_t ebpcn;
    uint64_t ebarg;
    uint64_t mmconfig;
    uint64_t mmtbase;
    uint64_t time;
    uint64_t timecmp;
    uint64_t xbinfo;
    uint64_t acr_param;
    uint64_t elpr[17];
#define EBSTATE_MAX_VALID_REG 46
} LinxSYSReg;

typedef struct TileAttr {
    uint32_t size;
    uint32_t dtyp;
} TileAttr;

struct TILEOPInfo {
#define IOTI_IMME_MASK 0b100000
#define IOTI_DATA_SIZE_MASK 0b11111
    uint32_t tileop_type;
    uint32_t tileop_datatype;
};

struct CPUArchState {
    target_ulong gpr[GPR_REG_SIZE];

    target_ulong pc;

    target_ulong badaddr;
    target_ulong badssr;
    uint32_t bins;

    /* todo: cstate has priv information, so let's merge priv into cstate later */
    uint64_t cstate;
    LinxSYSReg sysreg[LINX_ACR_NUM];
    uint32_t features;

#ifdef CONFIG_USER_ONLY
    uint32_t elf_flags;
#endif

#ifndef CONFIG_USER_ONLY
    target_ulong priv;
    target_ulong resetvec;

    target_ulong lxlcid;
    target_ulong vendor;
    target_ulong version;
    target_ulong capa;
    target_ulong capa_en;
    /* todo: currently we use rv mmu, should be replaced later */
    target_ulong satp;   /* since: priv-1.10.0 */

   /* machine specific rdtime callback */
    uint64_t (*rdtime_fn)(uint32_t);
    uint32_t rdtime_fn_arg;

    /* True if in debugger mode.  */
    bool debugger;
#endif
    float_status fp_status;
    QEMUTimer *gt_timer[NUM_GTIMERS]; /* Internal timer */

    /* BlockISA state */
    /* body start address using for decouple block and in-block jump inst */
    uint64_t tpc1;
    uint64_t tpc_s;
    uint32_t in_body;
    /* block header address */
    uint64_t bpc;
    uint64_t next_bpc;
    /* Temporary PC, address of current microinstruction */
    uint64_t tpc;
    uint64_t csr_lc[3];
    uint64_t csr_lb[3];
    uint64_t csr_lc_sum;
    uint64_t csr_lb_sum;
    uint64_t enable_lane_num;
    uint64_t csr_lpcb[3];
    uint64_t csr_lpce[3];
    target_ulong csr_lanenum;
    target_ulong predm;
    /* Control Stack using for the SIMT when the branch diverge */
    GQueue *cstk;
    /*
     * For the block LMCOPY/MLCOPY/LLCOPY:
     * tm_ext[4:0]: TransFormMode
     * tm_ext[16:5]: CopySize / ResetSize
     *
     * For the block MATMUL:
     * tm_ext[4:0]: RegSrc3
     * tm_ext[9:5]: RegSrc4
     * tm_ext[16:10]: MatMulType
     */
    uint64_t tm_ext;
    uint32_t t_idx;
    uint32_t u_idx;
    uint32_t ri_idx;
    uint32_t ro_idx;
    uint32_t blk_ri[RI_SIZE];
    uint32_t blk_ro[RO_SIZE];

    uint32_t tile_reg_t_idx;
    uint32_t tile_reg_u_idx;
    uint32_t tile_reg_m_idx;
    uint32_t tile_reg_n_idx;
    uint64_t *tile_reg_t[TILE_REG_SIZE];
    uint64_t *tile_reg_u[TILE_REG_SIZE];
    uint64_t *tile_reg_m[TILE_REG_SIZE];
    uint64_t *tile_reg_n[TILE_REG_SIZE];
    TileAttr  tile_attr_t[TILE_REG_SIZE];
    TileAttr  tile_attr_u[TILE_REG_SIZE];
    TileAttr  tile_attr_m[TILE_REG_SIZE];
    TileAttr  tile_attr_n[TILE_REG_SIZE];
    uint64_t *acc;
    /* stack space register */
    uint64_t *s;
    SrcType acc_data_typ;
    uint64_t ta, tb, tc, td, te, tf, tg, th;
    uint64_t to, to1, to2, to3, to4, to5, to6, to7;
    TileAttr ta_a, tb_a, tc_a, td_a, te_a,
             tf_a, tg_a, th_a;
    uint32_t dsttile[MAX_DST_TILE_NUM];
    TileAttr tile_attr[MAX_DST_TILE_NUM];

    /* bnext offset, indicates the next block header offsets. */
    int64_t bnext;
    uint32_t scall_arg;

#define HEADER_INFO_BATTR_START    0
#define HEADER_INFO_BATTR_LEN      8
#define HEADER_INFO_ATOMIC_START   4
#define HEADER_INFO_ATOMIC_LEN     4
#define HEADER_INFO_BRHTYPE_START  8
#define HEADER_INFO_BRHTYPE_LEN    3
#define HEADER_INFO_BLKTYPE_START  11
#define HEADER_INFO_BLKTYPE_LEN    5
#define HEADER_INFO_DECOUPLE_START 16
#define HEADER_INFO_DECOUPLE_LEN   1
#define HEADER_INFO_FORMAT_START   17
#define HEADER_INFO_FORMAT_LEN     5
#define HEADER_INFO_SRCTYP_START   22
#define HEADER_INFO_SRCTYP_LEN     5
#define HEADER_INFO_PAD_START      27
#define HEADER_INFO_PAD_LEN        3
#define HEADER_INFO_CM_START       30
#define HEADER_INFO_CM_LEN         2
#define HEADER_INFO_CANON_START    32
#define HEADER_INFO_CANON_LEN      1
#define HEADER_INFO_RMODE_START    33
#define HEADER_INFO_RMODE_LEN      5
#define HEADER_INFO_SAT_START      38
#define HEADER_INFO_SAT_LEN        1

#define HEADER_INFO_DR_MASK         0x4
#define HEADER_INFO_FIXUP_MASK      0x8
#define HEADER_INFO_AQ_MASK         0x20
#define HEADER_INFO_RL_MASK         0x10
#define HEADER_INFO_BRHTYPE_MASK    0x700
#define HEADER_INFO_BLKTYPE_MASK    0xF800

    /*
     * header_info[0:7]   => battr:{far,atom,aq,rl,fixup,H,R,T}
     * header_info[8:10]  => branch type
     * header_info[11:15] => block type
     * header_info[16:16] => decouple
     * header_info[17:21] => format
     */
    uint64_t header_info;

    uint32_t tile_reg_dst_num;
    uint32_t tile_reg_src_num;
    target_ulong need_combine_lbref;
    target_ulong is_relay;

    uint64_t blk_t[T_REG_SIZE];
    uint64_t blk_u[U_REG_SIZE];

    uint64_t fvec_t[CPU_NB_LANE_NUM][FVEC_REG_SIZE] QEMU_ALIGNED(16);
    uint64_t fvec_u[CPU_NB_LANE_NUM][FVEC_REG_SIZE] QEMU_ALIGNED(16);
    uint64_t fvec_m[CPU_NB_LANE_NUM][FVEC_REG_SIZE] QEMU_ALIGNED(16);
    uint64_t fvec_n[CPU_NB_LANE_NUM][FVEC_REG_SIZE] QEMU_ALIGNED(16);

    uint64_t fvec_tumn_width[CPU_NB_LANE_NUM];
    uint64_t fvec_tumn_valid[CPU_NB_LANE_NUM];

    uint64_t csr_tp;
    uint64_t csr_gp;
    uint64_t csr_cw;
    uint64_t csr_tr1;
    uint64_t csr_tr2;
    uint64_t csr_fssr;

    /*
     * 64 bytes CPU internal buffer, stores in atomic block should write
     * here firstly, write data in it to memory system when atomic block
     * commit. store_addr is the base of aligned 64 Bytes. store_addr_valid 1
     * mean store_addr is valid, 0 means invalid.
     */
    uint64_t store_buf[8];
    uint64_t store_addr;
    uint64_t store_addr_valid;

    /* we create this to avoid mixing with riscv lr/sc */
    target_ulong linx_load_res;
    target_ulong linx_load_val;

    // interrupt preemption threshold, acknowledge register
    target_ulong threshold;
    target_ulong ack;
    struct LinxBArg barg;
    struct TILEOPInfo tileop_info;
    struct TILEOPInfo tileop_s;
    target_ulong carg_tgt;
    target_ulong carg_flag;
};

OBJECT_DECLARE_CPU_TYPE(LINXCPU, LINXCPUClass, LINX_CPU)

/**
 * LINXCPUClass:
 * @parent_realize: The parent class' realize handler.
 * @parent_reset: The parent class' reset handler.
 *
 * A LINX CPU model.
 */
struct LINXCPUClass {
    /*< private >*/
    CPUClass parent_class;
    /*< public >*/
    DeviceRealize parent_realize;
    DeviceReset parent_reset;
};

/**
 * LINXCPU:
 * @env: #CPULINXState
 *
 * A LINX CPU.
 */
struct ArchCPU {
    /*< private >*/
    CPUState parent_obj;
    /*< public >*/
    CPUNegativeOffsetState neg;
    CPULINXState env;

    char *dyn_csr_xml;

    /* Configuration Settings */
    struct {
        bool mmu;
        uint64_t resetvec;
    } cfg;

    /* GPIO output for generic timer */
    qemu_irq gt_timer_output[NUM_GTIMERS];
};

static inline int linx_has_ext(CPULINXState *env, target_ulong ext)
{
    /* todo: add extension description */
    return false;
}

static inline bool linx_feature(CPULINXState *env, int feature)
{
    return env->features & (1ULL << feature);
}

static inline bool linx_is_atomic_blk(uint32_t block_atomic)
{
    return !!(block_atomic & BLK_ATOMIC);
}
#include "cpu_user.h"
#include "hw/linx/linx_hart.h"

extern const char * const blk_ri_regnames[];
extern const char * const blk_ro_regnames[];
extern const char * const linx_int_regnames[];
extern const char * const linx_int_local_regnames[];
extern const char * const linx_int_blk_tnames[];
extern const char * const linx_int_blk_unames[];
extern const char * const linx_lbnames[];
extern const char * const linx_lcnames[];
extern const char * const linx_fvec_tnames[CPU_NB_LANE_NUM][FVEC_REG_SIZE];
extern const char * const linx_fvec_unames[CPU_NB_LANE_NUM][FVEC_REG_SIZE];
extern const char * const linx_fvec_mnames[CPU_NB_LANE_NUM][FVEC_REG_SIZE];
extern const char * const linx_fvec_nnames[CPU_NB_LANE_NUM][FVEC_REG_SIZE];
extern const char * const linx_brhtype_names[];

const char *linx_cpu_get_trap_name(target_ulong cause, bool async);
void linx_cpu_do_interrupt(CPUState *cpu);
int linx_cpu_write_elf_note(WriteCoreDumpFunction f, CPUState *cs,
                            int cpuid, void *opaque);
int linx_cpu_gdb_read_register(CPUState *cpu, GByteArray *buf, int reg);
int linx_cpu_gdb_write_register(CPUState *cpu, uint8_t *buf, int reg);
int linx_cpu_mmu_index(CPULINXState *env, bool ifetch);
hwaddr linx_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr);
void  linx_cpu_do_unaligned_access(CPUState *cs, vaddr addr,
                                    MMUAccessType access_type, int mmu_idx,
                                    uintptr_t retaddr) QEMU_NORETURN;
bool linx_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr);
void linx_cpu_do_transaction_failed(CPUState *cs, hwaddr physaddr,
                                     vaddr addr, unsigned size,
                                     MMUAccessType access_type,
                                     int mmu_idx, MemTxAttrs attrs,
                                     MemTxResult response, uintptr_t retaddr);
void linx_cpu_list(void);

#define cpu_list linx_cpu_list
#define cpu_mmu_index linx_cpu_mmu_index

#ifndef CONFIG_USER_ONLY
bool linx_cpu_exec_interrupt(CPUState *cs, int interrupt_request);
uint32_t linx_cpu_update_ipending(LINXCPU *cpu, uint32_t mask,
                                   uint32_t value, uint32_t acr_num);
#define BOOL_TO_MASK(x) (-!!(x)) /* helper for linx_cpu_update_ipending value */
void linx_cpu_set_rdtime_fn(CPULINXState *env, uint64_t (*fn)(uint32_t),
                             uint32_t arg);
#endif
void linx_cpu_set_mode(CPULINXState *env, target_ulong newpriv);
void linx_translate_init(void);
void QEMU_NORETURN linx_raise_exception(CPULINXState *env,
                                         uint32_t exception, uintptr_t pc);
void linx_reset_bstate(CPULINXState *env);
void linx_reset_bstate_short(CPULINXState *env);
void linx_save_bstate(CPULINXState *env, target_ulong handle_acr);
void linx_save_bstate_layer1(CPULINXState *env, target_ulong handle_acr);
void linx_recovery_bstate_by_ebstate(CPULINXState *env, target_ulong priv);
void linx_cs_log(CPULINXState *env);

#define TB_FLAGS_PRIV_MMU_MASK                3
#define TB_FLAGS_PRIV_HYP_ACCESS_MASK   (1 << 2)

#include "exec/cpu-all.h"

FIELD(TB_FLAGS, MEM_IDX, 0, 3)
FIELD(TB_FLAGS, TIDX_INDEX, 3, 2)
FIELD(TB_FLAGS, UIDX_INDEX, 5, 2)

FIELD(TB_FLAGS, FVEC_TIDX_INDEX, 7, 2)
FIELD(TB_FLAGS, FVEC_UIDX_INDEX, 9, 2)
FIELD(TB_FLAGS, FVEC_MIDX_INDEX, 11, 2)
FIELD(TB_FLAGS, FVEC_NIDX_INDEX, 13, 2)

/* --- GQM implementation part --- */
FIELD(GQM_META_DATA, LEN, 0, 10)
FIELD(GQM_META_DATA, SPACE_STATUS, 10, 2)
FIELD(GQM_META_DATA, QUEUE_STATUS, 12, 1)
FIELD(GQM_META_DATA, HEAD, 13, 10)
FIELD(GQM_META_DATA, TAIL, 23, 10)

FIELD(GQM_QMT_RESULT, REMAIN, 0, 10)
FIELD(GQM_QMT_RESULT, STATUS, 10, 1)
FIELD(GQM_QMT_RESULT, ERRNO, 62, 2)

FIELD(GQM_QPUSH_RESULT, REMAIN, 0, 10)
FIELD(GQM_QPUSH_RESULT, ERRNO, 62, 2)

FIELD(GQM_QPOP_RESULT, REMAIN, 0, 10)
FIELD(GQM_QPOP_RESULT, ERRNO, 62, 2)

enum gqm_space_status {
    GQM_SPACE_STATUS_EMPTY = 0,
    GQM_SPACE_STATUS_NORMAL,
    GQM_SPACE_STATUS_FULL,
    GQM_SPACE_STATUS_MAX
};

enum gqm_queue_status {
    GQM_QUEUE_STATUS_OPEN = 0,
    GQM_QUEUE_STATUS_CLOSE,
    GQM_QUEUE_STATUS_MAX,
};

#define GQM_META_DATA_BYTE      8
#define GQM_ENTRY_BYTE          8

#define GQM_QMT_RET_ERRNO_EXEC_SUCCESS      0
#define GQM_QMT_RET_ERRNO_DATA_CORRUPTION   1
#define GQM_QMT_RET_ERRNO_INCORRECT_PARA    2

#define GQM_QPUSH_RET_ERRNO_EXEC_SUCCESS    0
#define GQM_QPUSH_RET_ERRNO_QUEUE_FULL      1
#define GQM_QPUSH_RET_ERRNO_DATA_CORRUPTION 2

#define GQM_QPOP_RET_ERRNO_EXEC_SUCCESS     0
#define GQM_QPOP_RET_ERRNO_QUEUE_EMPTY      1
#define GQM_QPOP_RET_ERRNO_DATA_CORRUPTION  2

FIELD(GQM_CMD, I, 0, 1)
FIELD(GQM_CMD, E, 1, 1)
FIELD(GQM_CMD, S, 2, 1)
FIELD(GQM_CMD, R, 3, 1)
FIELD(GQM_CMD, H, 4, 1)

void cpu_get_tb_cpu_state(CPULINXState *env, target_ulong *pc,
                          target_ulong *cs_base, uint32_t *pflags);

LINXException linx_csrrw(CPULINXState *env, int csrno,
                           target_ulong *ret_value,
                           target_ulong new_value, target_ulong write_mask);
LINXException linx_csrrw_debug(CPULINXState *env, int csrno,
                                 target_ulong *ret_value,
                                 target_ulong new_value,
                                 target_ulong write_mask);

static inline void linx_csr_write(CPULINXState *env, int csrno,
                                   target_ulong val)
{
    linx_csrrw(env, csrno, NULL, val, MAKE_64BIT_MASK(0, TARGET_LONG_BITS));
}

static inline target_ulong linx_csr_read(CPULINXState *env, int csrno)
{
    target_ulong val = 0;
    linx_csrrw(env, csrno, &val, 0, 0);
    return val;
}

typedef LINXException (*linx_csr_predicate_fn)(CPULINXState *env,
                                                 int csrno);
typedef LINXException (*linx_csr_read_fn)(CPULINXState *env, int csrno,
                                            target_ulong *ret_value);
typedef LINXException (*linx_csr_write_fn)(CPULINXState *env, int csrno,
                                             target_ulong new_value);
typedef LINXException (*linx_csr_op_fn)(CPULINXState *env, int csrno,
                                          target_ulong *ret_value,
                                          target_ulong new_value,
                                          target_ulong write_mask);

typedef struct {
    const char *name;
    linx_csr_predicate_fn predicate;
    linx_csr_read_fn read;
    linx_csr_write_fn write;
    linx_csr_op_fn op;
} linx_csr_operations;

/* CSR function table constants */
enum {
    SSR_TABLE_SIZE = 0xffff
};

/* CSR function table */
extern linx_csr_operations csr_ops[SSR_TABLE_SIZE];

uint64_t get_dr(uint64_t header_info);
uint64_t get_rl(uint64_t header_info);
uint64_t get_aq(uint64_t header_info);
uint64_t get_blktype(uint64_t header_info);
uint64_t get_brhtype(uint64_t header_info);
uint64_t get_blkdcp(uint64_t header_info);
uint64_t get_blk_atomic(uint64_t header_info);

#ifndef CONFIG_USER_ONLY
target_ulong read_ebstate(CPULINXState *env, target_ulong base, int num);
void write_ebstate(CPULINXState *env, target_ulong base, int num,
                   target_ulong data);
#endif

void linx_cpu_register_gdb_regs_for_features(CPUState *cs);

bool check_acr_request(CPULINXState *env, int aacr, int target_acr);
bool check_acr_enter(CPULINXState *env, int aacr, int target_acr);

/* is mask check error first happened? */
bool is_first_happened(uint64_t key);

#endif /* LINX_CPU_H */
