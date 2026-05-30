/*
 * QEMU BlockISA Disassembler
 *
 * Copyright by Hisilicon Tech. Co. Ltd. 2023.
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
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "disas/dis-asm.h"
#include "target/linx/linx_block_def.h"
#include "target/linx/cpu_bits.h"
#include "cpu.h"

/* types */
#define INST_TYPE_AU 0
#define INST_TYPE_LU 1
#define INST_TYPE_FP 2
#define INSTR_TYPE_CMP_SETC_LU  3
#define INSTR_TYPE_CMP_SETC  4

#define U_IMM 0
#define S_IMM 1

#define SB 0
#define SH 1
#define SW 2
#define SD 3

#define HEAD_C_LBREF_OPCODE  0b1100
#define HEAD_LBREF_OPCODE    0b0011001
#define LIEXT_1_OPCODE       0b1110111
#define LIEXT_2_OPCODE       0b1111111
#define SIMT_HEAD_OPCODE     0b110000001
#define C_SIMT_HEAD_OPCODE   0b110010000

#define FCVT_TYPE            0
#define UCVT_TYPE            8
#define SCVT_TYPE            12

/* structures */

typedef struct {
    disassemble_info *info;
    uint64_t pc;
    uint32_t insn;
} DisasContext;

/* register names */
static const char linx_src_reg_name[32][5] = {
    "zero", "sp",  "a0",  "a1",  "a2",  "a3",  "a4",  "a5",
    "a6",   "a7",  "ra",  "s0",  "s1",  "s2",  "s3",  "s4",
    "s5",   "s6",  "s7",  "s8",  "x0",  "x1",  "x2",  "x3",
    "t#1",  "t#2", "t#3", "t#4", "u#1",  "u#2", "u#3", "u#4",
};

static const char linx_dest_reg_name[32][8] = {
    "",        ", ->sp",  ", ->a0",  ", ->a1",
    ", ->a2",  ", ->a3",  ", ->a4",  ", ->a5",
    ", ->a6",  ", ->a7",  ", ->ra",  ", ->s0",
    ", ->s1",  ", ->s2",  ", ->s3",  ", ->s4",
    ", ->s5",  ", ->s6",  ", ->s7",  ", ->s8",
    ", ->x0",  ", ->x1",  ", ->x2",  ", ->x3",
    "", "", "", "", "", "", ", ->u", ", ->t",
};

static const char linx_dest_reg1_name[32][6] = {
    "",      ", sp",  ", a0",  ", a1",
    ", a2",  ", a3",  ", a4",  ", a5",
    ", a6",  ", a7",  ", ra",  ", s0",
    ", s1",  ", s2",  ", s3",  ", s4",
    ", s5",  ", s6",  ", s7",  ", s8",
    ", x0",  ", x1",  ", x2",  ", x3",
    ", tx2", ", ux2", ", tx4", ", ux4",
    "",      "",      ", u",   ", t",
};

static inline const char *get_src_reg_name(int reg_enc)
{
    return linx_src_reg_name[reg_enc];
}

static inline const char *get_dest_reg_name(int reg_enc)
{
    return linx_dest_reg_name[reg_enc];
}

static inline const char *get_dest_reg1_name(int reg_enc)
{
    return linx_dest_reg1_name[reg_enc];
}

static const char srctype_au[4][5] = { ".sw", ".uw", ".neg", "" };
static const char srctype_lu[4][5] = { ".sw", ".uw", ".not", "" };
static const char srctype_cmp_sect_lu[4][5] = { ".sw", ".uw", "", "" };
static const char srctype_cmp_sect[4][5] = { ".sw", ".uw", "", "" };


static const char cachetype_prf[4][5] = { ".l1", ".l2", ".l3" , ""};


static const char fence_mask_str[16][5] = {
    "NONE", "W",  "R",  "RW",  "O",  "OW",  "OR",  "ORW",
    "I",    "IW", "IR", "IRW", "IO", "IOW", "IOR", "IORW"
};

static const char *csr_name(int csrno)
{
    switch (csrno) {
    case CSR_TP: return "tp";
    case CSR_GP: return "gp";
    case CSR_CW: return "cw";
    case CSR_SCRATCH0: return "scratch0";
    case CSR_SCRATCH1: return "scratch1";
    case CSR_SCRATCH2: return "scratch2";
    case CSR_SCRATCH3: return "scratch3";
    case CSR_SCRATCH4: return "scratch4";
    case CSR_SCRATCH5: return "scratch5";
    case CSR_SCRATCH6: return "scratch6";
    case CSR_SCRATCH7: return "scratch7";
    case CSR_SCRATCH8: return "scratch8";
    case CSR_SCRATCH9: return "scratch9";
    case CSR_SCRATCH10: return "scratch10";
    case CSR_SCRATCH11: return "scratch11";
    case CSR_SCRATCH12: return "scratch12";
    case CSR_SCRATCH13: return "scratch13";
    case CSR_SCRATCH14: return "scratch14";
    case CSR_SCRATCH15: return "scratch15";
    case CSR_SCRATCH16: return "scratch16";
    case CSR_SCRATCH17: return "scratch17";
    case CSR_SCRATCH18: return "scratch18";
    case CSTATE: return "cstate";
    case LXLCID: return "lxlcid";
    case A0_ECSTATE: return "a0_ecstate";
    case A0_EVBASE: return "a0_evbase";
    case A0_ECAUSE: return "a0_ecause";
    case A0_EARG0: return "a0_earg0";
    case A0_ETEMP: return "a0_etemp";
    case A0_FUTO: return "a0_futo";
    case A0_IENABLE: return "a0_econfig";
    case A0_IPENDING: return "a0_ipending";
    case A0_TOPEI: return "a0_topei";
    case A0_EOIEI: return "a0_eoiei";
    case A0_EBPC: return "a0_ebpc";
    case A0_EBARG: return "a0_ebarg";
    case A0_ETPC: return "a0_etpc";
    case A0_MMTBASE: return "a0_mmtbase";
    case A0_MMCONFIG: return "a0_mmconfig";
    case A0_TIME: return "a0_time";
    case A0_TIMECMP: return "a0_timecmp";
    case A0_EBARG0: return "a0_ebarg0";
    case A0_EBARG_BPC_CUR: return "a0_ebarg_bpc_cur";
    case A0_EBARG_BPC_TGT: return "a0_ebarg_bpc_tgt";
    case A0_EBARG_TPC: return "a0_ebarg_tpc";
    case A0_EBARG_LRA: return "a0_ebarg_lra";
    case A0_EBARG_TQ0: return "a0_ebarg_tq0";
    case A0_EBARG_TQ1: return "a0_ebarg_tq1";
    case A0_EBARG_TQ2: return "a0_ebarg_tq2";
    case A0_EBARG_TQ3: return "a0_ebarg_tq3";
    case A0_EBARG_UQ0: return "a0_ebarg_uq0";
    case A0_EBARG_UQ1: return "a0_ebarg_uq1";
    case A0_EBARG_UQ2: return "a0_ebarg_uq2";
    case A0_EBARG_UQ3: return "a0_ebarg_uq3";
    case A0_EBARG_LB: return "a0_ebarg_lb";
    case A0_EBARG_LC: return "a0_ebarg_lc";
    case A0_EBARG_EXTCTX_PTR: return "a0_ebarg_extctx_ptr";
    case A0_EBARG_EXTCTX_META: return "a0_ebarg_extctx_meta";
    case A0_EBARG_TPLFLAGS: return "a0_ebarg_tplflags";
    case A0_EBSTATE_EXT0: return "a0_ebstate_ext0";
    case A0_EBSTATE_EXT1: return "a0_ebstate_ext1";
    case A0_EBSTATE_EXT2: return "a0_ebstate_ext2";
    case A0_EBSTATE_EXT3: return "a0_ebstate_ext3";
    case A0_EBSTATE_EXT4: return "a0_ebstate_ext4";
    case A0_EBSTATE_EXT5: return "a0_ebstate_ext5";
    case A0_EBSTATE_EXT6: return "a0_ebstate_ext6";
    case A0_EBSTATE_EXT7: return "a0_ebstate_ext7";
    case A0_EBSTATE_EXT8: return "a0_ebstate_ext8";
    case A0_EBSTATE_EXT9: return "a0_ebstate_ext9";
    case A0_EBSTATE_EXT10: return "a0_ebstate_ext10";
    case A0_EBSTATE_EXT11: return "a0_ebstate_ext11";
    case A0_EBSTATE_EXT12: return "a0_ebstate_ext12";
    case A0_EBSTATE_EXT13: return "a0_ebstate_ext13";
    case A1_ECSTATE: return "a1_ecstate";
    case A1_EVBASE: return "a1_evbase";
    case A1_ECAUSE: return "a1_ecause";
    case A1_EARG0: return "a1_earg0";
    case A1_ETEMP: return "a1_etemp";
    case A1_FUTO: return "a1_futo";
    case A1_IENABLE: return "a1_econfig";
    case A1_IPENDING: return "a1_ipending";
    case A1_TOPEI: return "a1_topei";
    case A1_EOIEI: return "a1_eoiei";
    case A1_EBPC: return "a1_ebpc";
    case A1_EBARG: return "a1_ebarg";
    case A1_ETPC: return "a1_etpc";
    case A1_MMTBASE: return "a1_mmtbase";
    case A1_MMCONFIG: return "a1_mmconfig";
    case A1_TIME: return "a1_time";
    case A1_TIMECMP: return "a1_timecmp";
    case A1_EBARG0: return "a1_ebarg0";
    case A1_EBARG_BPC_CUR: return "a1_ebarg_bpc_cur";
    case A1_EBARG_BPC_TGT: return "a1_ebarg_bpc_tgt";
    case A1_EBARG_TPC: return "a1_ebarg_tpc";
    case A1_EBARG_LRA: return "a1_ebarg_lra";
    case A1_EBARG_TQ0: return "a1_ebarg_tq0";
    case A1_EBARG_TQ1: return "a1_ebarg_tq1";
    case A1_EBARG_TQ2: return "a1_ebarg_tq2";
    case A1_EBARG_TQ3: return "a1_ebarg_tq3";
    case A1_EBARG_UQ0: return "a1_ebarg_uq0";
    case A1_EBARG_UQ1: return "a1_ebarg_uq1";
    case A1_EBARG_UQ2: return "a1_ebarg_uq2";
    case A1_EBARG_UQ3: return "a1_ebarg_uq3";
    case A1_EBARG_LB: return "a1_ebarg_lb";
    case A1_EBARG_LC: return "a1_ebarg_lc";
    case A1_EBARG_EXTCTX_PTR: return "a1_ebarg_extctx_ptr";
    case A1_EBARG_EXTCTX_META: return "a1_ebarg_extctx_meta";
    case A1_EBARG_TPLFLAGS: return "a1_ebarg_tplflags";
    case A1_EBSTATE_EXT0: return "a1_ebstate_ext0";
    case A1_EBSTATE_EXT1: return "a1_ebstate_ext1";
    case A1_EBSTATE_EXT2: return "a1_ebstate_ext2";
    case A1_EBSTATE_EXT3: return "a1_ebstate_ext3";
    case A1_EBSTATE_EXT4: return "a1_ebstate_ext4";
    case A1_EBSTATE_EXT5: return "a1_ebstate_ext5";
    case A1_EBSTATE_EXT6: return "a1_ebstate_ext6";
    case A1_EBSTATE_EXT7: return "a1_ebstate_ext7";
    case A1_EBSTATE_EXT8: return "a1_ebstate_ext8";
    case A1_EBSTATE_EXT9: return "a1_ebstate_ext9";
    case A1_EBSTATE_EXT10: return "a1_ebstate_ext10";
    case A1_EBSTATE_EXT11: return "a1_ebstate_ext11";
    case A1_EBSTATE_EXT12: return "a1_ebstate_ext12";
    case A1_EBSTATE_EXT13: return "a1_ebstate_ext13";
    default: return "unkown";
    }
}

/* SIMT I/O code */
#define SRC_LEN 16
#define DSR_LEN 16

static const char dep_src_name[32][4] = {
    "", "D#1", "D#2", "D#3", "D#4", "D#5", "D#6", "D#7", "D#8",
};
static const char dep_dst_name[32][6] = {
    "", ", ->D",
};

/* Tile Register name */
static const char tile_reg_name[64][4] = {
    "t#1",  "t#2", "t#3", "t#4", "t#5",  "t#6", "t#7", "t#8",
    "t#9",  "t#10", "t#11", "t#12", "t#13",  "t#14", "t#15", "t#16",
    "u#1",  "u#2", "u#3", "u#4", "u#5",  "u#6", "u#7", "u#8",
    "u#9",  "u#10", "u#11", "u#12", "u#13",  "u#14", "u#15", "u#16",
    "m#1",  "m#2", "m#3", "m#4", "m#5",  "m#6", "m#7", "m#8",
    "m#9",  "m#10", "m#11", "m#12", "m#13",  "m#14", "m#15", "m#16",
    "n#1",  "n#2", "n#3", "n#4", "n#5",  "n#6", "n#7", "n#8",
    "n#9",  "n#10", "n#11", "n#12", "n#13",  "n#14", "n#15", "n#16",
};
static const char tile_reg_dst_name[5][8] = {
    ", ->t", ", ->u", ", ->m", ", ->n", ", ->acc"
};

/* Block type names */
static const char fvec_src_vt[4][5] = {
    "vt#1",  "vt#2", "vt#3", "vt#4"
};

static const char fvec_src_vu[4][5] = {
    "vu#1",  "vu#2", "vu#3", "vu#4"
};

static const char fvec_src_vm[4][5] = {
    "vm#1",  "vm#2", "vm#3", "vm#4"
};

static const char fvec_src_vn[4][5] = {
    "vn#1",  "vn#2", "vn#3", "vn#4"
};

static const char fvec_src_vt_reuse[4][11] = {
    "vt#1.reuse", "vt#2.reuse", "vt#3.reuse", "vt#4.reuse"
};

static const char fvec_src_vu_reuse[4][11] = {
    "vu#1.reuse", "vu#2.reuse", "vu#3.reuse", "vu#4.reuse"
};

static const char fvec_src_vm_reuse[4][11] = {
    "vm#1.reuse", "vm#2.reuse", "vm#3.reuse", "vm#4.reuse"
};

static const char fvec_src_vn_reuse[4][11] = {
    "vn#1.reuse", "vn#2.reuse", "vn#3.reuse", "vn#4.reuse"
};

static const char fvec_ri_reg_name[12][5] = {
    "ri0", "ri1", "ri2", "ri3", "ri4", "ri5", "ri6", "ri7",
    "ri8", "ri9", "ri10", "ri11",
};

static const char srctype_fp[8][5] = {
    ".fd", ".fs", ".fh", ".fb", "", "", "", ""
};

static const char fvec_cvt_fp[32][6] = {
    ".fd", ".fs", ".tf32", ".hf32", ".fh", ".bf", ".hif8",
    ".fb", ".e5m2", ".e3m2", ".e2m3", ".e2m1", ".e1m2", ".e8m0", ".hif4",
};

static const char fvec_cvt_int[32][4] = {
    ".ud", ".uw", ".uh", ".ub", ".u4", "", "", "",
    ".sd", ".sw", ".sh", ".sb", ".s4",
};

static const char cvt_tp[16][3] = {
    "fd", "fs", "fh", "fb", "", "", "", "",
    "ud", "uw", "uh", "ub", "sd", "sw", "sh", "sb"
};

static const char src_reg_type[8][4] = {".ud", ".uw", ".uh", ".ub",
                                        ".sd", ".sw", ".sh", ".sb"};

static const char dst_reg_type[4][3] = {".d", ".w", ".h", ".b"};

static const char fvec_ldst_reg_name[4][7] = {
    ", ->vt", ", ->vu", ", ->vm", ", ->vn"
};

static const char fvec_ro_reg_name[4][8] = {
    ", ->ro0", ", ->ro1", ", ->ro2", ", ->ro3",
};

static const char acrc_request_type[3][8] = {
    [SCT_MAC] = "SCT_MAC",
    [SCT_SYS] = "SCT_SYS",
    [SCT_SEC] = "SCT_SEC"
};

/* Block type names */
static const char *battr_type_str(uint8_t battr)
{
    switch (battr) {
    case BLK_NONE:              return "none";
    case BLK_NONE_AQ:           return "none.acquire";
    case BLK_NONE_RL:           return "none.release";
    case BLK_NONE_AQRL:         return "none.acquire&release";
    case BLK_ATOMIC:            return "atomic";
    case BLK_ATOMIC_AQ:         return "atomic.acquire";
    case BLK_ATOMIC_RL:         return "atomic.release";
    case BLK_ATOMIC_AQRL:       return "atomic.acquire&release";
    case BLK_ATOMIC_FAR_NONE:   return "atomic.far.none";
    case BLK_ATOMIC_FAR_AQ:     return "atomic.far.acquire";
    case BLK_ATOMIC_FAR_RL:     return "atomic.far.release";
    case BLK_ATOMIC_FAR_AQRL:   return "atomic.far.acquire&release";
    default:
        return "unkown";
    }
};

static const char *datr_cm_str(uint8_t cm)
{
    switch (cm) {
    case 0: return "EQ";
    case 1: return "NE";
    case 2: return "LT";
    case 3: return "GT";
    case 4: return "LE";
    case 5: return "GE";
    default: return "REV";
    }
    return "REV";
}

static const char *next_type_str(uint8_t brtype)
{
    switch (brtype) {
    case BRANCH_FALL:               return "fall_through";
    case BRANCH_IND:                return "indirect_link";
    case BRANCH_INDCALL:            return "indirect_call";
    case BRANCH_RET:                return "ret";
    case BRANCH_DIRECT_LINK:        return "direct_link";
    case BRANCH_CONDITIONAL:        return "conditional";
    case BRANCH_CALL:               return "call";
    default:                        return "unknown";
    }
}

static const char *block_type_name(uint8_t block_type)
{
    switch (block_type) {
    case HEAD_TYPE_STD: return "BSTART.STD";
    case HEAD_TYPE_FP: return "BSTART.FP";
    case HEAD_TYPE_SYS: return "BSTART.SYS";
    case HEAD_TYPE_SIMT: return "BSTART.SIMT";
    case HEAD_TYPE_MCOPY: return "MCOPY";
    case HEAD_TYPE_MMOVE: return "MMOVE";
    case HEAD_TYPE_MSET: return "MSET";
    case HEAD_TYPE_MPUSH: return "MPUSH";
    case HEAD_TYPE_MPOP: return "MPOP";
    case HEAD_TYPE_FENTRY: return "FENTRY";
    case HEAD_TYPE_FEXIT: return "FEXIT";
    case HEAD_TYPE_FRET_RA: return "FRET.RA";
    case HEAD_TYPE_FRET_STK: return "FRET.STK";
    default: return "unkown_block";
    }
}

