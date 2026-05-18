#ifndef LINX_BLOCK_DEF_H
#define LINX_BLOCK_DEF_H

#define CSR_LANENUM      0x0032
#define CPU_NB_LANE_NUM  64

#define T_REG_SIZE       4
#define U_REG_SIZE       4
#define FVEC_REG_SIZE    8
#define FVEC_REG_IDX_MASK    (FVEC_REG_SIZE - 1)
#define GPR_REG_SIZE     24
#define RI_SIZE          12
#define RO_SIZE          4
#define TARGET_REG_TX2   0b11000
#define TARGET_REG_UX2   0b11001
#define TARGET_REG_TX4   0b11010
#define TARGET_REG_UX4   0b11011
#define TARGET_REG_U     0b11110
#define TARGET_REG_T     0b11111

/* real tile register size */
#define TILE_REG_SIZE  24
/* ISA define tile register size*/
#define TILE_REG_LOGICAL_SIZE 16

#define TILE_REG_SRC   0
#define TILE_REG_DST   1
#define TILE_REG_MEM   (512 * 1024)
#define TILE_REG_MEM_TMP (TILE_REG_MEM * 32)
#define MAX_DST_TILE_NUM 8
#define MAX_SRC_TILE_NUM 8

/* SIMT block src/dst reg valid */
#define FVEC_REG_VALID_BITNUM   4
/* SIMT block src/dst reg width */
#define FVEC_REG_WIDTH_BITNUM   2

enum reg_type {
    REG_TYPE_GPR,
    REG_TYPE_TREG,
    REG_TYPE_UREG,
    REG_TYPE_MREG,
    REG_TYPE_NREG,
};

typedef enum {
    T_REG_OFFSET_1 = 0b11000,
    T_REG_OFFSET_2,
    T_REG_OFFSET_3,
    T_REG_OFFSET_4,
    T_REG_OFFSET_5,
    T_REG_OFFSET_6,
    T_REG_OFFSET_7,
    T_REG_OFFSET_8,
} T_REG_INDEX;


/* 2:2 in head field */
typedef enum {
    HEAD_TYPE_STD,
    HEAD_TYPE_SYS,
    HEAD_TYPE_FP,
    HEAD_TYPE_SIMT,
    /*
     * The preceding information is defined based on BlockType in BISA encoding.
     * The following information is the internal definition of QEMU.
     */
    HEAD_TYPE_MCOPY = 0b1111,
    HEAD_TYPE_MMOVE,
    HEAD_TYPE_MSET,
    HEAD_TYPE_MPUSH,
    HEAD_TYPE_MPOP,
    HEAD_TYPE_FENTRY,
    HEAD_TYPE_FEXIT,
    HEAD_TYPE_FRET_RA,
    HEAD_TYPE_FRET_STK,
    HEAD_TYPE_RESERVE,
} LINX_HEAD_TYPE;

#define T_OP_SFT 5
#define T_MD_SFT 10
#define T_MD0    0
#define T_MD1    1
#define T_MD2    2
#define T_MD3    3



#define TILEOP_VECTOR   0
#define TILEOP_MEMORY   1
#define TILEOP_TMA      2
#define T_TEPL          3
#define TILEOP_V1       4
#define TILEOP_V2       5
#define TILEOP_CUBE     6

enum TILEOP_TYPE {
    /* TILEOP_XX  = (TMA/TEPL/MSEQ etc op << Function Length) | Function op */
    TILEOP_MPAR         = (TILEOP_VECTOR << T_OP_SFT) | 0,
    TILEOP_TROWSUMEXP   = (TILEOP_VECTOR << T_OP_SFT) | 26,
    TILEOP_TROWMAXEXP   = (TILEOP_VECTOR << T_OP_SFT) | 27,

    TILEOP_MSEQ         = (TILEOP_MEMORY << T_OP_SFT) | 0,

    TILEOP_TLOAD        = (TILEOP_TMA << T_OP_SFT) | 0,
    TILEOP_TSTORE       = (TILEOP_TMA << T_OP_SFT) | 1,
    TILEOP_TMOV        = (TILEOP_TMA << T_OP_SFT) | 2,
    TILEOP_MGATHER      = (TILEOP_TMA << T_OP_SFT) | 4,
    TILEOP_MSCATTER     = (TILEOP_TMA << T_OP_SFT) | 5,