static const char *tileop_type_name(int tileop_type)
{
    switch (tileop_type) {
    case TILEOP_MPAR: return "MPAR";
    case TILEOP_TADD: return "TADD";
    case TILEOP_TSUB: return "TSUB";
    case TILEOP_TMUL: return "TMUL";
    case TILEOP_TDIV: return "TDIV";
    case TILEOP_TMAX: return "TMAX";
    case TILEOP_TADDS: return "TADDS";
    case TILEOP_TSUBS: return "TSUBS";
    case TILEOP_TMULS: return "TMULS";
    case TILEOP_TDIVS: return "TDIVS";
    case TILEOP_TMAXS: return "TMAXS";
    case TILEOP_TEXP: return "TEXP";
    case TILEOP_TSQRT: return "TSQRT";
    case TILEOP_TRECIP: return "TRECIP";
    case TILEOP_TABS: return "TABS";
    case TILEOP_TCVT: return "TCVT";
    case TILEOP_TROWSUM: return "TROWSUM";
    case TILEOP_TROWMAX: return "TROWMAX";
    case TILEOP_TROWSUMEXP: return "TROWSUMEXP";
    case TILEOP_TROWMAXEXP: return "TROWMAXEXP";
    case TILEOP_MSEQ: return "MSEQ";
    case TILEOP_TLOAD: return "TLOAD";
    case TILEOP_TSTORE: return "TSTORE";
    case TILEOP_TMOV: return "TMOV";
    case TILEOP_MAMULB: return "MAMULB";
    case TILEOP_MAMULBAC: return "MAMULBAC";
    case TILEOP_MAMULBACC: return "MAMULBACC";
    case TILEOP_ACCCVT: return "ACCCVT";
    case TILEOP_VPAR: return "VPAR";
    case TILEOP_VSEQ: return "VSEQ";
    default: return "unkown_tileop";
    }
}

static const char *matrix_trans_format(int fmt)
{
    switch (fmt) {
    case 3: return "nd2zn";
    case 4: return "nd2nz";
    case 8: return "dn2zn";
    case 9: return "dn2nz";
    case 27: return "nz2nd";
    case 28: return "nz2dn";
    case 32: return "canon";
    default:
        break;
    }
    return "unkown format";
}


static const char *tileop_datatype_name(int tileop_datatype)
{
    switch (tileop_datatype) {
    case FP64: return "FP64";
    case FP32: return "FP32";
    case FP16: return "FP16";
    case E5M2: return "E5M2";
    case E4M3: return "E4M3";
    case BF16: return "BF16";
    case E8M0: return "E8M0";
    case INT64: return "INT64";
    case INT32: return "INT32";
    case INT16: return "INT16";
    case INT8: return "INT8";
    case UINT64: return "UINT64";
    case UINT32: return "UINT32";
    case UINT16: return "UINT16";
    case UINT8: return "UINT8";
    default: return "unkown_tileop_datatype";
    }
}

static inline bool is_vec_tumn_reg(int src)
{
    if ((src >= SRC_FVEC_VT_1 && src <= SRC_FVEC_VN_4) ||
        (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4)) {
        return true;
    }
    return false;
}

static inline const char *get_fvec_src_reg_name(DisasContext *ctx, int src_code,
                                                int type)
{
    int idx = 0, src = extract16(src_code, 0, 7);
    int src_type = extract16(src_code, 7, 3) & 0b111;
    char *src_name = g_malloc(SRC_LEN);

    if (src >= SRC_FVEC_VT_1 && src <= SRC_FVEC_VT_4) {
        idx = src - SRC_FVEC_VT_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vt[idx]);
    } else if (src >= SRC_FVEC_VU_1 && src <= SRC_FVEC_VU_4) {
        idx = src - SRC_FVEC_VU_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vu[idx]);
    } else if (src >= SRC_FVEC_VM_1 && src <= SRC_FVEC_VM_4) {
        idx = src - SRC_FVEC_VM_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vm[idx]);
    } else if (src >= SRC_FVEC_VN_1 && src <= SRC_FVEC_VN_4) {
        idx = src - SRC_FVEC_VN_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vn[idx]);
    } else if (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4) {
        idx = src - SRC_FVEC_VT_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vt_reuse[idx]);
    } else if (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4) {
        idx = src - SRC_FVEC_VU_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vu_reuse[idx]);
    } else if (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4) {
        idx = src - SRC_FVEC_VM_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vm_reuse[idx]);
    } else if (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4) {
        idx = src - SRC_FVEC_VN_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vn_reuse[idx]);
    } else if (src >= SRC_FVEC_RI0 && src <= SRC_FVEC_RI11) {
        idx = src - SRC_FVEC_RI0;
        pstrcpy(src_name, SRC_LEN, fvec_ri_reg_name[idx]);
    } else if (src >= SRC_FVEC_T_1 && src <= SRC_FVEC_U_4) {
        return get_src_reg_name(src & SRC_FVRC_REG_MASK);
    } else {
        switch (src) {
        case SRC_FVEC_LC0:
            return "lc0";
        case SRC_FVEC_LC1:
            return "lc1";
        case SRC_FVEC_LC2:
            return "lc2";
        case SRC_FVEC_LB0:
            return "lb0";
        case SRC_FVEC_LB1:
            return "lb1";
        case SRC_FVEC_LB2:
            return "lb2";
        case SRC_FVEC_TA:
            return "ta";
        case SRC_FVEC_TB:
            return "tb";
        case SRC_FVEC_TC:
            return "tc";
        case SRC_FVEC_TD:
            return "td";
        case SRC_FVEC_TE:
            return "te";
        case SRC_FVEC_TF:
            return "tf";
        case SRC_FVEC_TG:
            return "tg";
        case SRC_FVEC_TH:
            return "th";
        case SRC_FVEC_TO:
            return "to";
        case SRC_FVEC_TO1:
            return "to1";
        case SRC_FVEC_TO2:
            return "to2";
        case SRC_FVEC_TO3:
            return "to3";
        case SRC_FVEC_ZERO:
            return "zero";
        case SRC_FVEC_PRED:
            return "p";
        default:
            g_assert_not_reached();
            break;
        }
    }

    if (type) {
        pstrcat(src_name, SRC_LEN, srctype_fp[src_type]);
    } else {
        pstrcat(src_name, SRC_LEN, src_reg_type[src_type]);
    }

    return src_name;
}

static inline const char *get_fvec_cvt_src_reg_name(DisasContext *ctx,
    int src_code, int is_float, int src_type)
{
    int idx = 0, src = extract16(src_code, 0, 7);
    char *src_name = g_malloc(SRC_LEN);

    if (src >= SRC_FVEC_VT_1 && src <= SRC_FVEC_VT_4) {
        idx = src - SRC_FVEC_VT_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vt[idx]);
    } else if (src >= SRC_FVEC_VU_1 && src <= SRC_FVEC_VU_4) {
        idx = src - SRC_FVEC_VU_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vu[idx]);
    } else if (src >= SRC_FVEC_VM_1 && src <= SRC_FVEC_VM_4) {
        idx = src - SRC_FVEC_VM_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vm[idx]);
    } else if (src >= SRC_FVEC_VN_1 && src <= SRC_FVEC_VN_4) {
        idx = src - SRC_FVEC_VN_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vn[idx]);
    } else if (src >= SRC_FVEC_VT_REUSE_1 && src <= SRC_FVEC_VT_REUSE_4) {
        idx = src - SRC_FVEC_VT_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vt_reuse[idx]);
    } else if (src >= SRC_FVEC_VU_REUSE_1 && src <= SRC_FVEC_VU_REUSE_4) {
        idx = src - SRC_FVEC_VU_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vu_reuse[idx]);
    } else if (src >= SRC_FVEC_VM_REUSE_1 && src <= SRC_FVEC_VM_REUSE_4) {
        idx = src - SRC_FVEC_VM_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vm_reuse[idx]);
    } else if (src >= SRC_FVEC_VN_REUSE_1 && src <= SRC_FVEC_VN_REUSE_4) {
        idx = src - SRC_FVEC_VN_REUSE_1;
        pstrcpy(src_name, SRC_LEN, fvec_src_vn_reuse[idx]);
    } else if (src >= SRC_FVEC_RI0 && src <= SRC_FVEC_RI11) {
        idx = src - SRC_FVEC_RI0;
        pstrcpy(src_name, SRC_LEN, fvec_ri_reg_name[idx]);
    } else if (src >= SRC_FVEC_T_1 && src <= SRC_FVEC_U_4) {
        return get_src_reg_name(src & SRC_FVRC_REG_MASK);
    } else {
        switch (src) {
        case SRC_FVEC_LC0:
            return "lc0";
        case SRC_FVEC_LC1:
            return "lc1";
        case SRC_FVEC_LC2:
            return "lc2";
        case SRC_FVEC_LB0:
            return "lb0";
        case SRC_FVEC_LB1:
            return "lb1";
        case SRC_FVEC_LB2:
            return "lb2";
        case SRC_FVEC_TA:
            return "ta";
        case SRC_FVEC_TB:
            return "tb";
        case SRC_FVEC_TC:
            return "tc";
        case SRC_FVEC_TD:
            return "td";
        case SRC_FVEC_TE:
            return "te";
        case SRC_FVEC_TF:
            return "tf";
        case SRC_FVEC_TG:
            return "tg";
        case SRC_FVEC_TH:
            return "th";
        case SRC_FVEC_TO:
            return "to";
        case SRC_FVEC_TO1:
            return "to1";
        case SRC_FVEC_TO2:
            return "to2";
        case SRC_FVEC_TO3:
            return "to3";
        default:
            g_assert_not_reached();
            break;
        }
    }
    if (is_float) {
        pstrcat(src_name, SRC_LEN, fvec_cvt_fp[src_type]);
    } else {
        pstrcat(src_name, SRC_LEN, fvec_cvt_int[src_type]);
    }
    return src_name;
}

static inline const char *get_fvec_dst_reg_name(DisasContext *ctx, int dst_code,
                                                int type)
{
    int dst = extract16(dst_code, 0, 7);
    int dst_type = extract16(dst_code, 7, 3) & 0b011;
    char *src_name = g_malloc(DSR_LEN);

    if (dst >= DST_FVEC_VT && dst <= DST_FVEC_VN) {
        pstrcpy(src_name, DSR_LEN, fvec_ldst_reg_name[dst]);
    } else if (dst >= DST_FVEC_RO0 && dst <= DST_FVEC_RO3) {
        pstrcpy(src_name, DSR_LEN, fvec_ro_reg_name[dst - DST_FVEC_RO0]);
    } else if (dst == DST_FVEC_T) {
        pstrcpy(src_name, DSR_LEN, ", ->t");
    } else if (dst == DST_FVEC_U) {
        pstrcpy(src_name, DSR_LEN, ", ->u");
    } else if (dst == SRC_FVEC_PRED) {
        pstrcpy(src_name, DSR_LEN, ", ->p");
    } else if (dst == DST_FVEC_ZERO) {
        /* no output */
    } else {
        g_assert_not_reached();
    }

    if (type) {
        pstrcat(src_name, DSR_LEN, srctype_fp[dst_type]);
    } else {
        pstrcat(src_name, DSR_LEN, dst_reg_type[dst_type]);
    }
    return src_name;
}

static inline const char *get_fvec_cvt_dst_reg_name(DisasContext *ctx,
    int dst_code, int is_float, int dst_type)
{
    int dst = extract16(dst_code, 0, 7);
    char *src_name = g_malloc(DSR_LEN);

    if (dst >= DST_FVEC_VT && dst <= DST_FVEC_VN) {
        pstrcpy(src_name, DSR_LEN, fvec_ldst_reg_name[dst]);
    } else if (dst >= DST_FVEC_RO0 && dst <= DST_FVEC_RO3) {
        pstrcpy(src_name, DSR_LEN, fvec_ro_reg_name[dst - DST_FVEC_RO0]);
    } else if (dst == DST_FVEC_T) {
        pstrcpy(src_name, DSR_LEN, ", ->t");
    } else if (dst == DST_FVEC_U) {
        pstrcpy(src_name, DSR_LEN, ", ->u");
    } else if (dst == DST_FVEC_ZERO) {
        /* no output */
    } else {
        g_assert_not_reached();
    }

    if (is_float) {
        pstrcat(src_name, DSR_LEN, fvec_cvt_fp[dst_type]);
    } else {
        pstrcat(src_name, DSR_LEN, fvec_cvt_int[dst_type]);
    }
    return src_name;
}

static inline int strcompare(const char *str, const char *val)
{
    const char *p, *q;
    p = str;
    q = val;
    while (*q != '\0') {
        if (*p != *q) {
            return 0;
        }
        p++;
        q++;
    }
    return 1;
}

/* format instruction */
static void
print_src(uint16_t SrcBegin, uint16_t SrcEnd, struct disassemble_info *info)
{
    int i;
    bool is_first = true;

    (*info->fprintf_func)(info->stream, " [");
    for (i = SrcBegin; i <= SrcEnd; i++) {
        if (is_first) {
            (*info->fprintf_func)(info->stream, "%s", linx_src_reg_name[i]);
            is_first = false;
        } else {
            (*info->fprintf_func)(info->stream, ",%s", linx_src_reg_name[i]);
        }
    }
    (*info->fprintf_func)(info->stream, "]");
}

#define OUTPUT(ctx, mnemonic, fmt, ...)                                        \
{                                                                              \
    ctx->info->fprintf_func(ctx->info->stream, "%s" " " fmt,                   \
    mnemonic, ##__VA_ARGS__);                                                  \
}

#define EX_SH(amount) \
    static int ex_shift_##amount(DisasContext *ctx, int imm) \
    {                                         \
        return imm << amount;                 \
    }
EX_SH(1)
EX_SH(3)
EX_SH(12)

#include "decode-block16.c.inc"
#include "decode-block32.c.inc"
#include "decode-block48.c.inc"
#include "decode-block32_private_fvec.c.inc"
#include <math.h>

/* 16 bit 微指令 */

/* c.movr regSrc, {->t, ->u, ->Rd} */
static bool trans_blk_c_movr_16(DisasContext *ctx, arg_blk_c_movr_16 *a)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);
    (ctx->info->fprintf_func)(ctx->info->stream, "c.movr %s%s",
                              srcl_name, dest_name);
    return true;
}

/* c.movi simm, {->t, ->u, ->Rd} */
static bool trans_blk_c_movi_16(DisasContext *ctx, arg_blk_c_movi_16 *a)
{
    const char *dest_name = get_dest_reg_name(a->RegDst);
    (ctx->info->fprintf_func)(ctx->info->stream, "c.movi %x%s",
                              a->imm, dest_name);
    return true;
}

/* c.setret imm, ->Ra */
static bool trans_blk_c_setret_16(DisasContext *ctx, arg_blk_c_setret_16 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "c.setret %x, ->Ra", a->imm);
    return true;
}

/* c.add srcL, srcR, ->t */
static void print_block_c_insn_srcl_srcr(DisasContext *ctx, arg_arg_c_arith *a,
                                         const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(TARGET_REG_T);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

/* c.setc.eq srcL, srcR */
static void
print_block_c_insn_set_srcl_srcr(DisasContext *ctx, arg_arg_c_arith *a,
                                 const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);

    OUTPUT(ctx, mnemonic, "%s, %s", srcl_name, srcr_name);
}

/* c.lwi [srcL, simm], ->t */
static void
print_block_c_insn_load_srcl_imm(DisasContext *ctx, arg_arg_ld_sd *a,
                                 const char *mnemonic, int shift)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(TARGET_REG_T);

    OUTPUT(ctx, mnemonic, "[%s, %d]%s", srcl_name, (a->imm << shift),
           dest_name);
}

/* c.swi t#1, [srcL, simm] */
static void
print_block_c_insn_store_srcl_imm(DisasContext *ctx, arg_arg_ld_sd *a,
                                   const char *mnemonic, int shift)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "t#1, [%s, %d]", srcl_name, (a->imm << shift));
}

/* c.addi srcL, simm, ->t */
static bool trans_blk_c_addi_16(DisasContext *ctx, arg_blk_c_addi_16 *a)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(TARGET_REG_T);

    (ctx->info->fprintf_func)(ctx->info->stream, "c.addi %s, %d%s",
                              srcl_name, a->imm, dest_name);
    return true;
}

/* c.setc.tgt srcL */
static bool trans_blk_c_setc_tgt_16(DisasContext *ctx, arg_blk_c_setc_tgt_16 *a)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    (ctx->info->fprintf_func)(ctx->info->stream, "c.setc.tgt %s", srcl_name);
    return true;
}

/* sext.b srcL, ->t */
static void
print_block_c_insn_srcl(DisasContext *ctx, arg_arg_ext *a,
                        const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(TARGET_REG_T);

    OUTPUT(ctx, mnemonic, "%s%s", srcl_name, dest_name);
}

/* c.cmp.eqi t#1, simm, ->t */
static void
print_block_c_insn_imm(DisasContext *ctx, arg_arg_cmp *a, const char *mnemonic)
{
    const char *dest_name = get_dest_reg_name(TARGET_REG_T);

    OUTPUT(ctx, mnemonic, "t#1, %d%s", a->imm, dest_name);
}

/* c.ssrget SSR-ID, ->t */
static bool trans_blk_c_ssrget_16(DisasContext *ctx, arg_blk_c_ssrget_16 *a)
{
    const char *ssr_name = csr_name(a->SSRID);
    const char *dest_name = get_dest_reg_name(TARGET_REG_T);
    (ctx->info->fprintf_func)(ctx->info->stream, "c.ssrget %s%s", ssr_name,
                              dest_name);
    return true;
}

/* 32 bit 微指令 */

/* add SrcL, SrcR<{.sw,.uw,.neg}><<<shamt>, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcl_srcr_au_shamt_dst(DisasContext *ctx, arg_arg_arith *a,
                                        const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "%s, %s%s<<%d%s", srcl_name, srcr_name,
               srcr_type, a->shamt, dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "%s, %s%s%s", srcl_name, srcr_name,
               srcr_type, dest_name);
    }
}

/* and SrcL, SrcR<{.sw,.uw,.not}><<<shamt>, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcl_srcr_lu_shamt_dst(DisasContext *ctx, arg_arg_arith *a,
                                        const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_lu[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "%s, %s%s<<%d%s", srcl_name, srcr_name,
               srcr_type, a->shamt, dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "%s, %s%s%s", srcl_name, srcr_name,
               srcr_type, dest_name);
    }
}

/* srl SrcL, SrcR, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcl_srcr_dst(DisasContext *ctx, arg_arg_shift *a,
                               const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

/* addi SrcL, uimm, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcl_imm_dst(DisasContext *ctx, arg_arg_arith_i *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %d%s", srcl_name, a->imm, dest_name);
}

static void
print_block_insn_hl_srcl_imm_dst(DisasContext *ctx, arg_arg_hl_arith_i *a,
                                 const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %d%s", srcl_name, a->imm, dest_name);
}

/* srli SrcL, shamt, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcl_shamt_dst(DisasContext *ctx, arg_arg_shift_i *a,
                                const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %d%s", srcl_name, a->shamt, dest_name);
}

/* cmp.eq SrcL, SrcR<{.sw, .uw}>, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcl_srcr_au_dst(DisasContext *ctx, arg_arg_compare *a,
                                const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_cmp_sect[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s%s%s", srcl_name, srcr_name,
           srcr_type, dest_name);
}

/* cmp.or SrcL, SrcR<{.sw, .uw, .not}>, {->t, ->u, ->Rd} */
static void
print_block_insn_srcl_srcr_lu_dst(DisasContext *ctx, arg_arg_compare *a,
                                const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_cmp_sect_lu[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s%s%s", srcl_name, srcr_name,
           srcr_type, dest_name);
}

/* setc.eq SrcL, SrcR<{.sw, .uw}> */
static void
print_block_insn_srcl_srcr_au(DisasContext *ctx, arg_arg_setc *a,
                           const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_cmp_sect[a->SrcRType];

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, srcr_type);
}

/* setc.and SrcL, SrcR<{.sw, .uw, .not}> */
static void
print_block_insn_srcl_srcr_lu(DisasContext *ctx, arg_arg_setc *a,
                           const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_cmp_sect_lu[a->SrcRType];

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, srcr_type);
}

/* setc.eqi SrcL, simm */
static void
print_block_insn_srcl_imm(DisasContext *ctx, arg_arg_setc_i *a,
                          const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s, %d", srcl_name, (a->imm << a->shamt));
}

static void
print_block_insn_hl_srcl_imm(DisasContext *ctx, arg_arg_hl_setc_i *a,
                             const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s, %d", srcl_name, a->imm);
}

/* b.eq SrcL, SrcR, label */
static void
print_block_insn_branch_srcl_srcr_imm(DisasContext *ctx, arg_arg_branch *a,
                                           const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    uint64_t next = ctx->pc + a->imm;

    OUTPUT(ctx, mnemonic, "%s, %s, #0x%lx", srcl_name, srcr_name, next);
}

/* j label */
static void
print_block_insn_j_imm(DisasContext *ctx, arg_arg_j_32 *a,
                                   const char *mnemonic)
{
    OUTPUT(ctx, mnemonic, "%d", a->imm);
}

/* jr SrcL.<S>, simm */
static void
print_block_insn_jr_srcl_imm(DisasContext *ctx, arg_arg_branch *a,
                      const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);

    OUTPUT(ctx, mnemonic, "%s, %d", srcl_name, a->imm);
}

/* bxs SrcL, M, N, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcl_m_n_dst(DisasContext *ctx, arg_arg_bo *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %d, %d%s", srcl_name, a->M, a->N + 1, dest_name);
}

/* rev SrcL,  M, N, ->{t, u, Rd} */
static void
print_block_insn_rev_srcl_m_n_dst(DisasContext *ctx, arg_arg_bo *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %d, %d%s", srcl_name, a->M + 1, a->N, dest_name);
}

/* madd SrcL, SrcR, SrcD, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcd_srcl_srcr_dst(DisasContext *ctx, arg_arg_madd *a,
                                    const char *mnemonic)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s, %s%s", srcl_name, srcr_name,
           srcd_name, dest_name);
}

/* addtpc simm, {->t, ->u, ->RegDst} */
static void
print_block_insn_imm_dst(DisasContext *ctx, arg_arg_pc *a,
                         const char *mnemonic)
{
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%d%s", a->imm, dest_name);
}

/* addtpc simm, {->t, ->u, ->RegDst} */
static void
print_block_insn_imm_dst1(DisasContext *ctx, arg_arg_lui *a,
                         const char *mnemonic)
{
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%d%s", a->imm, dest_name);
}

/* ssrget SSR_ID, {->t, ->u, ->RegDst} */
static void
print_block_insn_ssrget_ssr_id_dst(DisasContext *ctx, arg_arg_ssrget *a,
                           const char *mnemonic)
{
    const char *ssr_name = csr_name(a->SSRID);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s%s", ssr_name, dest_name);
}

/* ssrset SrcL, SSRID */
static void
print_block_insn_ssrset_srcl_ssr_id(DisasContext *ctx, arg_arg_ssrset *a,
                            const char *mnemonic)
{
    const char *ssr_name = csr_name(a->SSRID);
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s, %s", srcl_name, ssr_name);
}

/* ssrswap SrcL, SSR_ID, ->Rd */
static void
print_block_insn_ssrswap_ssr_id_srcl_dst(DisasContext *ctx, arg_arg_ssrswap *a,
                            const char *mnemonic)
{
    const char *ssr_name = csr_name(a->SSRID);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, ssr_name, dest_name);
}

/* lsrset SrcL */
static void
print_block_insn_lsrset_srcl(DisasContext *ctx, arg_arg_ssrset *a,
                      const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s", srcl_name);
}

/* lb [SrcL, SrcR<{.sw, .uw}><<<shamt>], {->t, ->u, ->RegDst} */
static void
print_block_insn_load_srcl_srcr_au_shamt_dst(DisasContext *ctx,
                                             arg_arg_arith *a,
                                             const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "[%s, %s%s<<%d]%s", srcl_name, srcr_name,
               srcr_type, a->shamt, dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "[%s, %s%s]%s", srcl_name, srcr_name,
               srcr_type, dest_name);
    }
}

/* lbi [SrcL, simm], {->t, ->u, ->RegDst} */
static void
print_block_insn_load_srcl_imm_dst(DisasContext *ctx, arg_arg_arith_i *a,
                                   const char *mnemonic, int shift)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "[%s, %d]%s", srcl_name, (a->imm << shift),
           dest_name);
}

/* lb.pcr [symbol], ->{t, u, Rd} */
static void print_block_insn_load_imm_dst(DisasContext *ctx, arg_arg_pc *a,
                                          const char *mnemonic)
{
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "[%d]%s", a->imm, dest_name);
}


/* sb SrcD, [SrcL, srcR<{.sw,.uw,.neg}>] */
static void
print_block_insn_srcd_srcl_srcr_au(DisasContext *ctx,
    arg_arg_store *a,
    const char *mnemonic)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_lu[a->SrcRType];

    OUTPUT(ctx, mnemonic, "%s, [%s, %s%s]", srcd_name, srcl_name,
           srcr_name, srcr_type);
}

/* sbi SrcL, [SrcR, simm] */
static void
print_block_insn_store_srcl_srcr_imm(DisasContext *ctx,
    arg_arg_store_imm *a, const char *mnemonic, int shift)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);

    OUTPUT(ctx, mnemonic, "%s, [%s, %d]", srcl_name, srcr_name,
           (a->imm << shift));
}

/* sb.pcr SrcL, [symbol] */
static void
print_block_insn_store_srcl_imm(DisasContext *ctx,
    arg_arg_store_symbol *a, const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s, [%d]", srcl_name, a->imm);
}

/* csel SrcP, SrcL, SrcR, {->t, ->u, ->RegDst} */
static void
print_block_insn_srcp_srcl_srcr_dst(DisasContext *ctx, arg_arg_csel *a,
                                  const char *mnemonic)
{
    const char *srcp_name = get_src_reg_name(a->SrcP);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s, %s%s%s", srcp_name, srcl_name,
           srcr_name, srcr_type, dest_name);
}

/* prf{.l1,.l2,.l3} [SrcL, SrcR<{.sw, .uw}><<<shamt>] */
static void
print_block_insn_prf_srcl_srcr_au_shamt(DisasContext *ctx,
    arg_arg_prf *a, const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *cache_type = cachetype_prf[a->model];

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "%s [%s, %s%s<<%d]", cache_type, srcl_name,
               srcr_name, srcr_type, a->shamt);
    } else {
        OUTPUT(ctx, mnemonic, "%s [%s, %s%s]", cache_type, srcl_name, srcr_name,
               srcr_type);
    }
}

static void
print_block_insn_prf32_srcl_srcr_au_shamt(DisasContext *ctx,
    arg_arg_prf32 *a, const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "[%s, %s%s<<%d]", srcl_name, srcr_name,
               srcr_type, a->shamt);
    } else {
        OUTPUT(ctx, mnemonic, "[%s, %s%s]", srcl_name, srcr_name, srcr_type);
    }
}

/* prf.a{.l1,.l2,.l3} [SrcL, SrcR<{.sw, .uw}><<<shamt>], {->t, ->u, ->RegDst} */
static void
print_block_insn_prf_srcl_srcr_au_shamt_dst(DisasContext *ctx,
    arg_arg_prf *a, const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *cache_type = cachetype_prf[a->model];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "%s [%s, %s%s<<%d]%s", cache_type, srcl_name,
               srcr_name, srcr_type, a->shamt, dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "%s [%s, %s%s]%s", cache_type, srcl_name,
               srcr_name, srcr_type, dest_name);
    }
}

/* prfi.u.l1 [SrcL, simm] */
static void
print_block_insn_prf_srcl_imm(DisasContext *ctx, arg_arg_prfi *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *cache_type = cachetype_prf[0];

    OUTPUT(ctx, mnemonic, "%s [%s, %d]", cache_type, srcl_name, a->imm);
}

static void
print_block_insn_prf32_srcl_imm(DisasContext *ctx, arg_arg_prfi32 *a,
                                const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "[%s, %d]", srcl_name, a->imm);
}

/* prfi.ua.l1 [SrcL, simm], {->t, ->u, ->RegDst} */
static void
print_block_insn_prf_srcl_imm_dst(DisasContext *ctx, arg_arg_prfi *a,
                                  const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *cache_type = cachetype_prf[0];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s [%s, %d]%s",
           cache_type, srcl_name, a->imm, dest_name);
}

/* bse SrcL */
static void
print_block_insn_exec_ctrl_srcl(DisasContext *ctx, arg_arg_exec_ctrl *a,
                      const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s", srcl_name);
}

/* bc_iva SrcL */
static void
print_block_insn_srcl(DisasContext *ctx, arg_arg_cache *a,
                      const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s", srcl_name);
}

/* acrc request_type */
static void
print_block_insn_request_type(DisasContext *ctx, arg_arg_acrc *a,
                              const char *mnemonic)
{
    OUTPUT(ctx, mnemonic, "%s", acrc_request_type[a->RST_Type]);
}

static void
print_block_insn_rra_type(DisasContext *ctx, arg_arg_acre *a,
                          const char *mnemonic)
{
    OUTPUT(ctx, mnemonic, "%d", a->RRA_Type);
}

/* fence.d perd_imm, succ_imm */
static void
print_block_insn_pred_succ(DisasContext *ctx, arg_arg_fence_d *a,
                           const char *mnemonic)
{
    const char *pred = fence_mask_str[a->PRED_IMM];
    const char *succ = fence_mask_str[a->SUCC_IMM];

    OUTPUT(ctx, mnemonic, "%s, %s", pred, succ);
}

/* lr.size<{.aq,.rl,.aqrl}> [SrcL], {->t, ->u, ->RegDst} */
static void
print_block_insn_aq_rl_srcl_dst(DisasContext *ctx, arg_arg_atomic *a,
                                const char *mnemonic)
{
    /* 6 for ".aq" or ".rl" or ".aqrl" and zero-terminate */
    int len = strlen(mnemonic) + 6;
    char *inst_name = g_malloc(len);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    pstrcpy(inst_name, len, mnemonic);

    if (a->aq && a->rl) {
        pstrcat(inst_name, len, ".aqrl");
    } else if (a->aq && !a->rl) {
        pstrcat(inst_name, len, ".aq");
    } else if (!a->aq && a->rl) {
        pstrcat(inst_name, len, ".rl");
    }

    OUTPUT(ctx, inst_name, "[%s]%s", srcl_name, dest_name);
    g_free(inst_name);
}

/* sc.size<{.aq,.rl,.aqrl}> SrcL, [SrcR], {->t, ->u, ->RegDst} */
static void
print_block_insn_sc_aq_rl_srcl_srcr_dst(DisasContext *ctx,
    arg_arg_atomic *a, const char *mnemonic)
{
    /* 6 for ".aq" or ".rl" or ".aqrl" and zero-terminate */
    int len = strlen(mnemonic) + 6;
    char *inst_name = g_malloc(len);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    pstrcpy(inst_name, len, mnemonic);

    if (a->aq && a->rl) {
        pstrcat(inst_name, len, ".aqrl");
    } else if (a->aq && !a->rl) {
        pstrcat(inst_name, len, ".aq");
    } else if (!a->aq && a->rl) {
        pstrcat(inst_name, len, ".rl");
    }

    OUTPUT(ctx, inst_name, "%s, [%s]%s", srcl_name, srcr_name, dest_name);
    g_free(inst_name);
}

/* ld.add<{.aq,.rl,.aqrl}> [SrcL], SrcR, ->{t, u, Rd} */
static void
print_block_insn_atomic_srcl_srcr_dst(DisasContext *ctx,
    arg_arg_atomic *a, const char *mnemonic)
{
    /* 6 for ".aq" or ".rl" or ".aqrl" and zero-terminate */
    int len = strlen(mnemonic) + 6;
    char *inst_name = g_malloc(len);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    pstrcpy(inst_name, len, mnemonic);

    if (a->aq && a->rl) {
        pstrcat(inst_name, len, ".aqrl");
    } else if (a->aq && !a->rl) {
        pstrcat(inst_name, len, ".aq");
    } else if (!a->aq && a->rl) {
        pstrcat(inst_name, len, ".rl");
    }

    OUTPUT(ctx, inst_name, "[%s], %s%s", srcl_name, srcr_name, dest_name);
    g_free(inst_name);
}

static void
print_block_insn_dma_srcl_srcr(DisasContext *ctx, arg_arg_dma *a,
                                  const char *mnemonic)
{
    int len = strlen(mnemonic) + 6;
    char *inst_name = g_malloc(len);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);

    pstrcpy(inst_name, len, mnemonic);

    OUTPUT(ctx, inst_name, "[%s], %s", srcl_name, srcr_name);
    g_free(inst_name);
}

/* sw.op<{.rl}> [SrcL], SrcR */
static void
print_block_insn_atomic_srcl_srcr(DisasContext *ctx, arg_arg_atomic *a,
                                  const char *mnemonic)
{
    /* 6 for ".aq" or ".rl" or ".aqrl" and zero-terminate */
    int len = strlen(mnemonic) + 6;
    char *inst_name = g_malloc(len);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);

    pstrcpy(inst_name, len, mnemonic);

    if (a->rl) {
        pstrcat(inst_name, len, ".rl");
    }

    OUTPUT(ctx, inst_name, "[%s], %s", srcl_name, srcr_name);
    g_free(inst_name);
}

/* fadd.<T> SrcL, SrcR, ->{t, u, Rd} */
static void
print_block_insn_fp_srcl_srcr_dst(DisasContext *ctx, arg_arg_fpa *a,
                                  const char *mnemonic)
{
    int len = strlen(mnemonic) + 5;
    char *inst_name = g_malloc(len);

    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    pstrcpy(inst_name, len, mnemonic);
    pstrcat(inst_name, len, srctype_fp[a->SrcType]);

    OUTPUT(ctx, inst_name, "%s, %s%s", srcl_name, srcr_name, dest_name);
    g_free(inst_name);
}

/* fmadd.<T> SrcL, SrcR, SrcA, ->{t, u, Rd} */
static void
print_block_insn_fp_srcl_srcr_srca_dst(DisasContext *ctx, arg_arg_fpa_2 *a,
                                       const char *mnemonic)
{
    int len = strlen(mnemonic) + 5;
    char *inst_name = g_malloc(len);

    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srca_name = get_src_reg_name(a->SrcA);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    pstrcpy(inst_name, len, mnemonic);
    pstrcat(inst_name, len, srctype_fp[a->SrcType]);

    OUTPUT(ctx, inst_name, "%s, %s, %s%s", srcl_name, srcr_name,
           srca_name, dest_name);
    g_free(inst_name);
}

/* fabs.<T> SrcL, ->{t, u, Rd} */
static void
print_block_insn_fp_srcl_dst(DisasContext *ctx, arg_arg_fpa3 *a,
                             const char *mnemonic)
{
    int len = strlen(mnemonic) + 5;
    char *inst_name = g_malloc(len);

    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    pstrcpy(inst_name, len, mnemonic);
    pstrcat(inst_name, len, srctype_fp[a->SrcType]);

    OUTPUT(ctx, inst_name, "%s%s", srcl_name, dest_name);
    g_free(inst_name);
}

/* fcvt.{srcT2dstT} SrcL, ->{t, u, Rd} */
static void
print_block_insn_dst_tp_src_tp_srcl_srcr_dst(DisasContext *ctx,
                                             arg_arg_fpcvt *a,
                                             const char *mnemonic,
                                             int src_type, int dst_type)
{
    int len = strlen(mnemonic) + 9;
    char *inst_name = g_malloc(len);

    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    pstrcpy(inst_name, len, mnemonic);
    pstrcat(inst_name, len, ".");
    pstrcat(inst_name, len, cvt_tp[a->SrcType + src_type]);
    pstrcat(inst_name, len, "to");
    pstrcat(inst_name, len, cvt_tp[a->DstType + dst_type]);

    OUTPUT(ctx, inst_name, "%s%s", srcl_name, dest_name);
    g_free(inst_name);
}

/*
 * SIMT block disassembly
 */

/* add SrcL.<T>, SrcR.<T><<<shamt>, ->{t,u,m,n}.<W> */
static void
print_block_insn_fvec_srcl_srcr_au_shamt_dst(
    DisasContext *ctx, arg_simt_arith *a, const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "%s, %s%s<<%d%s", srcl_name, srcr_name,
               srcr_type, a->shamt, dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "%s, %s%s%s", srcl_name, srcr_name,
               srcr_type, dest_name);
    }
}

/* and SrcL.<S>, SrcR.<S><<<shamt>, {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_srcl_srcr_lu_shamt_dst(
    DisasContext *ctx, arg_simt_arith *a, const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *srcr_type = srctype_lu[a->SrcRType];
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "%s, %s%s<<%d%s", srcl_name, srcr_name,
               srcr_type, a->shamt, dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "%s, %s%s%s", srcl_name, srcr_name,
               srcr_type, dest_name);
    }
}

/* srl SrcL.<S>, SrcR.<S>, {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_srcl_srcr_dst(DisasContext *ctx, arg_simt_shift *a,
                               const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

/* addi SrcL.<S>, uimm, {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_srcl_imm_dst(DisasContext *ctx, arg_simt_arith_i *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %d%s", srcl_name, a->imm, dest_name);
}

static bool trans_simt_addli_32(DisasContext *ctx, arg_simt_addi_32 *a)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, "addli ", "%s, %d%s", srcl_name, a->imm, dest_name);
    return true;
}


/* srli SrcL.<S>, shamt, {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_srcl_shamt_dst(DisasContext *ctx, arg_simt_shift_i *a,
                                     const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %d%s", srcl_name, a->shamt, dest_name);
}

/* cmp.eq SrcL.<S>, SrcR.<S>, {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_srcl_srcr_au_dst(DisasContext *ctx, arg_simt_shift *a,
                                       const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

/* cmp.or SrcL.<S>, SrcR.<S>, {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_srcl_srcr_lu_dst(DisasContext *ctx, arg_simt_shift *a,
                                       const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

/* l.bxs SrcL.<T>, M, N, ->Dst.<W> */
static void
print_block_insn_fvec_srcl_m_n_dst(DisasContext *ctx, arg_simt_bo *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %d, %d%s", srcl_name, a->M, a->N + 1, dest_name);
}

/* l.rev SrcL.<T>, M, N, ->Dst.<W> */
static void
print_block_insn_fvec_rev_srcl_m_n_dst(DisasContext *ctx, arg_simt_bo *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %d, %d%s", srcl_name, a->M + 1, a->N, dest_name);
}

/* madd SrcL.<S>, SrcR.<S>, SrcD.<S>, {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_srcd_srcl_srcr_dst(DisasContext *ctx, arg_simt_madd *a,
                                    const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *srcd_name = get_fvec_src_reg_name(ctx, a->SrcD, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s, %s%s", srcl_name, srcr_name,
           srcd_name, dest_name);
}

/* lb [SrcL.d, SrcR.<T><<<shamt>], ->{t,u,m,n}.<W> */
static void
print_block_insn_fvec_load_srcl_srcr_au_shamt_dst(DisasContext *ctx,
                                                  arg_simt_load *a,
                                                  const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "[%s, %s<<%d]%s", srcl_name, srcr_name,
               a->shamt, dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "[%s, %s]%s", srcl_name, srcr_name,
               dest_name);
    }
}

/* lbi [SrcL, simm], {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_load_srcl_imm_dst(DisasContext *ctx, arg_simt_load_i *a,
                                   const char *mnemonic, int shift)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "[%s, %d]%s", srcl_name, (a->imm << shift),
           dest_name);
}

/* sb SrcD.<T>, [SrcL.d, SrcR.<T>] */
static void
print_block_insn_fvec_srcd_srcl_srcr_au(DisasContext *ctx,
    arg_simt_store *a, const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *srcd_name = get_fvec_src_reg_name(ctx, a->SrcD, 0);

    OUTPUT(ctx, mnemonic, "%s, [%s, %s]", srcd_name, srcl_name,
           srcr_name);
}