    TILEOP_TADD         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 0,
    TILEOP_TSUB         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 1,
    TILEOP_TMUL         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 2,
    TILEOP_TDIV         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 3,
    TILEOP_TREM         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 4,
    TILEOP_TFMOD        = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 5,
    TILEOP_TAND         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 6,
    TILEOP_TOR          = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 7,
    TILEOP_TXOR         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 8,
    TILEOP_TSHL         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 9,
    TILEOP_TSHR         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 10,
    TILEOP_TMAX         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 11,
    TILEOP_TMIN         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 12,
    TILEOP_TCMP         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 13,
    TILEOP_TPRELU       = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 14,
    TILEOP_TABS         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 15,
    TILEOP_TNOT         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 16,
    TILEOP_TNEG         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 17,
    TILEOP_TEXP         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 18,
    TILEOP_TLOG         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 19,
    TILEOP_TRECIP       = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 20,
    TILEOP_TSQRT        = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 21,
    TILEOP_TRSQRT       = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 22,
    TILEOP_TRELU        = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 23,
    TILEOP_TADDC        = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 24,
    TILEOP_TSUBC        = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 25,
    TILEOP_TSEL         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 26,
    TILEOP_TCVT         = (T_MD0 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 27,
    TILEOP_TADDS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 0,
    TILEOP_TSUBS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 1,
    TILEOP_TMULS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 2,
    TILEOP_TDIVS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 3,
    TILEOP_TREMS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 4,
    TILEOP_TFMODS       = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 5,
    TILEOP_TANDS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 6,
    TILEOP_TORS         = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 7,
    TILEOP_TXORS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 8,
    TILEOP_TSHLS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 9,
    TILEOP_TSHRS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 10,
    TILEOP_TMAXS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 11,
    TILEOP_TMINS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 12,
    TILEOP_TCMPS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 13,
    TILEOP_TLRELU       = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 14,
    TILEOP_TADDSC       = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 24,
    TILEOP_TSUBSC       = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 25,
    TILEOP_TSELS        = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 26,
    TILEOP_TEXPANDS     = (T_MD1 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 27,
    TILEOP_TROWSUM      = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 0,
    TILEOP_TROWMAX      = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 1,
    TILEOP_TROWMIN      = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 2,
    TILEOP_TROWPROD     = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 3,
    TILEOP_TROWEXPAND       = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 4,
    TILEOP_TROWEXPANDADD    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 5,
    TILEOP_TROWEXPANDSUB    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 6,
    TILEOP_TROWEXPANDMUL    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 7,
    TILEOP_TROWEXPANDDIV    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 8,
    TILEOP_TROWEXPANDMAX    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 9,
    TILEOP_TROWEXPANDMIN    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 10,
    TILEOP_TROWEXPANDEXPDIF = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 11,
    TILEOP_TCOLSUM          = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 16,
    TILEOP_TCOLMAX          = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 17,
    TILEOP_TCOLMIN          = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 18,
    TILEOP_TCOLPROD         = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 19,
    TILEOP_TCOLEXPAND       = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 20,
    TILEOP_TCOLEXPANDADD    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 21,
    TILEOP_TCOLEXPANDSUB    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 22,
    TILEOP_TCOLEXPANDMUL    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 23,
    TILEOP_TCOLEXPANDDIV    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 24,
    TILEOP_TCOLEXPANDMAX    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 25,
    TILEOP_TCOLEXPANDMIN    = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 26,
    TILEOP_TCOLEXPANDEXPDIF = (T_MD2 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 27,
    TILEOP_ESAVE            = (T_MD3 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 30,
    TILEOP_ERCOV            = (T_MD3 << T_MD_SFT) | (T_TEPL << T_OP_SFT) | 31,

    TILEOP_MAMULB       = (TILEOP_CUBE << T_OP_SFT) | 0,
    TILEOP_MAMULBAC     = (TILEOP_CUBE << T_OP_SFT) | 1,
    TILEOP_MAMULBACC    = (TILEOP_CUBE << T_OP_SFT) | 2,
    TILEOP_MAMULBMX     = (TILEOP_CUBE << T_OP_SFT) | 4,
    TILEOP_MAMULBMXAC   = (TILEOP_CUBE << T_OP_SFT) | 5,
    TILEOP_MAMULBMXACC  = (TILEOP_CUBE << T_OP_SFT) | 6,
    TILEOP_ACCCVT       = (TILEOP_CUBE << T_OP_SFT) | 8,

    TILEOP_VPAR         = (TILEOP_V1  << T_OP_SFT) | 0,
    TILEOP_VSEQ         = (TILEOP_V2  << T_OP_SFT) | 0,
};

typedef enum {
    FP64 = 0,
    FP32,
    TF32,
    HF32,
    FP16 = 4,
    BF16 = 5,
    HIF8,
    E4M3 = 7,
    E5M2 = 8,
    E3M2,
    E2M3,
    E2M1x2 = 11,
    E1M2x2 = 12,
    E8M0   = 13,
    HiF4x2 = 14,
    INT64  = 16,
    INT32,
    INT16,
    INT8,
    S4x2,
    UINT64 = 24,
    UINT32,
    UINT16,
    UINT8,
    U4x2    = 28,
    INVALID = 31,
} SrcType;

typedef enum {
    NORMAL = 0,
    ND2Zn = 3,
    ND2Nz = 4,
    DN2Zn = 8,
    DN2Nz = 9,
    Zn2ND = 17,
    Zn2DN = 18,
    Nz2ND = 27,
    Nz2DN = 28,
    CANON = 32,
} MatrixTransTyp;

typedef enum {
    ND = 0,
    DN = 1,
    Nz,
    Zn,
    Nn,
    Zz,
} MatrixStTyp;

typedef enum {
    WIDTH_DOUBLE,
    WIDTH_WORD,
    WIDTH_HALF,
    WIDTH_BYTE,
} DisasSrcWidth;

enum LINX_HEAD_BRANCH_TYPE {
    BRANCH_FALL = 0b001,
    BRANCH_DIRECT_LINK,
    BRANCH_CONDITIONAL,
    BRANCH_CALL,
    BRANCH_IND,
    BRANCH_INDCALL,
    BRANCH_RET,
    BRANCH_ACRE,
    BRANCH_TYPE_NUM
};

/* carg branch type */
enum LINX_CBRANCH_TYPE {
    CBRANCH_FALL    = 0x0,
    CBRANCH_DIRECT  = 0x1,
    CBRANCH_COND    = 0x2,
    CBRANCH_IND     = 0x3,
};

#define OPCODE_BSTART                  0b0000

#define NO_HYP                    0
#define HYP                       1

#define HEAD_SIZE_16     2
#define HEAD_SIZE_32     4

#define SIZE_16     0
#define SIZE_32     1

#define BLK_NONE                0
#define BLK_NONE_RL             1
#define BLK_NONE_AQ             2
#define BLK_NONE_AQRL           3
#define BLK_ATOMIC              4
#define BLK_ATOMIC_RL           5
#define BLK_ATOMIC_AQ           6
#define BLK_ATOMIC_AQRL         7
#define BLK_ATOMIC_FAR_NONE     12
#define BLK_ATOMIC_FAR_RL       13
#define BLK_ATOMIC_FAR_AQ       14
#define BLK_ATOMIC_FAR_AQRL     15

/* if G/L == 0 && RegDst == 0, then the output does not work */
#define GL_0 0
#define REGDST_0 0

#define FIXUP_NONE           0
#define FIXUP                1

#define WRITE_FIXED_BIT_MASK            0b0001
#define B_SBAR_SPEC_TYPE_INORDER_DYN    0b0000
#define B_SBAR_SPEC_TYPE_INORDER_FIXED  0b0001
#define B_SBAR_SPEC_TYPE_UNORDER_DYN    0b0010
#define B_SBAR_SPEC_TYPE_UNORDER_FIXED  0b0011

#define LINX_CACHE_LINE_SIZE 64
#define LINX_CACHE_LINE_SHIFT 6
#define linx_debug_not_reached() g_assert_not_reached()

#define SIMT_DOUBLE_PRECISION        0b000
#define SIMT_SINGLE_PRECISION        0b001
#define SIMT_HALF_PRECISION          0b010
#define SIMT_LOW_PRECISION           0b011
#define SIMT_BF_PRECISION            0b110

#define DOUBLE_PRECISION        0b00
#define SINGLE_PRECISION        0b01
#define HALF_PRECISION          0b10
#define LOW_PRECISION           0b11

#define SIMT_CVT_FP64           0b00000
#define SIMT_CVT_FP32           0b00001
#define SIMT_CVT_TF32           0b00010 /* e8m10 */
#define SIMT_CVT_HF32           0b00011 /* e8m11 */
#define SIMT_CVT_FP16           0b00100 /* e5m10 */
#define SIMT_CVT_BF16           0b00101 /* e8m7 */
#define SIMT_CVT_HIF8           0b00110 /* ??? */
#define SIMT_CVT_FP8            0b00111 /* e4m3 */
#define SIMT_CVT_FP8_1          0b01000 /* e5m2 */
#define SIMT_CVT_E3M2           0b01001
#define SIMT_CVT_E2M3           0b01010
#define SIMT_CVT_E2M1x2         0b01011
#define SIMT_CVT_E1M2x2         0b01100
#define SIMT_CVT_HIF4x2         0b01101
#define SIMT_CVT_E8M0           0b01110
#define SIMT_CVT_BF16x2         0b10001
#define SIMT_CVT_U64            0b00000
#define SIMT_CVT_U32            0b00001
#define SIMT_CVT_U16            0b00010
#define SIMT_CVT_U8             0b00011
#define SIMT_CVT_U4             0b00100
#define SIMT_CVT_S64            0b01000
#define SIMT_CVT_S32            0b1001
#define SIMT_CVT_S16            0b1010
#define SIMT_CVT_S8             0b1011
#define SIMT_CVT_S4             0b1100

#define CVT_FP64                0b00
#define CVT_FP32                0b01
#define CVT_FP16                0b10
#define CVT_FP8                 0b11

#define CVT_ITOF_64             0b00
#define CVT_ITOF_32             0b01
#define CVT_ITOF_16             0b10
#define CVT_ITOF_8              0b11

#define CVT_FTOI_U64            0b000
#define CVT_FTOI_U32            0b001
#define CVT_FTOI_U16            0b010
#define CVT_FTOI_U8             0b011
#define CVT_FTOI_S64            0b100
#define CVT_FTOI_S32            0b101
#define CVT_FTOI_S16            0b110
#define CVT_FTOI_S8             0b111

#endif