/* sbi SrcL.<S>, [SrcR, simm] */
static void
print_block_insn_fvec_store_srcl_srcr_imm(DisasContext *ctx,
    arg_simt_store_i *a, const char *mnemonic, int shift)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);

    OUTPUT(ctx, mnemonic, "%s, [%s, %d]", srcl_name, srcr_name,
           (a->imm << shift));
}

/* lb [SrcL.d, lc0<<<1,2,3>, SrcR.<T><<<shamt>], ->{t,u,m,n}.<W> */
static void
print_block_insn_fvec_load_srcl_lc0_srcr_au_shamt_dst(DisasContext *ctx,
    arg_simt_load *a, const char *mnemonic, int shift_lc0)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    if (a->shamt && shift_lc0) {
        OUTPUT(ctx, mnemonic, "[%s, lc0<<%d, %s<<%d]%s", srcl_name, shift_lc0,
               srcr_name, a->shamt, dest_name);
    } else if (a->shamt && !shift_lc0) {
        OUTPUT(ctx, mnemonic, "[%s, lc0, %s<<%d]%s", srcl_name,
               srcr_name, a->shamt, dest_name);
    } else if (!a->shamt && shift_lc0) {
        OUTPUT(ctx, mnemonic, "[%s, lc0<<%d, %s]%s", srcl_name, shift_lc0,
               srcr_name, dest_name);
    } else if (!a->shamt && !shift_lc0) {
        OUTPUT(ctx, mnemonic, "[%s, lc0, %s]%s", srcl_name,
               srcr_name, dest_name);
    }
}

/* lbi [SrcL, lc0<<<1,2,3>, simm], {->t,->u,->m,->n}.<S> */
static void
print_block_insn_fvec_load_srcl_lc0_imm_dst(DisasContext *ctx,
    arg_simt_load_i *a, const char *mnemonic, int shift, int shift_lc0)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    if (shift_lc0) {
        OUTPUT(ctx, mnemonic, "[%s, lc0<<%d, %d]%s", srcl_name, shift_lc0,
               (a->imm << shift), dest_name);
    } else {
        OUTPUT(ctx, mnemonic, "[%s, lc0, %d]%s", srcl_name,
               (a->imm << shift), dest_name);
    }
}

/* sb SrcD.<T>, [SrcL.d, lc0<<<1,2,3>, SrcR.<T>] */
static void
print_block_insn_fvec_srcd_srcl_lc0_srcr_au(DisasContext *ctx,
    arg_simt_store *a, const char *mnemonic, int shift_lc0)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *srcd_name = get_fvec_src_reg_name(ctx, a->SrcD, 0);

    if (shift_lc0) {
        OUTPUT(ctx, mnemonic, "%s, [%s, lc0<<%d, %s]", srcd_name, srcl_name,
           shift_lc0, srcr_name);
    } else {
        OUTPUT(ctx, mnemonic, "%s, [%s, lc0, %s]", srcd_name, srcl_name,
           srcr_name);
    }
}

/* sbi SrcL.<S>, [SrcR, lc0<<<1,2,3>, simm] */
static void
print_block_insn_fvec_store_srcl_srcr_lc0_imm(DisasContext *ctx,
    arg_simt_store_i *a, const char *mnemonic, int shift, int shift_lc0)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);

    if (shift_lc0) {
        OUTPUT(ctx, mnemonic, "%s, [%s, lc0<<%d, %d]", srcl_name, srcr_name,
           shift_lc0, (a->imm << shift));
    } else {
        OUTPUT(ctx, mnemonic, "%s, [%s, lc0, %d]", srcl_name, srcr_name,
           (a->imm << shift));
    }
}

/* fadd SrcL.<T>, SrcR.<T>, ->{t,u,m,n}.<W> */
static void
print_block_insn_fvec_fp_srcl_srcr_dst(DisasContext *ctx, arg_simt_shift *a,
                                       const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 1);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 1);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

static void
print_block_insn_fvec_fp_srcl_srcr_dst_vlen_rm_sat(
    DisasContext *ctx, arg_simt_fp1 *a, const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 1);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 1);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

static void
print_block_insn_fvec_fp_srcl_srcr_dst_vlen(DisasContext *ctx, arg_simt_fp2 *a,
                                       const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 1);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 1);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s%s", srcl_name, srcr_name, dest_name);
}

/* fmadd SrcL.<T>, SrcR.<T>, srcA.<T>, ->{t,u,m,n}.<W> */
static void
print_block_insn_fvec_fp_srcl_srcr_srca_dst(DisasContext *ctx, arg_simt_fp *a,
                                     const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 1);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 1);
    const char *srca_name = get_fvec_src_reg_name(ctx, a->SrcA, 1);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s, %s%s", srcl_name, srcr_name,
           srca_name, dest_name);
}

/* fabs SrcL.<T>, ->{t,u,m,n}.<W> */
static void
print_block_insn_fvec_fp_srcl_dst(DisasContext *ctx, arg_simt_onesrc1 *a,
                                  const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 1);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s%s", srcl_name, dest_name);
}

/* l.csel SrcP.<T>, SrcL.<T>, SrcR.<T><.neg>, ->Dst.<W> */
static void
print_block_insn_fvec_srcp_srcl_srcr_dst(DisasContext *ctx, arg_simt_csel *a,
                                         const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *srcp_name = get_fvec_src_reg_name(ctx, a->SrcP, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s, %s%s%s", srcp_name, srcl_name,
           srcr_name, srcr_type, dest_name);
}

static bool trans_simt_psel_32(DisasContext *ctx, arg_simt_csel_32 * a)
{
    print_block_insn_fvec_srcp_srcl_srcr_dst(ctx, a, "srcp_srcl_srcr_dst");
    return true;
}

/* l.shfl.xx SrcL.<T>, SrcR.<T>, SrcD.<T>, ->Dst.<W> */
static void
print_block_insn_fvec_srcl_srcr_srcd_dst(DisasContext *ctx, arg_simt_shfl *a,
                                         const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *srcd_name = get_fvec_src_reg_name(ctx, a->SrcD, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s, %s%s", srcl_name, srcr_name, srcd_name,
           dest_name);
}

/* fcvt SrcL.<srcT>, ->{t,u,m,n}.<dstT> */
static void
print_block_insn_fvec_fp_cvt_srcl_dst(DisasContext *ctx, arg_simt_cvt *a,
                                      const char *mnemonic)
{
    const char *srcl_name, *dest_name;

    if (*mnemonic == 'f') {
        srcl_name = get_fvec_cvt_src_reg_name(ctx, a->SrcL, 1, a->SrcType);
    } else {
        srcl_name = get_fvec_cvt_src_reg_name(ctx, a->SrcL, 0, a->SrcType);
    }

    if (strcompare(mnemonic, "fcvt") || strcompare(mnemonic, "icvtf")) {
        dest_name = get_fvec_cvt_dst_reg_name(ctx, a->RegDst, 1, a->DstType);
    } else {
        dest_name = get_fvec_cvt_dst_reg_name(ctx, a->RegDst, 0, a->DstType);
    }

    OUTPUT(ctx, mnemonic, "%s%s", srcl_name, dest_name);
}

#ifndef PRERELEASE
/* b.feq SrcL.<T>, SrcR.<T>, label */
/*
 * Note: There may then be other instructions that have the same type format,
 * but do not necessarily need to compute the jump address.
 */
static void print_block_insn_fvec_fp_branch_srcl_srcr_imm(
    DisasContext *ctx, arg_simt_branch *a, const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 1);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 1);
    uint64_t next = ctx->pc + a->br_offset;

    OUTPUT(ctx, mnemonic, "%s, %s, #0x%lx", srcl_name, srcr_name, next);
}
#endif

/* rdaddu srcL_v.<S>, ->gpr */
static void
print_block_insn_fvec_reduce_srcl_dst(DisasContext *ctx, arg_simt_onesrc *a,
                                      const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s%s", srcl_name, dest_name);
}

/* rdfadd srcL_v.<S>, ->gpr */
static void print_block_insn_fvec_reduce_srct_srcl_dst(DisasContext *ctx,
    arg_simt_onesrc *a, const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 1);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 1);

    OUTPUT(ctx, mnemonic, "%s%s", srcl_name, dest_name);
}

/* lw.add<A> [SrcL.{T}], SrcR.{T}, ->{t,u,m,n}.{W} */
static void print_block_insn_fvec_atomic_srcl_srcr_dst(DisasContext *ctx,
    arg_simt_atomic_ld *a, const char *mnemonic)
{
    /* 8 for {.aq,.rl,.ne,.aqrl,.neaq,.nerl,.neaqrl} */
    int len = strlen(mnemonic) + 8;
    char *inst_name = g_malloc(len);
    pstrcpy(inst_name, len, mnemonic);
    pstrcat(inst_name, len, ".");

    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    if (a->far) {
        pstrcat(inst_name, len, "far");
    } else if (a->aq) {
        pstrcat(inst_name, len, "aq");
    } else if (a->rl) {
        pstrcat(inst_name, len, "rl");
    }

    OUTPUT(ctx, mnemonic, "[%s], %s%s", srcl_name, srcr_name, dest_name);
}

/* sw.add<A> [SrcL.{T}], SrcR.{T} */
static void print_block_insn_fvec_atomic_srcl_srcr(DisasContext *ctx,
    arg_simt_atomic_st *a, const char *mnemonic)
{
    /* 8 for {.aq,.rl,.ne,.aqrl,.neaq,.nerl,.neaqrl} */
    int len = strlen(mnemonic) + 8;
    char *inst_name = g_malloc(len);
    pstrcpy(inst_name, len, mnemonic);
    pstrcat(inst_name, len, ".");

    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);

    if (a->rd) {
        pstrcat(inst_name, len, "rd");
    } else if (a->rl) {
        pstrcat(inst_name, len, "rl");
    }

    OUTPUT(ctx, mnemonic, "[%s], %s", srcl_name, srcr_name);
}

/* shfli.up SrcL.{T}, SrcR.{T}, offset, ->{t,u,m,n}.{W} */
static void
print_block_insn_fvec_imm_srcl_srcr_dst(DisasContext *ctx, arg_simt_shfli *a,
                                         const char *mnemonic)
{
    const char *srcl_name = get_fvec_src_reg_name(ctx, a->SrcL, 0);
    const char *srcr_name = get_fvec_src_reg_name(ctx, a->SrcR, 0);
    const char *dest_name = get_fvec_dst_reg_name(ctx, a->RegDst, 0);

    OUTPUT(ctx, mnemonic, "%s, %s, %d%s", srcl_name, srcr_name,
           a->imm, dest_name);
}

/* 48-bit inst */

/* hl.mul SrcL, SrcR, ->Dst0, Dst1 */
static void
print_block_insn_srcr_srcl_dst0_dst1(DisasContext *ctx, arg_arg_mul *a,
                                     const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg1_name(a->RegDst1);

    OUTPUT(ctx, mnemonic, "%s, %s%s%s",
           srcl_name, srcr_name, dest_name0, dest_name1);
}

/* hl.madd SrcL, SrcR, SrcD, ->Dst0, Dst1 */
static void
print_block_insn_srcd_srcr_srcl_dst0_dst1(DisasContext *ctx, arg_arg_mul *a,
                                          const char *mnemonic)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg1_name(a->RegDst1);

    OUTPUT(ctx, mnemonic, "%s, %s, %s%s%s",
           srcl_name, srcr_name, srcd_name, dest_name0, dest_name1);
}


/* hl.lb.pr [SrcL, SrcR<{.sw,.uw}><<<shamt>], ->Dst0, Dst1 */
static void
print_block_insn_shm_au_srcr_srcl_dst0_dst1(DisasContext *ctx, arg_arg_ld *a,
                                            const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg1_name(a->RegDst1);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "[%s, %s%s<<%d]%s%s", srcl_name, srcr_name,
               srcr_type, a->shamt, dest_name0, dest_name1);
    } else {
        OUTPUT(ctx, mnemonic, "[%s, %s%s]%s%s", srcl_name, srcr_name,
               srcr_type, dest_name0, dest_name1);
    }
}

/* hl.lbi.pr [SrcL, simm], ->Dst0, Dst1 */
static void
print_block_insn_imm_srcl_dst0_dst1(DisasContext *ctx, arg_arg_ldi *a,
                                   const char *mnemonic, int shift)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg1_name(a->RegDst1);

    OUTPUT(ctx, mnemonic, "[%s, %d]%s%s", srcl_name, (a->imm << shift),
           dest_name0, dest_name1);
}

/* hl.lbp.pr [SrcL, SrcR<{.sw,.uw}><<<shamt>], ->Dst0, Dst1, Dst2 */
static void print_block_insn_shm_au_srcr_srcl_dst0_dst1_dst2(DisasContext *ctx,
    arg_arg_ld *a, const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg1_name(a->RegDst1);
    const char *dest_name2 = get_dest_reg1_name(a->RegDst2);

    if (a->shamt) {
        OUTPUT(ctx, mnemonic, "[%s, %s%s<<%d]%s%s%s", srcl_name, srcr_name,
               srcr_type, a->shamt, dest_name0, dest_name1, dest_name2);
    } else {
        OUTPUT(ctx, mnemonic, "[%s, %s%s]%s%s%s", srcl_name, srcr_name,
               srcr_type, dest_name0, dest_name1, dest_name2);
    }
}

/* hl.lbip.pr [SrcL, simm], ->Dst0, Dst1, Dst2 */
static void
print_block_insn_imm_srcl_dst0_dst1_dst2(DisasContext *ctx, arg_arg_ldi *a,
                                   const char *mnemonic, int shift)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg1_name(a->RegDst1);
    const char *dest_name2 = get_dest_reg1_name(a->RegDst2);

    OUTPUT(ctx, mnemonic, "[%s, %d]%s%s%s", srcl_name, (a->imm << shift),
           dest_name0, dest_name1, dest_name2);
}

/* hl.sb.pr SrcD, [SrcL, SrcR<{.sw,.uw}>], ->{t, u, Rd} */
static void
print_block_insn_srcd_au_srcr_srcl_dst(DisasContext *ctx, arg_arg_sd *a,
                                       const char *mnemonic)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, [%s, %s%s]%s", srcd_name, srcl_name,
           srcr_name, srcr_type, dest_name);

}

/* hl.sbi.pr SrcD, [SrcR, simm], ->{t, u, Rd} */
static void
print_block_insn_imm_srcr_srcd_dst(DisasContext *ctx, arg_arg_sdi *a,
                                   const char *mnemonic, int shift)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, [%s, %d]%s", srcd_name, srcr_name,
           (a->imm << shift), dest_name);

}

static void
print_block_insn_imm_srcr_srcd_dst_raw(DisasContext *ctx, arg_arg_sdi_raw *a,
                                       const char *mnemonic, int shift)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, [%s, %d]%s", srcd_name, srcr_name,
           (a->imm << shift), dest_name);
}

/* hl.sbp.pr SrcD, SrcD1, [SrcL, SrcR<{.sw,.uw}>], ->{t, u, Rd} */
static void
print_block_insn_srcd_au_srcr_srcl_dst_srcd1(DisasContext *ctx, arg_arg_sdp *a,
                                            const char *mnemonic)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcd_name1 = get_src_reg_name(a->SrcD1);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcr_type = srctype_au[a->SrcRType];
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s, [%s, %s%s]%s", srcd_name, srcd_name1,
           srcl_name, srcr_name, srcr_type, dest_name);

}

/* hl.sbip.pr SrcD, SrcD1, [SrcR, simm], ->{t, u, Rd} */
static void
print_block_insn_imm_srcr_srcd_dst_srcd1(DisasContext *ctx, arg_arg_sdip *a,
                                         const char *mnemonic,  int shift)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcd_name1 = get_src_reg_name(a->SrcD1);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s, [%s, %d]%s", srcd_name, srcd_name1,
           srcr_name, (a->imm << shift), dest_name);

}

/* hl.sbip SrcD, SrcD1, [SrcR, simm] */
static void
print_block_insn_imm_srcr_srcd_srcd1(DisasContext *ctx,
                                     arg_arg_sdip_plain *a,
                                     const char *mnemonic, int shift)
{
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *srcd_name1 = get_src_reg_name(a->SrcD1);
    const char *srcr_name = get_src_reg_name(a->SrcR);

    OUTPUT(ctx, mnemonic, "%s, %s, [%s, %d]", srcd_name, srcd_name1,
           srcr_name, (a->imm << shift));
}

/* hl.lb.pcr [<symbol>], ->{t, u, Rd} */
static void
print_block_insn_ld_imm_dst(DisasContext *ctx, arg_arg_ld_pcr *a,
                                   const char *mnemonic)
{
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "[0x%lx]%s", (a->imm + ctx->pc), dest_name);
}

/* hl.sb.pcr SrcL, [<symbol>] */
static void
print_block_insn_sd_imm_srcl(DisasContext *ctx, arg_arg_sd_pcr *a,
                            const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);

    OUTPUT(ctx, mnemonic, "%s[0x%lx]", srcl_name, (a->imm + ctx->pc));
}

/* hl.casb<{.aq,.rl,.aqrl}> [SrcL], SrcR, SrcD, {->t, ->u, =>Rd} */
static void print_block_insn_atomic_srcd_srcl_srcr_dst(DisasContext *ctx,
    arg_arg_cas *a, const char *mnemonic)
{
    /* 6 for {.aq,.rl,.aqrl} */
    int len = strlen(mnemonic) + 8;
    char *inst_name = g_malloc(len);
    pstrcpy(inst_name, len, mnemonic);
    pstrcat(inst_name, len, ".");

    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *srcd_name = get_src_reg_name(a->SrcD);
    const char *dest_name = get_src_reg_name(a->RegDst);

    if (a->aq) {
        pstrcat(inst_name, len, "aq");
    } else if (a->rl) {
        pstrcat(inst_name, len, "rl");
    }

    OUTPUT(ctx, inst_name, "[%s], %s%s%s", srcl_name, srcr_name, srcd_name,
           dest_name);
    g_free(inst_name);
}

#define PRINT_LOAD_INST(NAME)                               \
static bool trans_simt_l_##NAME##_brg(DisasContext *ctx,    \
    arg_simt_l_##NAME##_brg * a)                             \
{                                                           \
    int len = strlen("l.l"#NAME".brg") + 8;                 \
    char *inst_name = g_malloc(len);                        \
    pstrcpy(inst_name, len, "l.l"#NAME".brg");              \
    const char *dest_name = get_src_reg_name(a->RegDst);    \
    const char *srcr = get_src_reg_name(a->SrcR);           \
    const char *srcl = get_src_reg_name(a->SrcL);           \
    OUTPUT(ctx, inst_name, "[%s%s<<%d] %s", srcr, srcl,     \
        a->shamt, dest_name);                               \
    g_free(inst_name);                                      \
    return true;                                            \
}

/* lb [SrcL SrcR << shamt ] -> RetDst */
PRINT_LOAD_INST(lb)
PRINT_LOAD_INST(lh)
PRINT_LOAD_INST(lw)
PRINT_LOAD_INST(ld)
PRINT_LOAD_INST(lbu)
PRINT_LOAD_INST(lhu)
PRINT_LOAD_INST(lwu)


#define PRINT_LOADI_INST(NAME, x)                               \
static bool trans_simt_l_##NAME##_##x(DisasContext *ctx,        \
    arg_simt_l_##NAME##_##x * a)                                \
{                                                               \
    int len = strlen("l.l"#NAME"."#x) + 8;                      \
    char *inst_name = g_malloc(len);                            \
    pstrcpy(inst_name, len, "l.l"#NAME"."#x);                   \
    const char *dest_name = get_src_reg_name(a->RegDst);        \
    const char *srcl = get_src_reg_name(a->SrcL);               \
    OUTPUT(ctx, inst_name, "[%s, %d] %s", srcl, a->imm, dest_name); \
    g_free(inst_name);                                          \
    return true;                                                \
}

PRINT_LOADI_INST(lbi, brg)
PRINT_LOADI_INST(lhi, brg)
PRINT_LOADI_INST(lwi, brg)
PRINT_LOADI_INST(ldi, brg)
PRINT_LOADI_INST(lbui, brg)
PRINT_LOADI_INST(lhui, brg)
PRINT_LOADI_INST(lwui, brg)

PRINT_LOADI_INST(lhi, ubrg)
PRINT_LOADI_INST(lwi, ubrg)
PRINT_LOADI_INST(ldi, ubrg)

PRINT_LOADI_INST(lhui, ubrg)
PRINT_LOADI_INST(lwui, ubrg)

#define PRINT_STORE_INST(NAME, x)                                \
static bool trans_simt_l_##NAME##_##x(DisasContext *ctx,   \
arg_simt_l_##NAME##_##x * a)                                \
{                                                               \
    int len = strlen("l.l"#NAME"."#x) + 8;                 \
    char *inst_name = g_malloc(len);                            \
    pstrcpy(inst_name, len, "l.l"#NAME"."#x);              \
    const char *srcl = get_src_reg_name(a->SrcL);               \
    const char *srcr = get_src_reg_name(a->SrcR);               \
    const char *srcd = get_src_reg_name(a->SrcD);               \
    OUTPUT(ctx, inst_name, "%s [%s, %s]", srcd, srcl, srcr);    \
    g_free(inst_name);                                          \
    return true;                                                \
}

PRINT_STORE_INST(sb, brg)
PRINT_STORE_INST(sh, brg)
PRINT_STORE_INST(sw, brg)
PRINT_STORE_INST(sd, brg)
PRINT_STORE_INST(sh, ubrg)
PRINT_STORE_INST(sw, ubrg)
PRINT_STORE_INST(sd, ubrg)

#define PRINT_STOREI_INST(NAME, x)                               \
static bool trans_simt_l_##NAME##_##x(DisasContext *ctx,   \
arg_simt_l_##NAME##_##x * a)                                \
{                                                               \
    int len = strlen("l.l"#NAME"."#x) + 8;                 \
    char *inst_name = g_malloc(len);                            \
    pstrcpy(inst_name, len, "l.l"#NAME"."#x);              \
    const char *srcl = get_src_reg_name(a->SrcL);               \
    const char *srcr = get_src_reg_name(a->SrcR);               \
    OUTPUT(ctx, inst_name, "%s [%s, %d]", srcl, srcr, a->imm);  \
    g_free(inst_name);                                          \
    return true;                                                \
}

PRINT_STOREI_INST(sbi, brg)
PRINT_STOREI_INST(shi, brg)
PRINT_STOREI_INST(swi, brg)
PRINT_STOREI_INST(sdi, brg)
PRINT_STOREI_INST(shi, ubrg)
PRINT_STOREI_INST(swi, ubrg)
PRINT_STOREI_INST(sdi, ubrg)

#define PRINT_L_INST_PCR(WIDTH)                             \
do {                                                        \
    int len = strlen("l.l"WIDTH".pcr") + 8;                 \
    char *inst_name = g_malloc(len);                        \
    pstrcpy(inst_name, len, "l.l"WIDTH".pcr");              \
    const char *dest_name = get_src_reg_name(a->RegDst);    \
    OUTPUT(ctx, inst_name, "0x%x%s", a->imm, dest_name);    \
    g_free(inst_name);                                      \
} while (0)

#define PRINT_S_INST_PCR(WIDTH)                             \
do {                                                        \
    int len = strlen("l.s"WIDTH".pcr") + 8;                 \
    char *inst_name = g_malloc(len);                        \
    pstrcpy(inst_name, len, "l.s"WIDTH".pcr");              \
    const char *srcl_name = get_src_reg_name(a->SrcL);      \
    OUTPUT(ctx, inst_name, "%s,0x%x", srcl_name, a->imm);   \
    g_free(inst_name);                                      \
} while (0)


static bool trans_l_lb_pcr(DisasContext *ctx, arg_l_lb_pcr *a)
{
    int len = strlen("l.lb.pcr") + 8;
    char *inst_name = g_malloc(len);
    pstrcpy(inst_name, len, "l.lb.pcr");
    const char *dest_name = get_src_reg_name(a->RegDst);
    OUTPUT(ctx, inst_name, "0x%x%s", a->imm, dest_name);
    g_free(inst_name);
    return true;
}

static bool trans_l_lh_pcr(DisasContext *ctx, arg_l_lh_pcr *a)
{
    PRINT_L_INST_PCR("h");
    return true;
}

static bool trans_l_lw_pcr(DisasContext *ctx, arg_l_lw_pcr *a)
{
    PRINT_L_INST_PCR("w");
    return true;
}

static bool trans_l_ld_pcr(DisasContext *ctx, arg_l_ld_pcr *a)
{
    PRINT_L_INST_PCR("d");
    return true;
}

static bool trans_l_lbu_pcr(DisasContext *ctx, arg_l_lbu_pcr *a)
{
    PRINT_L_INST_PCR("bu");
    return true;
}

static bool trans_l_lhu_pcr(DisasContext *ctx, arg_l_lhu_pcr *a)
{
    PRINT_L_INST_PCR("hu");
    return true;
}

static bool trans_l_lwu_pcr(DisasContext *ctx, arg_l_lwu_pcr *a)
{
    PRINT_L_INST_PCR("wu");
    return true;
}

static bool trans_l_sb_pcr(DisasContext *ctx, arg_l_sb_pcr *a)
{
    PRINT_S_INST_PCR("b");
    return true;
}

static bool trans_l_sh_pcr(DisasContext *ctx, arg_l_sh_pcr *a)
{
    PRINT_S_INST_PCR("h");
    return true;
}
static bool trans_l_sw_pcr(DisasContext *ctx, arg_l_sw_pcr *a)
{
    PRINT_S_INST_PCR("w");
    return true;
}
static bool trans_l_sd_pcr(DisasContext *ctx, arg_l_sd_pcr *a)
{
    PRINT_S_INST_PCR("d");
    return true;
}

#define PRINT_INST_CAS(x)                               \
do {                                                    \
    int len = strlen("l.cas"#x"p") + 8;                 \
    char *inst_name = g_malloc(len);                    \
    pstrcpy(inst_name, len, "l.cas"#x"p");              \
    const char *srcl = get_src_reg_name(a->SrcL);       \
    const char *srcr0 = get_src_reg_name(a->SrcR0);     \
    const char *srcr1 = get_src_reg_name(a->SrcR1);     \
    const char *srcd0 = get_src_reg_name(a->SrcD0);     \
    const char *srcd1 = get_src_reg_name(a->SrcD1);     \
    const char *dst0 = get_src_reg_name(a->RegDst0);        \
    const char *dst1 = get_src_reg_name(a->RegDst1);        \
    OUTPUT(ctx, inst_name,                                  \
    "aq:%d, rl:%d, far:%d [%s], %s, %s, %s, %s ->  %s, %s", \
    a->aq, a->rl, a->far, srcl, srcr0, srcr1, srcd0, srcd1, dst0, dst1);\
    g_free(inst_name);                                      \
} while (0)


static bool trans_l_casbp(DisasContext *ctx, arg_l_casbp *a)
{
    PRINT_INST_CAS(b);
    return true;
}

static bool trans_l_cashp(DisasContext *ctx, arg_l_cashp *a)
{
    PRINT_INST_CAS(h);
    return true;
}

static bool trans_l_caswp(DisasContext *ctx, arg_l_caswp *a)
{
    PRINT_INST_CAS(w);
    return true;
}

static bool trans_l_casdp(DisasContext *ctx, arg_l_casdp *a)
{
    PRINT_INST_CAS(d);
    return true;
}

/* hl.miadd SrcL, SrcR, uimm, ->{t, u, Rd} */
static void
print_block_insn_srcl_srcr_imm_dst(DisasContext *ctx,
    arg_arg_mi *a, const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s, %d%s", srcl_name, srcr_name,
           a->imm, dest_name);
}

/* hl.bfi SrcL, SrcR, M, N, ->{t, u, Rd} */
static void
print_block_insn_srcl_srcr_m_n_dst(DisasContext *ctx, arg_arg_bfi *a,
                              const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    OUTPUT(ctx, mnemonic, "%s, %s, %d, %d%s", srcl_name, srcr_name,
           a->M, a->N + 1, dest_name);
}

/* hl.ccat SrcL, SrcR, shamt, ->Dst0, Dst1 */
static void
print_block_insn_srcl_srcr_shamt_dst(DisasContext *ctx, arg_arg_ccat *a,
                                     const char *mnemonic)
{
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg_name(a->RegDst1);

    OUTPUT(ctx, mnemonic, "%s, %s, %d%s%s", srcl_name, srcr_name, a->shamt,
           dest_name0, dest_name1);
}


/* nop */
static void
print_block_insn_no_arg(DisasContext *ctx, arg_arg_empty *a,
                               const char *mnemonic)
{
    OUTPUT(ctx, mnemonic, "");
}

static void
print_block_insn_unknown(DisasContext *ctx, struct disassemble_info *info)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "(unknown)");
}

#define C_INSN(insn, mnemonic, format)                                         \
static bool trans_blk_##insn##_16(DisasContext *ctx, arg_blk_##insn##_16 * a)  \
{                                                                              \
    print_block_c_insn_##format(ctx, a, #mnemonic);                            \
    return true;                                                               \
}

#define SCALED_IMM_C_INSN(insn, mnemonic, shift, format)                       \
static bool trans_blk_##insn##_16(DisasContext *ctx, arg_blk_##insn##_16 * a)  \
{                                                                              \
    print_block_c_insn_##format(ctx, a, #mnemonic, shift);                     \
    return true;                                                               \
}

#define INSN(insn, mnemonic, format)                                           \
static bool trans_blk_##insn##_32(DisasContext *ctx, arg_blk_##insn##_32 * a)  \
{                                                                              \
    print_block_insn_##format(ctx, a, #mnemonic);                              \
    return true;                                                               \
}

#define HL_INSN(insn, mnemonic, format)                                        \
static bool trans_blk_##insn##_48(DisasContext *ctx, arg_blk_##insn##_48 * a)  \
{                                                                              \
    print_block_insn_##format(ctx, a, #mnemonic);                              \
    return true;                                                               \
}

#define IMM_HL_INSN(insn, mnemonic, format, shift)                             \
static bool trans_blk_##insn##_48(DisasContext *ctx, arg_blk_##insn##_48 * a)  \
{                                                                              \
    print_block_insn_##format(ctx, a, #mnemonic, shift);                       \
    return true;                                                               \
}

#define INSN_OFFSET(insn, mnemonic, format, src_offset, dst_offset)            \
static bool trans_blk_##insn##_32(DisasContext *ctx, arg_blk_##insn##_32 * a)  \
{                                                                              \
    print_block_insn_##format(ctx, a, #mnemonic, src_offset, dst_offset);      \
    return true;                                                               \
}

#define SIMT_INSN(insn, mnemonic, format)                                      \
static bool trans_simt_##insn##_32(DisasContext *ctx, arg_simt_##insn##_32 * a)\
{                                                                              \
    print_block_insn_fvec_##format(ctx, a, #mnemonic);                         \
    return true;                                                               \
}

#define SIMT_AND_NORMAL_INSN(insn, mnemonic, format)                           \
    SIMT_INSN(insn, mnemonic, format)                                          \
    INSN(insn, mnemonic, format)                                               \

#define SCALED_IMM_INSN(insn, mnemonic, shift, format)                         \
static bool trans_blk_##insn##_32(DisasContext *ctx, arg_blk_##insn##_32 * a)  \
{                                                                              \
    print_block_insn_##format(ctx, a, #mnemonic, shift);                       \
    return true;                                                               \
}

#define SIMT_LDST_LC0_INSN(insn, mnemonic, lc0_shift, format)                  \
static bool trans_simt_##insn##_32(DisasContext *ctx, arg_simt_##insn##_32 * a)\
{                                                                              \
    print_block_insn_fvec_##format(ctx, a, #mnemonic, lc0_shift);              \
    return true;                                                               \
}

#define SIMT_LDST_LC0_IMM_INSN(insn, mnemonic, imm_shift, lc0_shift, format)   \
static bool trans_simt_##insn##_32(DisasContext *ctx, arg_simt_##insn##_32 * a)\
{                                                                              \
    print_block_insn_fvec_##format(ctx, a, #mnemonic, imm_shift, lc0_shift);   \
    return true;                                                               \
}

#define SIMT_SCALED_IMM_INSN(insn, mnemonic, shift, format)                    \
static bool trans_simt_##insn##_32(DisasContext *ctx, arg_simt_##insn##_32 * a)\
{                                                                              \
    print_block_insn_fvec_##format(ctx, a, #mnemonic, shift);                  \
    return true;                                                               \
}                                                                              \
static bool trans_blk_##insn##_32(DisasContext *ctx, arg_blk_##insn##_32 * a)  \
{                                                                              \
    print_block_insn_##format(ctx, a, #mnemonic, shift);                       \
    return true;                                                               \
}

C_INSN(c_add, c.add, srcl_srcr)
C_INSN(c_sub, c.sub, srcl_srcr)
C_INSN(c_and, c.and, srcl_srcr)
C_INSN(c_or, c.or, srcl_srcr)

C_INSN(c_setc_eq, c.setc.eq, set_srcl_srcr)
C_INSN(c_setc_ne, c.setc.ne, set_srcl_srcr)

SCALED_IMM_C_INSN(c_lwi, c.lwi, 2, load_srcl_imm)
SCALED_IMM_C_INSN(c_ldi, c.ldi, 3, load_srcl_imm)
SCALED_IMM_C_INSN(c_swi, c.swi, 2, store_srcl_imm)
SCALED_IMM_C_INSN(c_sdi, c.sdi, 3, store_srcl_imm)

C_INSN(sext_b, sext.b, srcl)
C_INSN(sext_h, sext.h, srcl)
C_INSN(sext_w, sext.w, srcl)
C_INSN(zext_b, zext.b, srcl)
C_INSN(zext_h, zext.h, srcl)
C_INSN(zext_w, zext.w, srcl)

C_INSN(c_cmp_eqi, c.cmp.eqi, imm)
C_INSN(c_cmp_nei, c.cmp.nei, imm)
C_INSN(c_slli, c.slli, imm)
C_INSN(c_srli, c.srli, imm)

SIMT_AND_NORMAL_INSN(add, add, srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(sub, sub, srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(and, and, srcl_srcr_lu_shamt_dst)
SIMT_AND_NORMAL_INSN(or,  or,  srcl_srcr_lu_shamt_dst)
SIMT_AND_NORMAL_INSN(xor, xor, srcl_srcr_lu_shamt_dst)
SIMT_AND_NORMAL_INSN(srl, srl, srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(sra, sra, srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(sll, sll, srcl_srcr_dst)

SIMT_AND_NORMAL_INSN(addi, addi, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(subi, subi, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(andi, andi, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(ori,  ori,  srcl_imm_dst)
SIMT_AND_NORMAL_INSN(xori, xori, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(srli, srli, srcl_shamt_dst)
SIMT_AND_NORMAL_INSN(srai, srai, srcl_shamt_dst)
SIMT_AND_NORMAL_INSN(slli, slli, srcl_shamt_dst)

INSN(addw, addw, srcl_srcr_au_shamt_dst)
INSN(subw, subw, srcl_srcr_au_shamt_dst)
INSN(andw, andw, srcl_srcr_lu_shamt_dst)
INSN(orw, orw, srcl_srcr_lu_shamt_dst)
INSN(xorw, xorw, srcl_srcr_lu_shamt_dst)
INSN(srlw, srlw, srcl_srcr_dst)
INSN(sraw, sraw, srcl_srcr_dst)
INSN(sllw, sllw, srcl_srcr_dst)

INSN(addiw, addiw, srcl_imm_dst)
INSN(subiw, subiw, srcl_imm_dst)
INSN(andiw, andiw, srcl_imm_dst)
INSN(oriw, oriw, srcl_imm_dst)
INSN(xoriw, xoriw, srcl_imm_dst)
INSN(srliw, srliw, srcl_shamt_dst)
INSN(sraiw, sraiw, srcl_shamt_dst)
INSN(slliw, slliw, srcl_shamt_dst)

SIMT_AND_NORMAL_INSN(cmp_eq, cmp.eq, srcl_srcr_au_dst)
SIMT_AND_NORMAL_INSN(cmp_ne, cmp.ne, srcl_srcr_au_dst)
SIMT_AND_NORMAL_INSN(cmp_and, cmp.and, srcl_srcr_lu_dst)
SIMT_AND_NORMAL_INSN(cmp_or, cmp.or, srcl_srcr_lu_dst)
SIMT_AND_NORMAL_INSN(cmp_lt, cmp.lt, srcl_srcr_au_dst)
SIMT_AND_NORMAL_INSN(cmp_ge, cmp.ge, srcl_srcr_au_dst)
SIMT_AND_NORMAL_INSN(cmp_ltu, cmp.ltu, srcl_srcr_au_dst)
SIMT_AND_NORMAL_INSN(cmp_geu, cmp.geu, srcl_srcr_au_dst)

SIMT_AND_NORMAL_INSN(cmp_eqi, cmp.eqi, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(cmp_nei, cmp.nei, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(cmp_andi, cmp.andi, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(cmp_ori, cmp.ori, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(cmp_lti, cmp.lti, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(cmp_gei, cmp.gei, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(cmp_ltui, cmp.ltui, srcl_imm_dst)
SIMT_AND_NORMAL_INSN(cmp_geui, cmp.geui, srcl_imm_dst)

INSN(setc_eq, setc.eq, srcl_srcr_au)
INSN(setc_ne, setc.ne, srcl_srcr_au)
INSN(setc_and, setc.and, srcl_srcr_lu)
INSN(setc_or, setc.or, srcl_srcr_lu)
INSN(setc_lt, setc.lt, srcl_srcr_au)
INSN(setc_ge, setc.ge, srcl_srcr_au)
INSN(setc_ltu, setc.ltu, srcl_srcr_au)
INSN(setc_geu, setc.geu, srcl_srcr_au)

INSN(setc_eqi, setc.eqi, srcl_imm)
INSN(setc_nei, setc.nei, srcl_imm)
INSN(setc_andi, setc.andi, srcl_imm)
INSN(setc_ori, setc.ori, srcl_imm)
INSN(setc_lti, setc.lti, srcl_imm)
INSN(setc_gei, setc.gei, srcl_imm)
INSN(setc_ltui, setc.ltui, srcl_imm)
INSN(setc_geui, setc.geui, srcl_imm)

SIMT_AND_NORMAL_INSN(bxs, bxs, srcl_m_n_dst)
SIMT_AND_NORMAL_INSN(bxu, bxu, srcl_m_n_dst)
SIMT_AND_NORMAL_INSN(bic, bic, srcl_m_n_dst)
SIMT_AND_NORMAL_INSN(bis, bis, srcl_m_n_dst)

SIMT_AND_NORMAL_INSN(mul, mul, srcl_srcr_dst)
INSN(mulu, mulu, srcl_srcr_dst)
INSN(mulw, mulw, srcl_srcr_dst)
INSN(muluw, muluw, srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(madd, madd, srcd_srcl_srcr_dst)
INSN(maddw, maddw, srcd_srcl_srcr_dst)

INSN(addtpc, addtpc, imm_dst)
INSN(setret, setret, imm_dst)
INSN(lui, lui, imm_dst)

INSN(ssrget, ssrget, ssrget_ssr_id_dst)
INSN(ssrset, ssrset, ssrset_srcl_ssr_id)
INSN(ssrswap, ssrswap, ssrswap_ssr_id_srcl_dst)
INSN(lsrget, lsrget, ssrget_ssr_id_dst)

INSN(b_eq, b.eq, branch_srcl_srcr_imm)
INSN(b_ne, b.ne, branch_srcl_srcr_imm)
INSN(b_lt, b.lt, branch_srcl_srcr_imm)
INSN(b_ge, b.ge, branch_srcl_srcr_imm)
INSN(b_ltu, b.ltu, branch_srcl_srcr_imm)
INSN(b_geu, b.geu, branch_srcl_srcr_imm)
INSN(jr, jr, jr_srcl_imm)
INSN(setc_tgt, setc.tgt, lsrset_srcl)

INSN(j, j, j_imm)
INSN(b_z, b.z, j_imm)
INSN(b_nz, b.nz, j_imm)

SIMT_AND_NORMAL_INSN(lb, lb, load_srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(lh, lh, load_srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(lw, lw, load_srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(ld, ld, load_srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(lbu, lbu, load_srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(lhu, lhu, load_srcl_srcr_au_shamt_dst)
SIMT_AND_NORMAL_INSN(lwu, lwu, load_srcl_srcr_au_shamt_dst)
static bool trans_blk_prf_32(DisasContext *ctx, arg_blk_prf_32 *a)
{
    print_block_insn_prf32_srcl_srcr_au_shamt(ctx, a, "prf");
    return true;
}

SIMT_SCALED_IMM_INSN(lb_i, lbi, 0, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lh_i, lhi, 1, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lw_i, lwi, 2, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(ld_i, ldi, 3, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lbu_i, lbui, 0, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lhu_i, lhui, 1, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lwu_i, lwui, 2, load_srcl_imm_dst)

SIMT_SCALED_IMM_INSN(lh_ui, lhi.u, 0, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lw_ui, lwi.u, 0, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(ld_ui, ldi.u, 0, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lhu_ui, lhui.u, 0, load_srcl_imm_dst)
SIMT_SCALED_IMM_INSN(lwu_ui, lwui.u, 0, load_srcl_imm_dst)
static bool trans_blk_prfi_u_32(DisasContext *ctx, arg_blk_prfi_u_32 *a)
{
    print_block_insn_prf32_srcl_imm(ctx, a, "prfi.u");
    return true;
}

SIMT_LDST_LC0_INSN(lb_lc0, lb, 0, load_srcl_lc0_srcr_au_shamt_dst)
SIMT_LDST_LC0_INSN(lh_lc0, lh, 1, load_srcl_lc0_srcr_au_shamt_dst)
SIMT_LDST_LC0_INSN(lw_lc0, lw, 2, load_srcl_lc0_srcr_au_shamt_dst)
SIMT_LDST_LC0_INSN(ld_lc0, ld, 3, load_srcl_lc0_srcr_au_shamt_dst)
SIMT_LDST_LC0_INSN(lbu_lc0, lbu, 0, load_srcl_lc0_srcr_au_shamt_dst)
SIMT_LDST_LC0_INSN(lhu_lc0, lhu, 1, load_srcl_lc0_srcr_au_shamt_dst)
SIMT_LDST_LC0_INSN(lwu_lc0, lwu, 2, load_srcl_lc0_srcr_au_shamt_dst)

SIMT_LDST_LC0_IMM_INSN(lb_i_lc0, lbi, 0, 0, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lh_i_lc0, lhi, 1, 1, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lw_i_lc0, lwi, 2, 2, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(ld_i_lc0, ldi, 3, 3, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lbu_i_lc0, lbui, 0, 0, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lhu_i_lc0, lhui, 1, 1, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lwu_i_lc0, lwui, 2, 2, load_srcl_lc0_imm_dst)

SIMT_LDST_LC0_IMM_INSN(lh_ui_lc0, lhi.u, 0, 1, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lw_ui_lc0, lwi.u, 0, 2, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(ld_ui_lc0, ldi.u, 0, 3, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lhu_ui_lc0, lhui.u, 0, 1, load_srcl_lc0_imm_dst)
SIMT_LDST_LC0_IMM_INSN(lwu_ui_lc0, lwui.u, 0, 2, load_srcl_lc0_imm_dst)

INSN(lbl, lb.pcr, load_imm_dst)
INSN(lhl, lh.pcr, load_imm_dst)
INSN(lwl, lw.pcr, load_imm_dst)
INSN(ldl, ld.pcr, load_imm_dst)
INSN(lbul, lbu.pcr, load_imm_dst)
INSN(lhul, lhu.pcr, load_imm_dst)
INSN(lwul, lwu.pcr, load_imm_dst)

SIMT_AND_NORMAL_INSN(sb, sb, srcd_srcl_srcr_au)
SIMT_AND_NORMAL_INSN(sh, sh, srcd_srcl_srcr_au)
SIMT_AND_NORMAL_INSN(sw, sw, srcd_srcl_srcr_au)
SIMT_AND_NORMAL_INSN(sd, sd, srcd_srcl_srcr_au)
SIMT_AND_NORMAL_INSN(sh_u, sh.u, srcd_srcl_srcr_au)
SIMT_AND_NORMAL_INSN(sw_u, sw.u, srcd_srcl_srcr_au)
SIMT_AND_NORMAL_INSN(sd_u, sd.u, srcd_srcl_srcr_au)

SIMT_SCALED_IMM_INSN(sb_i, sbi, 0, store_srcl_srcr_imm)
SIMT_SCALED_IMM_INSN(sh_i, shi, 1, store_srcl_srcr_imm)
SIMT_SCALED_IMM_INSN(sw_i, swi, 2, store_srcl_srcr_imm)
SIMT_SCALED_IMM_INSN(sd_i, sdi, 3, store_srcl_srcr_imm)
SIMT_SCALED_IMM_INSN(sh_ui, shi.u, 0, store_srcl_srcr_imm)
SIMT_SCALED_IMM_INSN(sw_ui, swi.u, 0, store_srcl_srcr_imm)
SIMT_SCALED_IMM_INSN(sd_ui, sdi.u, 0, store_srcl_srcr_imm)

SIMT_LDST_LC0_INSN(sb_lc0, sb, 0, srcd_srcl_lc0_srcr_au)
SIMT_LDST_LC0_INSN(sh_lc0, sh, 1, srcd_srcl_lc0_srcr_au)
SIMT_LDST_LC0_INSN(sw_lc0, sw, 2, srcd_srcl_lc0_srcr_au)
SIMT_LDST_LC0_INSN(sd_lc0, sd, 3, srcd_srcl_lc0_srcr_au)
SIMT_LDST_LC0_INSN(sh_u_lc0, sh.u, 1, srcd_srcl_lc0_srcr_au)
SIMT_LDST_LC0_INSN(sw_u_lc0, sw.u, 2, srcd_srcl_lc0_srcr_au)
SIMT_LDST_LC0_INSN(sd_u_lc0, sd.u, 3, srcd_srcl_lc0_srcr_au)

SIMT_LDST_LC0_IMM_INSN(sb_i_lc0, sbi, 0, 0, store_srcl_srcr_lc0_imm)
SIMT_LDST_LC0_IMM_INSN(sh_i_lc0, shi, 1, 1, store_srcl_srcr_lc0_imm)
SIMT_LDST_LC0_IMM_INSN(sw_i_lc0, swi, 2, 2, store_srcl_srcr_lc0_imm)
SIMT_LDST_LC0_IMM_INSN(sd_i_lc0, sdi, 3, 3, store_srcl_srcr_lc0_imm)
SIMT_LDST_LC0_IMM_INSN(sh_ui_lc0, shi.u, 0, 1, store_srcl_srcr_lc0_imm)
SIMT_LDST_LC0_IMM_INSN(sw_ui_lc0, swi.u, 0, 2, store_srcl_srcr_lc0_imm)
SIMT_LDST_LC0_IMM_INSN(sd_ui_lc0, sdi.u, 0, 3, store_srcl_srcr_lc0_imm)

INSN(sbl, sb.pcr, store_srcl_imm)
INSN(shl, sh.pcr, store_srcl_imm)
INSN(swl, sw.pcr, store_srcl_imm)
INSN(sdl, sd.pcr, store_srcl_imm)

SIMT_AND_NORMAL_INSN(div, div, srcl_srcr_dst)
INSN(divu, divu, srcl_srcr_dst)
INSN(divw, divw, srcl_srcr_dst)
INSN(divuw, divuw, srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(rem, rem, srcl_srcr_dst)
INSN(remu, remu, srcl_srcr_dst)
INSN(remw, remw, srcl_srcr_dst)
INSN(remuw, remuw, srcl_srcr_dst)

SIMT_AND_NORMAL_INSN(csel, csel, srcp_srcl_srcr_dst)

SIMT_AND_NORMAL_INSN(ctz, ctz, srcl_m_n_dst)
SIMT_AND_NORMAL_INSN(clz, clz, srcl_m_n_dst)
SIMT_AND_NORMAL_INSN(bcnt, bcnt, srcl_m_n_dst)
SIMT_AND_NORMAL_INSN(rev, rev, rev_srcl_m_n_dst)

INSN(bc_iva, bc.iva, srcl)
INSN(bc_iall, bc.iall, no_arg)
INSN(ic_iva, ic.iva, srcl)
INSN(ic_iall, ic.iall, no_arg)
INSN(dc_iva, dc.iva, srcl)
INSN(dc_iall, dc.iva, no_arg)
INSN(dc_cva, dc.cav, srcl)
INSN(dc_civa, dc.civa, srcl)
INSN(dc_isw, dc.isw, srcl)
INSN(dc_csw, dc.csw, srcl)
INSN(dc_cisw, dc.cisw, srcl)
INSN(dc_zva, dc.zva, srcl)
INSN(tc_ia, tc.ia, srcl)
INSN(tc_iv, tc.iv, srcl)
INSN(tc_iav, tc.iav, srcl)
INSN(tc_iall, tc.iall, no_arg)

INSN(bse, bse, exec_ctrl_srcl)
INSN(bwe, bwe, exec_ctrl_srcl)
INSN(bwi, bwi, exec_ctrl_srcl)
INSN(bwa, bwt, exec_ctrl_srcl)
INSN(assert, assert, exec_ctrl_srcl)

INSN(fence_d, fence.d, pred_succ)
INSN(fence_i, fence.i, no_arg)

INSN(acrc, acrc, request_type)
INSN(acre, acre, rra_type)

INSN(lr_b, lr.b, aq_rl_srcl_dst)
INSN(lr_h, lr.h, aq_rl_srcl_dst)
INSN(lr_w, lr.w, aq_rl_srcl_dst)
INSN(lr_d, lr.d, aq_rl_srcl_dst)
INSN(sc_b, sc.b, sc_aq_rl_srcl_srcr_dst)
INSN(sc_h, sc.h, sc_aq_rl_srcl_srcr_dst)
INSN(sc_w, sc.w, sc_aq_rl_srcl_srcr_dst)
INSN(sc_d, sc.d, sc_aq_rl_srcl_srcr_dst)
INSN(swap_b, swapb, atomic_srcl_srcr_dst)
INSN(swap_h, swaph, atomic_srcl_srcr_dst)
INSN(swap_w, swapw, atomic_srcl_srcr_dst)
INSN(swap_d, swapd, atomic_srcl_srcr_dst)

SIMT_AND_NORMAL_INSN(lw_add, lw.add, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(lw_and, lw.and, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(lw_or, lw.or, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(lw_xor, lw.xor, atomic_srcl_srcr_dst)
INSN(lw_smax, lw.smax, atomic_srcl_srcr_dst)
INSN(lw_smin, lw.smin, atomic_srcl_srcr_dst)
INSN(lw_umax, lw.umax, atomic_srcl_srcr_dst)
INSN(lw_umin, lw.umin, atomic_srcl_srcr_dst)
SIMT_INSN(lw_max, lw.max, atomic_srcl_srcr_dst)
SIMT_INSN(lw_min, lw.min, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(ld_add, ld.add, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(ld_and, ld.and, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(ld_or, ld.or, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(ld_xor, ld.xor, atomic_srcl_srcr_dst)
INSN(ld_smax, ld.smax, atomic_srcl_srcr_dst)
INSN(ld_smin, ld.smin, atomic_srcl_srcr_dst)
INSN(ld_umax, ld.umax, atomic_srcl_srcr_dst)
INSN(ld_umin, ld.umin, atomic_srcl_srcr_dst)
SIMT_INSN(ld_max, ld.max, atomic_srcl_srcr_dst)
SIMT_INSN(ld_min, ld.min, atomic_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(sw_add, sw.add, atomic_srcl_srcr)
SIMT_AND_NORMAL_INSN(sw_and, sw.and, atomic_srcl_srcr)
SIMT_AND_NORMAL_INSN(sw_or, sw.or, atomic_srcl_srcr)
SIMT_AND_NORMAL_INSN(sw_xor, sw.xor, atomic_srcl_srcr)
INSN(sw_smax, sw.smax, atomic_srcl_srcr)
INSN(sw_smin, sw.smin, atomic_srcl_srcr)
INSN(sw_umax, sw.umax, atomic_srcl_srcr)
INSN(sw_umin, sw.umin, atomic_srcl_srcr)
SIMT_INSN(sw_max, sw.max, atomic_srcl_srcr)
SIMT_INSN(sw_min, sw.min, atomic_srcl_srcr)
SIMT_AND_NORMAL_INSN(sd_add, sd.add, atomic_srcl_srcr)
SIMT_AND_NORMAL_INSN(sd_and, sd.and, atomic_srcl_srcr)
SIMT_AND_NORMAL_INSN(sd_or, sd.or, atomic_srcl_srcr)
SIMT_AND_NORMAL_INSN(sd_xor, sd.xor, atomic_srcl_srcr)
INSN(sd_smax, sd.smax, atomic_srcl_srcr)
INSN(sd_smin, sd.smin, atomic_srcl_srcr)
INSN(sd_umax, sd.umax, atomic_srcl_srcr)
INSN(sd_umin, sd.umin, atomic_srcl_srcr)
INSN(dma, dma, dma_srcl_srcr)

SIMT_INSN(sd_max, sd.max, atomic_srcl_srcr)
SIMT_INSN(sd_min, sd.min, atomic_srcl_srcr)

INSN(max, max, srcl_srcr_dst)
INSN(min, min, srcl_srcr_dst)
INSN(maxu, maxu, srcl_srcr_dst)
INSN(minu, minu, srcl_srcr_dst)

INSN_OFFSET(fcvt, fcvt, dst_tp_src_tp_srcl_srcr_dst, FCVT_TYPE, FCVT_TYPE)
INSN_OFFSET(fcvta, fcvta, dst_tp_src_tp_srcl_srcr_dst, FCVT_TYPE, UCVT_TYPE)
INSN_OFFSET(fcvtm, fcvtm, dst_tp_src_tp_srcl_srcr_dst, FCVT_TYPE, UCVT_TYPE)
INSN_OFFSET(fcvtn, fcvtn, dst_tp_src_tp_srcl_srcr_dst, FCVT_TYPE, UCVT_TYPE)
INSN_OFFSET(fcvtp, fcvtp, dst_tp_src_tp_srcl_srcr_dst, FCVT_TYPE, UCVT_TYPE)
INSN_OFFSET(fcvtz, fcvtz, dst_tp_src_tp_srcl_srcr_dst, FCVT_TYPE, UCVT_TYPE)
INSN_OFFSET(scvtf, scvtf, dst_tp_src_tp_srcl_srcr_dst, SCVT_TYPE, FCVT_TYPE)
INSN_OFFSET(ucvtf, ucvtf, dst_tp_src_tp_srcl_srcr_dst, UCVT_TYPE, FCVT_TYPE)

INSN(fabs, fabs, fp_srcl_dst)
INSN(fsqrt, fsqrt, fp_srcl_dst)
INSN(fexp, fexp, fp_srcl_dst)
INSN(frecip, frecip, fp_srcl_dst)


INSN(fadd, fadd, fp_srcl_srcr_dst)
INSN(fsub, fsub, fp_srcl_srcr_dst)
INSN(fmul, fmul, fp_srcl_srcr_dst)
INSN(fdiv, fdiv, fp_srcl_srcr_dst)


/* SIMT block */
SIMT_INSN(fadd, fadd, fp_srcl_srcr_dst_vlen_rm_sat)
SIMT_INSN(fsub, fsub, fp_srcl_srcr_dst_vlen_rm_sat)
SIMT_INSN(fmul, fmul, fp_srcl_srcr_dst_vlen_rm_sat)
SIMT_INSN(fdiv, fdiv, fp_srcl_srcr_dst_vlen_rm_sat)

SIMT_AND_NORMAL_INSN(fmadd, fmadd, fp_srcl_srcr_srca_dst)
SIMT_AND_NORMAL_INSN(fmsub, fmsub, fp_srcl_srcr_srca_dst)
SIMT_AND_NORMAL_INSN(fnmadd, fnmadd, fp_srcl_srcr_srca_dst)
SIMT_AND_NORMAL_INSN(fnmsub, fnmsub, fp_srcl_srcr_srca_dst)

SIMT_AND_NORMAL_INSN(feq, feq, fp_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(fne, fne, fp_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(flt, flt, fp_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(fge, fge, fp_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(feqs, feqs, fp_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(fnes, fnes, fp_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(flts, flts, fp_srcl_srcr_dst)
SIMT_AND_NORMAL_INSN(fges, fges, fp_srcl_srcr_dst)

SIMT_INSN(fabs, fabs, fp_srcl_dst)
SIMT_INSN(fsqrt, fsqrt, fp_srcl_dst)
SIMT_INSN(fexp, fexp, fp_srcl_dst)
SIMT_INSN(frecip, frecip, fp_srcl_dst)
SIMT_INSN(fclass, fclass, fp_srcl_dst)

SIMT_INSN(max, max, fp_srcl_srcr_dst)
SIMT_INSN(min, min, fp_srcl_srcr_dst)

INSN(fmax, fmax, fp_srcl_srcr_dst)
INSN(fmin, fmin, fp_srcl_srcr_dst)

SIMT_INSN(fmax, fmax, fp_srcl_srcr_dst_vlen)
SIMT_INSN(fmin, fmin, fp_srcl_srcr_dst_vlen)

SIMT_INSN(fcvt, fcvt, fp_cvt_srcl_dst)
SIMT_INSN(fcvti, fcvti, fp_cvt_srcl_dst)
SIMT_INSN(icvt, icvt, fp_cvt_srcl_dst)
SIMT_INSN(icvtf, icvtf, fp_cvt_srcl_dst)

#ifndef PRERELEASE
SIMT_INSN(b_feq, b.feq, fp_branch_srcl_srcr_imm)
SIMT_INSN(b_fne, b.fne, fp_branch_srcl_srcr_imm)
SIMT_INSN(b_flt, b.flt, fp_branch_srcl_srcr_imm)
SIMT_INSN(b_fge, b.fge, fp_branch_srcl_srcr_imm)
#endif

SIMT_INSN(mov, mov, reduce_srcl_dst)
SIMT_INSN(rdadd, rdadd, reduce_srcl_dst)
SIMT_INSN(rdand, rdand, reduce_srcl_dst)
SIMT_INSN(rdor, rdor, reduce_srcl_dst)
SIMT_INSN(rdxor, rdxor, reduce_srcl_dst)
SIMT_INSN(rdfadd, rdfadd, reduce_srct_srcl_dst)

SIMT_INSN(rdmax, rdmax, reduce_srcl_dst)
SIMT_INSN(rdmin, rdmin, reduce_srcl_dst)
SIMT_INSN(rdfmax, rdfmax, reduce_srct_srcl_dst)
SIMT_INSN(rdfmin, rdfmin, reduce_srct_srcl_dst)

SIMT_INSN(shfl_up, shfl.up, srcl_srcr_srcd_dst)
SIMT_INSN(shfl_down, shfl.down, srcl_srcr_srcd_dst)
SIMT_INSN(shfl_bfly, shfl.bfly, srcl_srcr_srcd_dst)
SIMT_INSN(shfl_idx, shfl.idx, srcl_srcr_srcd_dst)

SIMT_INSN(shfli_up, shfli.up, imm_srcl_srcr_dst)
SIMT_INSN(shfli_down, shfli.down, imm_srcl_srcr_dst)
SIMT_INSN(shfli_bfly, shfli.bfly, imm_srcl_srcr_dst)
SIMT_INSN(shfli_idx, shfli.idx, imm_srcl_srcr_dst)

HL_INSN(lui, hl.lui, imm_dst1)
static bool trans_blk_addtpc_48(DisasContext *ctx, arg_blk_addtpc_48 *a)
{
    print_block_insn_imm_dst1(ctx, a, (a->RegDst == 10) ? "hl.setret" : "hl.addtpc");
    return true;
}
HL_INSN(addi, hl.addi, hl_srcl_imm_dst)
HL_INSN(subi, hl.subi, hl_srcl_imm_dst)
HL_INSN(andi, hl.andi, hl_srcl_imm_dst)
HL_INSN(ori, hl.ori, hl_srcl_imm_dst)
HL_INSN(xori, hl.xori, hl_srcl_imm_dst)
HL_INSN(addiw, hl.addiw, hl_srcl_imm_dst)
HL_INSN(subiw, hl.subiw, hl_srcl_imm_dst)
HL_INSN(andiw, hl.andiw, hl_srcl_imm_dst)
HL_INSN(oriw, hl.oriw, hl_srcl_imm_dst)
HL_INSN(xoriw, hl.xoriw, hl_srcl_imm_dst)
HL_INSN(cmp_eqi, hl.cmp.eqi, hl_srcl_imm_dst)
HL_INSN(cmp_nei, hl.cmp.nei, hl_srcl_imm_dst)
HL_INSN(cmp_andi, hl.cmp.andi, hl_srcl_imm_dst)
HL_INSN(cmp_ori, hl.cmp.ori, hl_srcl_imm_dst)
HL_INSN(cmp_lti, hl.cmp.lti, hl_srcl_imm_dst)
HL_INSN(cmp_gei, hl.cmp.gei, hl_srcl_imm_dst)
HL_INSN(cmp_ltui, hl.cmp.ltui, hl_srcl_imm_dst)
HL_INSN(cmp_geui, hl.cmp.geui, hl_srcl_imm_dst)
HL_INSN(setc_eqi, hl.setc.eqi, hl_srcl_imm)
HL_INSN(setc_nei, hl.setc.nei, hl_srcl_imm)
HL_INSN(setc_andi, hl.setc.andi, hl_srcl_imm)
HL_INSN(setc_ori, hl.setc.ori, hl_srcl_imm)
HL_INSN(setc_lti, hl.setc.lti, hl_srcl_imm)
HL_INSN(setc_gei, hl.setc.gei, hl_srcl_imm)
HL_INSN(setc_ltui, hl.setc.ltui, hl_srcl_imm)
HL_INSN(setc_geui, hl.setc.geui, hl_srcl_imm)
HL_INSN(mul, hl.mul, srcr_srcl_dst0_dst1)
HL_INSN(mulu, hl.mulu, srcr_srcl_dst0_dst1)
HL_INSN(madd, hl.madd, srcd_srcr_srcl_dst0_dst1)
HL_INSN(maddw, hl.maddw, srcd_srcr_srcl_dst0_dst1)
HL_INSN(div, hl.div, srcr_srcl_dst0_dst1)
HL_INSN(divu, hl.divu, srcr_srcl_dst0_dst1)
HL_INSN(divw, hl.divw, srcr_srcl_dst0_dst1)
HL_INSN(divuw, hl.divuw, srcr_srcl_dst0_dst1)
HL_INSN(rem, hl.rem, srcr_srcl_dst0_dst1)
HL_INSN(remu, hl.remu, srcr_srcl_dst0_dst1)
HL_INSN(remw, hl.remw, srcr_srcl_dst0_dst1)
HL_INSN(remuw, hl.remuw, srcr_srcl_dst0_dst1)

/* Load Pre-Index */
HL_INSN(lb_pr, hl.lb.pr, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lh_pr, hl.lh.pr, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lw_pr, hl.lw.pr, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(ld_pr, hl.ld.pr, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lbu_pr, hl.lbu.pr, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lhu_pr, hl.lhu.pr, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lwu_pr, hl.lwu.pr, shm_au_srcr_srcl_dst0_dst1)

IMM_HL_INSN(lbi_pr, hl.lbi.pr, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhi_pr, hl.lhi.pr, imm_srcl_dst0_dst1, 1)
IMM_HL_INSN(lwi_pr, hl.lwi.pr, imm_srcl_dst0_dst1, 2)
IMM_HL_INSN(ldi_pr, hl.ldi.pr, imm_srcl_dst0_dst1, 3)
IMM_HL_INSN(lbui_pr, hl.lbui.pr, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhui_pr, hl.lhui.pr, imm_srcl_dst0_dst1, 1)
IMM_HL_INSN(lwui_pr, hl.lwui.pr, imm_srcl_dst0_dst1, 2)

IMM_HL_INSN(lhi_upr, hl.lhi.upr, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lwi_upr, hl.lwi.upr, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(ldi_upr, hl.ldi.upr, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhui_upr, hl.lhui.upr, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lwui_upr, hl.lwui.upr, imm_srcl_dst0_dst1, 0)

/* Load Post-Index */
HL_INSN(lb_po, hl.lb.po, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lh_po, hl.lh.po, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lw_po, hl.lw.po, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(ld_po, hl.ld.po, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lbu_po, hl.lbu.po, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lhu_po, hl.lhu.po, shm_au_srcr_srcl_dst0_dst1)
HL_INSN(lwu_po, hl.lwu.po, shm_au_srcr_srcl_dst0_dst1)

IMM_HL_INSN(lbi_po, hl.lbi.po, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhi_po, hl.lhi.po, imm_srcl_dst0_dst1, 1)
IMM_HL_INSN(lwi_po, hl.lwi.po, imm_srcl_dst0_dst1, 2)
IMM_HL_INSN(ldi_po, hl.ldi.po, imm_srcl_dst0_dst1, 3)
IMM_HL_INSN(lbui_po, hl.lbui.po, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhui_po, hl.lhui.po, imm_srcl_dst0_dst1, 1)
IMM_HL_INSN(lwui_po, hl.lwui.po, imm_srcl_dst0_dst1, 2)

IMM_HL_INSN(lhi_upo, hl.lhi.upo, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lwi_upo, hl.lwi.upo, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(ldi_upo, hl.ldi.upo, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhui_upo, hl.lhui.upo, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lwui_upo, hl.lwui.upo, imm_srcl_dst0_dst1, 0)

/* Load Pair Pre-Index */
HL_INSN(lbp_pr, hl.lbp.pr, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lhp_pr, hl.lhp.pr, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lwp_pr, hl.lwp.pr, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(ldp_pr, hl.ldp.pr, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lbup_pr, hl.lbup.pr, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lhup_pr, hl.lhup.pr, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lwup_pr, hl.lwup.pr, shm_au_srcr_srcl_dst0_dst1_dst2)

IMM_HL_INSN(lbip, hl.lbip, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhip, hl.lhip, imm_srcl_dst0_dst1, 1)
IMM_HL_INSN(lwip, hl.lwip, imm_srcl_dst0_dst1, 2)
IMM_HL_INSN(ldip, hl.ldip, imm_srcl_dst0_dst1, 3)
IMM_HL_INSN(lbuip, hl.lbuip, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhuip, hl.lhuip, imm_srcl_dst0_dst1, 1)
IMM_HL_INSN(lwuip, hl.lwuip, imm_srcl_dst0_dst1, 2)
IMM_HL_INSN(lhip_u, hl.lhip.u, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lwip_u, hl.lwip.u, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(ldip_u, hl.ldip.u, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lhuip_u, hl.lhuip.u, imm_srcl_dst0_dst1, 0)
IMM_HL_INSN(lwuip_u, hl.lwuip.u, imm_srcl_dst0_dst1, 0)

/* Load Pair Post-Index */
HL_INSN(lbp_po, hl.lbp.po, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lhp_po, hl.lhp.po, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lwp_po, hl.lwp.po, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(ldp_po, hl.ldp.po, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lbup_po, hl.lbup.po, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lhup_po, hl.lhup.po, shm_au_srcr_srcl_dst0_dst1_dst2)
HL_INSN(lwup_po, hl.lwup.po, shm_au_srcr_srcl_dst0_dst1_dst2)

/* Store Pre-Index */
HL_INSN(sb_pr, hl.sb.pr, srcd_au_srcr_srcl_dst)
HL_INSN(sh_pr, hl.sh.pr, srcd_au_srcr_srcl_dst)
HL_INSN(sw_pr, hl.sw.pr, srcd_au_srcr_srcl_dst)
HL_INSN(sd_pr, hl.sd.pr, srcd_au_srcr_srcl_dst)
HL_INSN(sh_upr, hl.sh.upr, srcd_au_srcr_srcl_dst)
HL_INSN(sw_upr, hl.sw.upr, srcd_au_srcr_srcl_dst)
HL_INSN(sd_upr, hl.sd.upr, srcd_au_srcr_srcl_dst)

IMM_HL_INSN(sbi_pr, hl.sbi.pr, imm_srcr_srcd_dst, 0)
IMM_HL_INSN(shi_pr, hl.shi.pr, imm_srcr_srcd_dst, 1)
IMM_HL_INSN(swi_pr, hl.swi.pr, imm_srcr_srcd_dst, 2)
IMM_HL_INSN(sdi_pr, hl.sdi.pr, imm_srcr_srcd_dst, 3)
static bool trans_blk_sdi_pr_alt_48(DisasContext *ctx, arg_blk_sdi_pr_alt_48 *a)
{
    return trans_blk_sdi_pr_48(ctx, (arg_blk_sdi_pr_48 *)a);
}
IMM_HL_INSN(shi_upr, hl.shi.upr, imm_srcr_srcd_dst_raw, 0)
IMM_HL_INSN(swi_upr, hl.swi.upr, imm_srcr_srcd_dst_raw, 0)
IMM_HL_INSN(sdi_upr, hl.sdi.upr, imm_srcr_srcd_dst_raw, 0)
static bool trans_blk_sdi_upr_alt_48(DisasContext *ctx, arg_blk_sdi_upr_alt_48 *a)
{
    return trans_blk_sdi_upr_48(ctx, (arg_blk_sdi_upr_48 *)a);
}

/* Store Post-Index */
HL_INSN(sb_po, hl.sb.po, srcd_au_srcr_srcl_dst)
HL_INSN(sh_po, hl.sh.po, srcd_au_srcr_srcl_dst)
HL_INSN(sw_po, hl.sw.po, srcd_au_srcr_srcl_dst)
HL_INSN(sd_po, hl.sd.po, srcd_au_srcr_srcl_dst)
HL_INSN(sh_upo, hl.sh.upo, srcd_au_srcr_srcl_dst)
HL_INSN(sw_upo, hl.sw.upo, srcd_au_srcr_srcl_dst)
HL_INSN(sd_upo, hl.sd.upo, srcd_au_srcr_srcl_dst)

IMM_HL_INSN(sbi_po, hl.sbi.po, imm_srcr_srcd_dst, 0)
IMM_HL_INSN(shi_po, hl.shi.po, imm_srcr_srcd_dst, 1)
IMM_HL_INSN(swi_po, hl.swi.po, imm_srcr_srcd_dst, 2)
IMM_HL_INSN(sdi_po, hl.sdi.po, imm_srcr_srcd_dst, 3)
IMM_HL_INSN(shi_upo, hl.shi.upo, imm_srcr_srcd_dst_raw, 0)
IMM_HL_INSN(swi_upo, hl.swi.upo, imm_srcr_srcd_dst_raw, 0)
IMM_HL_INSN(sdi_upo, hl.sdi.upo, imm_srcr_srcd_dst_raw, 0)

/* Store Pair Pre-Index */
HL_INSN(sbp_pr, hl.sbp.pr, srcd_au_srcr_srcl_dst_srcd1)
HL_INSN(shp_pr, hl.shp.pr, srcd_au_srcr_srcl_dst_srcd1)
HL_INSN(swp_pr, hl.swp.pr, srcd_au_srcr_srcl_dst_srcd1)
HL_INSN(sdp_pr, hl.sdp.pr, srcd_au_srcr_srcl_dst_srcd1)
HL_INSN(shp_upr, hl.shp.upr, srcd_au_srcr_srcl_dst_srcd1)
HL_INSN(swp_upr, hl.swp.upr, srcd_au_srcr_srcl_dst_srcd1)
HL_INSN(sdp_upr, hl.sdp.upr, srcd_au_srcr_srcl_dst_srcd1)

IMM_HL_INSN(sbip, hl.sbip, imm_srcr_srcd_srcd1, 0)
IMM_HL_INSN(ship, hl.ship, imm_srcr_srcd_srcd1, 1)
IMM_HL_INSN(swip, hl.swip, imm_srcr_srcd_srcd1, 2)
IMM_HL_INSN(sdip, hl.sdip, imm_srcr_srcd_srcd1, 3)
IMM_HL_INSN(ship_u, hl.ship.u, imm_srcr_srcd_srcd1, 0)
IMM_HL_INSN(swip_u, hl.swip.u, imm_srcr_srcd_srcd1, 0)
IMM_HL_INSN(sdip_u, hl.sdip.u, imm_srcr_srcd_srcd1, 0)

HL_INSN(lb_pcr, hl.lb.pcr, ld_imm_dst)
HL_INSN(lh_pcr, hl.lh.pcr, ld_imm_dst)
HL_INSN(lw_pcr, hl.lw.pcr, ld_imm_dst)
HL_INSN(ld_pcr, hl.ld.pcr, ld_imm_dst)
HL_INSN(lbu_pcr, hl.lb.pcr, ld_imm_dst)
HL_INSN(lhu_pcr, hl.lh.pcr, ld_imm_dst)
HL_INSN(lwu_pcr, hl.lw.pcr, ld_imm_dst)

HL_INSN(sb_pcr, hl.sb.pcr, sd_imm_srcl)
HL_INSN(sh_pcr, hl.sh.pcr, sd_imm_srcl)
HL_INSN(sw_pcr, hl.sw.pcr, sd_imm_srcl)
HL_INSN(sd_pcr, hl.sd.pcr, sd_imm_srcl)

HL_INSN(prf, hl.prf, prf_srcl_srcr_au_shamt)
HL_INSN(prf_a, hl.prf.a, prf_srcl_srcr_au_shamt_dst)
HL_INSN(prfi_u, hl.prfi.u, prf_srcl_imm)
HL_INSN(prfi_ua, hl.prfi.ua, prf_srcl_imm_dst)

HL_INSN(ssrget, hl.ssrget, ssrget_ssr_id_dst)
HL_INSN(ssrset, hl.ssrset, ssrset_srcl_ssr_id)

HL_INSN(casb, hl.casb, atomic_srcd_srcl_srcr_dst)
HL_INSN(cash, hl.cash, atomic_srcd_srcl_srcr_dst)
HL_INSN(casw, hl.casw, atomic_srcd_srcl_srcr_dst)
HL_INSN(casd, hl.casd, atomic_srcd_srcl_srcr_dst)

HL_INSN(lis, hl.lis, imm_dst)
HL_INSN(liu, hl.liu, imm_dst)

HL_INSN(miadd, hl.miadd, srcl_srcr_imm_dst)
HL_INSN(misub, hl.misub, srcl_srcr_imm_dst)
HL_INSN(bfi, hl.bfi, srcl_srcr_m_n_dst)

HL_INSN(ccat, hl.ccat, srcl_srcr_shamt_dst)
HL_INSN(ccatw, hl.ccatw, srcl_srcr_shamt_dst)

INSN(invalid, invalid, no_arg)


static bool trans_blk_qmt_48(DisasContext *ctx, arg_blk_qmt_48 * a)
{
    char *inst_para = g_malloc0(4);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    if (a->i) {
        pstrcat(inst_para, 4, "i");
    }

    if (a->e) {
        pstrcat(inst_para, 4, "e");
    }

    if (a->s) {
        pstrcat(inst_para, 4, "s");
    }

    if (a->r) {
        pstrcat(inst_para, 4, "r");
    }

    if (strlen(inst_para) != 0) {
        OUTPUT(ctx, "hl.qmt.", "%s %s, %s%s", inst_para, srcl_name, srcr_name,
               dest_name);
    } else {
       OUTPUT(ctx, "hl.qmt", "%s, %s%s", srcl_name, srcr_name, dest_name);
    }

    g_free(inst_para);

    return true;
}

static bool trans_blk_qpush_48(DisasContext *ctx, arg_blk_qpush_48 *a)
{
    char *inst_para = g_malloc0(3);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *srcr_name = get_src_reg_name(a->SrcR);
    const char *dest_name = get_dest_reg_name(a->RegDst);

    if (a->h) {
        pstrcat(inst_para, 3, "h");
    }

    if (a->e) {
        pstrcat(inst_para, 3, "e");
    }

    if (a->r) {
        pstrcat(inst_para, 3, "r");
    }

    if (strlen(inst_para) != 0) {
        OUTPUT(ctx, "hl.qpush.", "%s %s, %s%s", inst_para, srcl_name, srcr_name,
               dest_name);
    } else {
       OUTPUT(ctx, "hl.qpush", "%s, %s%s", srcl_name, srcr_name, dest_name);
    }

    g_free(inst_para);
    return true;
}

/* hl.qpop.{e,r,er} SrcL, ->Dst0, Dst1 */
static bool trans_blk_qpop_48(DisasContext *ctx, arg_blk_qpop_48 *a)
{
    char *inst_para = g_malloc0(2);
    const char *srcl_name = get_src_reg_name(a->SrcL);
    const char *dest_name0 = get_dest_reg_name(a->RegDst0);
    const char *dest_name1 = get_dest_reg_name(a->RegDst1);

    if (a->e) {
        pstrcat(inst_para, 2, "e");
    }

    if (a->r) {
        pstrcat(inst_para, 2, "r");
    }

    if (strlen(inst_para) != 0) {
        OUTPUT(ctx, "hl.qpop.", "%s %s%s%s", inst_para, srcl_name, dest_name0,
               dest_name1);
    } else {
       OUTPUT(ctx, "hl.qpop", "%s%s%s", srcl_name, dest_name0, dest_name1);
    }

    g_free(inst_para);
    return true;
}


/* ============================ Block Header ============================ */

/* 16 bit 块头 */

/* c.bstart.type fall */
static bool trans_blk_short_head(DisasContext *ctx, arg_blk_short_head *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "C.%s %s",
    block_type_name(a->blk_type), next_type_str(a->br_type));
    return true;
}

/* C.B.DIMI imm, ->{LB0, LB1, LB2} */
static bool trans_blk_c_bdimi(DisasContext *ctx, arg_blk_c_bdimi *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream,
        "C.B.DIMI %d, ->LB%d", a->imm, a->loopnest);
    return true;
}

/* C.B.DIM RegSrc, ->{LB0, LB1, LB2} */
static bool trans_blk_c_bdim(DisasContext *ctx, arg_blk_c_bdim *a)
{
    const char *dst_name = get_src_reg_name(a->RegSrc);

    (ctx->info->fprintf_func)(ctx->info->stream,
        "C.B.DIM %s, ->LB%d", dst_name, a->loopnest);
    return true;
}

/* 32 bit 块头 */
#define PRINT_LINX_HEAD_NO_BNEXT(ctx, head_arg, branch_type, opcode)           \
do {                                                                           \
    (ctx->info->fprintf_func)(ctx->info->stream, "%s %s",                      \
    block_type_name(head_arg->blk_type), next_type_str(branch_type));          \
} while (0)

#define PRINT_LINX_HEAD_BNEXT(ctx, head_arg, branch_type, opcode)              \
do {                                                                           \
    int64_t bnext_off = 0;                                                     \
    bnext_off = head_arg->bnext_offset;                                        \
    (ctx->info->fprintf_func)(ctx->info->stream, "%s %s 0x%" PRIx64,           \
                              block_type_name(head_arg->blk_type),             \
                              next_type_str(branch_type),                      \
                              ctx->pc + bnext_off);                            \
} while (0)

static bool trans_blk_start_stop(DisasContext *ctx, arg_blk_start_stop *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "BSTOP");
    return true;
}

static bool trans_blk_start_simt(DisasContext *ctx, arg_blk_start_simt *a)
{
    uint32_t tileop_type = (a->para_op << T_OP_SFT) | a->para_func;
    switch (a->para_mode) {
    case 0: /* vector mode */
        (ctx->info->fprintf_func)(ctx->info->stream, "BSTART.VECT %s %s FALL",
            tileop_type_name(tileop_type),
            tileop_datatype_name(a->para_datatype));
        break;
    case 1: /* para mode */
        (ctx->info->fprintf_func)(ctx->info->stream, "BSTART.PAR %s %s FALL",
            tileop_type_name(tileop_type),
            tileop_datatype_name(a->para_datatype));
        break;
    default:
        break;
    }

    return true;
}

static bool trans_blk_start_fall(DisasContext *ctx, arg_blk_start_fall *a)
{
    PRINT_LINX_HEAD_NO_BNEXT(ctx, a, BRANCH_FALL, OPCODE_BSTART);
    if (a->fixup_label > 0)
        (ctx->info->fprintf_func)(ctx->info->stream, " 0x%" PRIx64,
                                  ctx->pc + a->fixup_label);
    return true;
}

static bool trans_blk_start_direct(DisasContext *ctx, arg_blk_start_direct *a)
{
    PRINT_LINX_HEAD_BNEXT(ctx, a, BRANCH_DIRECT_LINK, OPCODE_BSTART);
    return true;
}

static bool trans_blk_start_cond(DisasContext *ctx, arg_blk_start_cond *a)
{
    PRINT_LINX_HEAD_BNEXT(ctx, a, BRANCH_CONDITIONAL, OPCODE_BSTART);
    return true;
}

static bool trans_blk_start_call(DisasContext *ctx, arg_blk_start_call *a)
{
    PRINT_LINX_HEAD_BNEXT(ctx, a, BRANCH_CALL, OPCODE_BSTART);
    return true;
}

static bool trans_blk_start_ind(DisasContext *ctx, arg_blk_start_ind *a)
{
    PRINT_LINX_HEAD_NO_BNEXT(ctx, a, BRANCH_IND, OPCODE_BSTART);
    return true;
}

static bool trans_blk_start_icall(DisasContext *ctx, arg_blk_start_icall *a)
{
    PRINT_LINX_HEAD_NO_BNEXT(ctx, a, BRANCH_INDCALL, OPCODE_BSTART);
    return true;
}

static bool trans_blk_start_ret(DisasContext *ctx, arg_blk_start_ret *a)
{
    PRINT_LINX_HEAD_NO_BNEXT(ctx, a, BRANCH_RET, OPCODE_BSTART);
    return true;
}

static bool trans_blk_offset_direct_call(DisasContext *ctx,
                                         arg_blk_offset_direct_call *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream,
        "BSTART DIRECT/CALL, next:%lx", ctx->pc + a->bnext_low);
    return true;
}

static bool trans_blk_offset_cond(DisasContext *ctx, arg_blk_offset_cond *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream,
        "BSTART COND, next:%lx", ctx->pc + a->bnext_low);
    return true;
}

static bool trans_blk_offset_btext(DisasContext *ctx, arg_blk_offset_btext *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream,
        "B.TEXT %lx", ctx->pc + a->btext);
    return true;
}

/* b.ior [BGetList], [BSetList] */
static bool trans_blk_bior(DisasContext *ctx, arg_blk_bior *a)
{
    g_autoptr(GString) input_buf = g_string_new("");
    g_autoptr(GString) output_buf = g_string_new("");

    (ctx->info->fprintf_func)(ctx->info->stream,
        "B.IOR [%s][%s]", input_buf->str, output_buf->str);

    return true;
}

static bool trans_blk_b_arg_norm_32(DisasContext *ctx, arg_blk_b_arg_norm_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "B.ARG NORM.normal");
    return true;
}

static bool trans_blk_b_arg_format_32(DisasContext *ctx, arg_blk_b_arg_format_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "B.ARG %u", a->format);
    return true;
}

static bool trans_blk_b_arg_nz2dn_32(DisasContext *ctx, arg_blk_b_arg_nz2dn_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "B.ARG NZ2DN.canon");
    return true;
}

static bool trans_blk_b_arg_nd2zn_32(DisasContext *ctx, arg_blk_b_arg_nd2zn_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "B.ARG ND2ZN.normal, FP16, Null");
    return true;
}

static bool trans_blk_b_arg_dn2zn_32(DisasContext *ctx, arg_blk_b_arg_dn2zn_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "B.ARG DN2ZN.normal, FP16, Null");
    return true;
}

static bool trans_blk_b_arg_dn2nz_32(DisasContext *ctx, arg_blk_b_arg_dn2nz_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "B.ARG DN2NZ.normal, FP32, Null");
    return true;
}

const char biot_size[16][6] = {
    "0", "32B", "64B", "128B", "256B", "512B", "1KB", "2KB",
    "4KB", "8KB", "16KB", "32KB", "64KB", "128KB", "256KB", "512KB"
};

/* B.IOT [SrcTile0, SrcTile1], ->DstTile<Size> */
static bool trans_blk_biot1(DisasContext *ctx, arg_blk_biot1 *a)
{
    int len = 21;
    char *src_list = g_malloc0(len);
    const char *tile_name = tile_reg_dst_name[a->dsttile];
    const char *dst_name = biot_size[a->imm];

    pstrcpy(src_list, len, tile_reg_name[a->srctile0]);
    if (a->s0r) {
        pstrcpy(src_list, len, ".reuse");
    }

    pstrcat(src_list, len, ", ");
    pstrcpy(src_list, len, tile_reg_name[a->srctile1]);
    if (a->s1r) {
        pstrcpy(src_list, len, ".reuse");
    }

    if (a->dsttile != 0b111) {
        (ctx->info->fprintf_func)(ctx->info->stream,
            "B.IOT [%s], %s<%s>",
            src_list, tile_name, dst_name);
    } else {
        (ctx->info->fprintf_func)(ctx->info->stream,
            "B.IOT [%s]", src_list);
    }
    return true;
}

static bool trans_blk_biot2(DisasContext *ctx, arg_blk_biot2 *a)
{
    int len = 21;
    char *src_list = g_malloc0(len);
    const char *tile_name = tile_reg_dst_name[a->dsttile];
    const char *dst_name = biot_size[a->imm];

    pstrcpy(src_list, len, tile_reg_name[a->srctile0]);
    if (a->s0r) {
        pstrcpy(src_list, len, ".reuse");
    }

    if (a->dsttile != 0b111) {
        (ctx->info->fprintf_func)(ctx->info->stream,
            "B.IOT [%s] %s<%s>",
            src_list, tile_name, dst_name);
    } else {
        (ctx->info->fprintf_func)(ctx->info->stream,
            "B.IOT [%s]", src_list);
    }

    return true;
}

static bool trans_blk_biot3(DisasContext *ctx, arg_blk_biot3 *a)
{
    int len = 21;
    char *src_list = g_malloc0(len);
    const char *tile_name = tile_reg_dst_name[a->dsttile];
    const char *dst_name = biot_size[a->imm];
    if (a->dsttile != 0b111) {
        (ctx->info->fprintf_func)(ctx->info->stream,
            "B.IOT [%s] %s<%s>",
            src_list, tile_name, dst_name);
    } else {
        (ctx->info->fprintf_func)(ctx->info->stream,
            "B.IOT [%s]", src_list);
    }

    return true;
}

/* B.DIM RegSrc, imm, ->{LB0, LB1, LB2} */
static bool trans_blk_bdim_fixed(DisasContext *ctx, int regsrc, int imm, int loopnest)
{
    const char *dst_name = get_src_reg_name(regsrc);

    (ctx->info->fprintf_func)(ctx->info->stream,
        "B.DIM %s, %d, ->LB%d", dst_name, imm, loopnest);
    return true;
}

static bool trans_blk_bdim_lb0_32(DisasContext *ctx, arg_blk_bdim_lb0_32 *a)
{
    return trans_blk_bdim_fixed(ctx, a->RegSrc, a->imm, 0);
}

static bool trans_blk_bdim_lb1_32(DisasContext *ctx, arg_blk_bdim_lb1_32 *a)
{
    return trans_blk_bdim_fixed(ctx, a->RegSrc, a->imm, 1);
}

static bool trans_blk_bdim_lb2_32(DisasContext *ctx, arg_blk_bdim_lb2_32 *a)
{
    return trans_blk_bdim_fixed(ctx, a->RegSrc, a->imm, 2);
}

/* b.catr {trap, atomic, <.aq,.rl,.aqrl>, far} */
static bool trans_blk_catr(DisasContext *ctx, arg_blk_catr *a)
{
    uint32_t block_atomic = (a->far << 3) | (a->atom << 2) |
                            (a->aq << 1) | (a->rl);
    (ctx->info->fprintf_func)(ctx->info->stream, "B.CATR %s",
                              battr_type_str(block_atomic));
    if (a->dr) {
        (ctx->info->fprintf_func)(ctx->info->stream, ", dr");
    }
    if (a->trap) {
        (ctx->info->fprintf_func)(ctx->info->stream, ", trap");
    }
    return true;
}

/* b.datr {trap, atomic, <.aq,.rl,.aqrl>, far} */
static bool trans_blk_datr(DisasContext *ctx, arg_blk_datr *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "B.DATR ");
    char pad[4][6] = {"ZERO", "MAX", "MIN", "NULL"};
    char rmode[8][4] = {"RNE", "RTZ", "RDN", "RUP", "RNA", "RTO", "RHB"};

    (ctx->info->fprintf_func)(ctx->info->stream, ", %s", datr_cm_str(a->cm));
    (ctx->info->fprintf_func)(ctx->info->stream, ", %s ", pad[a->pad]);
    (ctx->info->fprintf_func)(ctx->info->stream, ", dtyp:%d ", a->dat_typ);
    (ctx->info->fprintf_func)(ctx->info->stream, ", %s ", rmode[a->rm]);
    if (a->c) {
        (ctx->info->fprintf_func)(ctx->info->stream, ", c");
    }
    if (a->sat) {
        (ctx->info->fprintf_func)(ctx->info->stream, ", sat");
    }
    const char *fmt = matrix_trans_format(a->layout);
    (ctx->info->fprintf_func)(ctx->info->stream, ", %s", fmt);
    return true;
}

/* B.HINT {BR.{likely, unlikely}, TEMP.{hot, warm, cool, cold}, PRFSIZE} */
static bool trans_blk_hint(DisasContext *ctx, arg_blk_hint *a)
{
    const char temp_str[4][5] = {
        "cold", "cool", "warm", "hot"
    };
    g_autoptr(GString) buf = g_string_new("");

    if (a->v) {
        g_string_append_printf(buf, "%s", "BR.");
        if (a->l_ul) {
            g_string_append_printf(buf, "%s, ", "likely");
        } else {
            g_string_append_printf(buf, "%s, ", "unlikely");
        }
    }

    g_string_append_printf(buf, "TEMP.%s, %d", temp_str[a->temp],
                           a->prefetch_size);
    (ctx->info->fprintf_func)(ctx->info->stream, "B.HINT %s", buf->str);

    return true;
}

/* B.HINT TRADE.{begin, end} */
static bool trans_blk_hint_trace(DisasContext *ctx, arg_blk_hint_trace *a)
{
    if (a->b_e) {
        (ctx->info->fprintf_func)(ctx->info->stream, "B.HINT TRACE.end");
    } else {
        (ctx->info->fprintf_func)(ctx->info->stream, "B.HINT TRACE.begin");
    }
    return true;
}

/* b.mcopy [RegSrc0, RegSrc1, RegSrc2] */
static bool trans_blk_memcopy(DisasContext *ctx, arg_blk_memcopy *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "%s",
        block_type_name(HEAD_TYPE_MCOPY));

    (ctx->info->fprintf_func)(ctx->info->stream, " [%s, %s, %s]",
        linx_src_reg_name[a->reg_src0],
        linx_src_reg_name[a->reg_src1],
        linx_src_reg_name[a->reg_src2]);
    return true;
}

/* b.mmov [RegSrc0, RegSrc1, RegSrc2] */
static bool trans_blk_memmove(DisasContext *ctx, arg_blk_memmove *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "%s",
        block_type_name(HEAD_TYPE_MMOVE));

    (ctx->info->fprintf_func)(ctx->info->stream, " [%s, %s, %s]",
        linx_src_reg_name[a->reg_src0],
        linx_src_reg_name[a->reg_src1],
        linx_src_reg_name[a->reg_src2]);
    return true;
}

/* b.mset [RegSrc0, RegSrc1, RegSrc2] */
static bool trans_blk_memset(DisasContext *ctx, arg_blk_memset *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "%s",
        block_type_name(HEAD_TYPE_MSET));

    (ctx->info->fprintf_func)(ctx->info->stream, " [%s, %s, %s]",
        linx_src_reg_name[a->reg_src0],
        linx_src_reg_name[a->reg_src1],
        linx_src_reg_name[a->reg_src2]);
    return true;
}

static bool trans_blk_esave_32(DisasContext *ctx, arg_blk_esave_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "ESAVE [%s, %s, %s]",
        linx_src_reg_name[a->reg_src0],
        linx_src_reg_name[a->reg_src1],
        linx_src_reg_name[a->reg_src2]);
    return true;
}

static bool trans_blk_ercov_32(DisasContext *ctx, arg_blk_ercov_32 *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "ERCOV [%s, %s, %s]",
        linx_src_reg_name[a->reg_src0],
        linx_src_reg_name[a->reg_src1],
        linx_src_reg_name[a->reg_src2]);
    return true;
}

/* b.iod DepSrc0, ->DepDst */
static bool trans_blk_biod(DisasContext *ctx, arg_blk_biod *a)
{
    const char *dep_src = dep_src_name[a->DepSrc0];
    const char *dep_dst = dep_dst_name[a->DepDst];

    (ctx->info->fprintf_func)(ctx->info->stream,
        "B.IOD %s%s", dep_src, dep_dst);
    return true;
}

/* EBREAK<.CMT> */
static bool trans_blk_ebreak(DisasContext *ctx, arg_blk_ebreak* a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "EBREAK %d", a->imm);
    return true;
}

/* c EBREAK */
static bool trans_blk_c_ebreak(DisasContext *ctx, arg_blk_c_ebreak* a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "C_EBREAK %d", a->imm);
    return true;
}

/* XB ARC-ID, C-ID */
static bool trans_blk_xb(DisasContext *ctx, arg_blk_xb *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "XB %d %d",
                                a->arc_id, a->cross_bid);
    return true;
}

/* f.entry [RegSrc0 ~ RegSrcn], sp!, uimm */
static bool trans_blk_fentry(DisasContext *ctx, arg_blk_fentry *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "%s",
        block_type_name(HEAD_TYPE_FENTRY));

    print_src(a->src_begin, a->src_end, ctx->info);
    (ctx->info->fprintf_func)(ctx->info->stream, ", sp!, %x", a->imm);
    return true;
}

/* f.exit [RegDst0 ~ RegDstn], sp!, uimm */
static bool trans_blk_fexit(DisasContext *ctx, arg_blk_fexit *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "%s",
        block_type_name(HEAD_TYPE_FEXIT));

    print_src(a->dst_begin, a->dst_end, ctx->info);
    (ctx->info->fprintf_func)(ctx->info->stream, ",  sp!, %x", a->imm);
    return true;
}

/* f.ret.ra [RegDst0 ~ RegDstn], sp!, uimm */
static bool trans_blk_fret_ra(DisasContext *ctx, arg_blk_fret_ra *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "%s",
        block_type_name(HEAD_TYPE_FRET_RA));

    print_src(a->dst_begin, a->dst_end, ctx->info);
    (ctx->info->fprintf_func)(ctx->info->stream, ",  sp!, %x", a->imm);
    return true;
}

/* f.ret.stk [RegDst0 ~ RegDstn], sp!, uimm */
static bool trans_blk_fret_stk(DisasContext *ctx, arg_blk_fret_stk *a)
{
    (ctx->info->fprintf_func)(ctx->info->stream, "%s",
        block_type_name(HEAD_TYPE_FRET_STK));

    print_src(a->dst_begin, a->dst_end, ctx->info);
    (ctx->info->fprintf_func)(ctx->info->stream, ",  sp!, %x", a->imm);
    return true;
}

static int get_inst_len(uint64_t opcode)
{
    int inst_size = 0;
    if (extract64(opcode, 0, 1) == 0) {
        if (extract16(opcode, 1, 3) == 0b111) {
            inst_size = 6;
        } else {
            inst_size = 2;
        }
    } else if (extract64(opcode, 0, 4) == 0b1111) {
        inst_size = 8;
    } else {
        inst_size = 4;
    }
    return inst_size;
}


#define MICRO_FMT_2 "%04"  PRIx16 "                "
#define MICRO_FMT_4 "%08"  PRIx32 "            "
#define MICRO_FMT_8 "%016" PRIx64 "    "

int print_insn_linx(bfd_vma memaddr, struct disassemble_info *info)
{
    int status = 0, head_size = 0;
    uint64_t inst;
    bfd_byte insn_8b[8];
    DisasContext ctx = {
        .info = info,
        .pc = memaddr
    };

    status = (*info->read_memory_func)(memaddr, insn_8b, 8, info);
    if (status != 0) {
        (*info->memory_error_func)(status, memaddr, info);
        return status;
    }

    inst = (uint64_t) bfd_getl64(insn_8b);
    head_size = get_inst_len(inst);

    if (head_size == 2) {/*  16 bit 指令 */
        (*info->fprintf_func)(info->stream, MICRO_FMT_2, (uint16_t)inst);
        if (!decode_block16(&ctx, (uint16_t)inst)) {
            print_block_insn_unknown(&ctx, info);
        }
    } else if (head_size == 6) {  /*  48 bit 指令 */
        (*info->fprintf_func)(info->stream, MICRO_FMT_8, inst);
        if (!decode_block48(&ctx, inst)) {
            print_block_insn_unknown(&ctx, info);
        }
    } else if (head_size == 4) {  /*  32 bit 指令 */
        (*info->fprintf_func)(info->stream, MICRO_FMT_4, (uint32_t)inst);
        if (!decode_block32(&ctx, (uint32_t)inst)) {
            print_block_insn_unknown(&ctx, info);
        }
    } else if (head_size == 8) {   /*  64 bit 指令 */
        (*info->fprintf_func)(info->stream, MICRO_FMT_8, inst);
        if (!decode_block32_private_fvec(&ctx, inst)) {
            print_block_insn_unknown(&ctx, info);
        }
    }

    return head_size;

}
