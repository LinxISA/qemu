/* LINX ISA constants */

#ifndef TARGET_LINX_CPU_BITS_H
#define TARGET_LINX_CPU_BITS_H

#include "linx_block_def.h"

#define get_field(reg, mask) (((reg) & \
                 (uint64_t)(mask)) / ((mask) & ~((mask) << 1)))
#define set_field(reg, mask, val) (((reg) & ~(uint64_t)(mask)) | \
                 (((uint64_t)(val) * ((mask) & ~((mask) << 1))) & \
                 (uint64_t)(mask)))


/* SSR Address Space Allocation Rule Fields */
#define SYSREG_ADDR_ACR  0xf000

/* Control and Status Registers */
#define CSR_TP           0x0000
#define CSR_GP           0x0001

#define TIME             0x0010

#define CSTATE           0x0020
#define LXLCID           0x0021
#define VENDOR           0x0022
#define VERSION          0x0023
#define CAPA             0x0024
#define CAPA_EN          0x0025

#define CSR_LC0          0x0050
#define CSR_LB0          0x0051
#define CSR_LPCB0        0x0052
#define CSR_LPCE0        0x0053

#define CSR_LC1          0x0054
#define CSR_LB1          0x0055
#define CSR_LPCB1        0x0056
#define CSR_LPCE1        0x0057

#define CSR_LC2          0x0058
#define CSR_LB2          0x0059
#define CSR_LPCB2        0x005a
#define CSR_LPCE2        0x005b

#define CSR_TR1          0x0800
#define CSR_TR2          0x0801

#define CSR_CW           0x0820

#define CSR_FSSR         0x0038

/* ACR0 System Register Address Macro Definition */
#define  A0_ECSTATE      0x0f00
#define  A0_EVBASE       0x0f01
#define  A0_ECAUSE       0x0f02
#define  A0_EARG0        0x0f03
#define  A0_ETEMP        0x0f05
#define  A0_FUTO         0x0f06
#define  A0_IENABLE      0x0f07
#define  A0_IPENDING     0x0f08
#define  A0_TOPEI        0x0f09
#define  A0_EOIEI        0x0f0a
#define  A0_EBPC         0x0f0b
#define  A0_EBARG        0x0f0c
#define  A0_ETPC         0x0f0d
#define  A0_EBPCN        0x0f0e
#define  A0_MMTBASE      0x0f10
#define  A0_MMCONFIG     0x0f11
#define  A0_TIME         0x0f20
#define  A0_TIMECMP      0x0f21
#define  A0_XBINFO       0x0f30
#define  A0_ACR_PARAM    0x0f31
#define  A0_ELPR0        0x0f40
#define  A0_ELPR1        0x0f41
#define  A0_ELPR2        0x0f42
#define  A0_ELPR3        0x0f43
#define  A0_ELPR4        0x0f44
#define  A0_ELPR5        0x0f45
#define  A0_ELPR6        0x0f46
#define  A0_ELPR7        0x0f47
#define  A0_ELPR8        0x0f48
#define  A0_ELPR9        0x0f49
#define  A0_ELPR10       0x0f4a
#define  A0_ELPR11       0x0f4b
#define  A0_ELPR12       0x0f4c
#define  A0_ELPR13       0x0f4d
#define  A0_ELPR14       0x0f4e
#define  A0_ELPR15       0x0f4f
#define  A0_ELPR16       0x0f50

/* ACR1 System Register Address Macro Definition */
#define  A1_ECSTATE      0x1f00
#define  A1_EVBASE       0x1f01
#define  A1_ECAUSE       0x1f02
#define  A1_EARG0        0x1f03
#define  A1_ETEMP        0x1f05
#define  A1_FUTO         0x1f06
#define  A1_IENABLE      0x1f07
#define  A1_IPENDING     0x1f08
#define  A1_TOPEI        0x1f09
#define  A1_EOIEI        0x1f0a
#define  A1_EBPC         0x1f0b
#define  A1_EBARG        0x1f0c
#define  A1_ETPC         0x1f0d
#define  A1_EBPCN        0x1f0e
#define  A1_MMTBASE      0x1f10
#define  A1_MMCONFIG     0x1f11
#define  A1_TIME         0x1f20
#define  A1_TIMECMP      0x1f21
#define  A1_XBINFO       0x1f30
#define  A1_ACR_PARAM    0x1f31
#define  A1_ELPR0        0x1f40
#define  A1_ELPR1        0x1f41
#define  A1_ELPR2        0x1f42
#define  A1_ELPR3        0x1f43
#define  A1_ELPR4        0x1f44
#define  A1_ELPR5        0x1f45
#define  A1_ELPR6        0x1f46
#define  A1_ELPR7        0x1f47
#define  A1_ELPR8        0x1f48
#define  A1_ELPR9        0x1f49
#define  A1_ELPR10       0x1f4a
#define  A1_ELPR11       0x1f4b
#define  A1_ELPR12       0x1f4c
#define  A1_ELPR13       0x1f4d
#define  A1_ELPR14       0x1f4e
#define  A1_ELPR15       0x1f4f
#define  A1_ELPR16       0x1f50

#define CSTATE_PERMIT       0x200ULL
#define CSTATE_IE           0x10ULL
#define CSTATE_ACR          0xFULL

#define EBARG_GROUPID       0xF700000000000000ULL
#define EBARG_RL            0x20000000000ULL
#define EBARG_AQ            0x10000000000ULL
#define EBARG_TAKEN         0x8000000000ULL
#define EBARG_TYPE          0x6000000000ULL
#define EBARG_BLOCKTYPE     0x1F00000000ULL
#define EBARG_REGDST0       0x1FULL
#define EBARG_REGDST1       0x3E0ULL
#define EBARG_REGDST2       0x7C00ULL
#define EBARG_REGDST3       0xF8000ULL

#define EBPC_BPC            0x0000FFFFFFFFFFFFULL
#define EBPC_BPCN_OFFSET    0xFFFF000000000000ULL

#define ETPC_TPC            0x0000FFFFFFFFFFFFULL

#define ECSTATE_PERMIT      0x200ULL
#define ECSTATE_BI          0x40ULL
#define ECSTATE_IE          0x10ULL
#define ECSTATE_ACR         0xFULL
#define ECSTATE_MASK        (ECSTATE_PERMIT | ECSTATE_IE | ECSTATE_ACR | \
                             ECSTATE_BI)
#define ELINK_BPC           0xFFFFFFFFFFFFFFF0ULL
#define EVBASE_BASE         0xFFFFFFFFFFFFFFFCULL
#define ECAUSE_E            (1ULL << 63)
#define ECAUSE_SYNDROME     0xFFFFFF000000ULL
#define ECAUSE_TRAPNUM      0x003FULL
#define ECONFIG_SOFT        0x4ULL
#define ECONFIG_TIMER       0x2ULL
#define ECONFIG_EXT         0x1ULL
#define ECONFIG_V           0x100000000ULL
#define ECONFIG_C           0x200000000ULL

#define IPENDING_SOFT       0x4ULL
#define IPENDING_TIMER      0x2ULL
#define IPENDING_EXT        0x1ULL
#define XBINFO_BASE         0xFFFFFFFFFFFFFFE0ULL
#define ACR_PARAM_EBS_SZ    0x3FULL

typedef enum {
    MXL_LINX  = 2,
} LINXMXL;

enum {
    ACR0 = 0,
    ACR1 = 1,
    ACR2 = 2,
};

/* Privilege modes */
#define PRV_U 0
#define PRV_S 1
#define PRV_H 2 /* Reserved */
#define PRV_M 3

/* acr mmtbase CSR field masks */
#define MMTBASE_ASID         0xFFFFFF0000000000ULL
#define MMTBASE_TN0PB        0x000000FFFFFFFFFCULL
#define MMTBASE_CNT          0x0000000000000003ULL

#define MMCONFIG_Q           0x0000000000000080ULL
#define MMCONFIG_M           0x0000000000000003ULL
#define MMCONFIG_HU          0x0000000080000000ULL
#define MMCONFIG_EN          0x8000000000000000ULL

/* EN value */
#define VMMA_ON         0b01
#define VMMA_OFF        0b00

/* VM modes(mmconfig.nl) for linx */
#define VM_0_VA36_VA39       0
#define VM_1_VA44_VA48       1
#define VM_2_VA52_VA57       2

/* Page table entry (PTE) fields */
#define TNE_A               0x00400000ULL
#define TNE_D               0x00200000ULL
#define TNE_RWXP            0x00e
#define TNE_V               0x001
#define TNE_X               0x002
#define TNE_W               0x004
#define TNE_R               0x008
#define TNE_PV              0x010

/* Page table PPN shift amount */
#define PTE_PPN_SHIFT       32

/* Leaf page shift amount */
#define PGSHIFT             12

/* Default Reset Vector adress */
#define DEFAULT_RSTVEC      0x1000

/* Exception causes */
typedef enum {
    LINX_EXCP_NONE = -1, /* sentinel value */

    /* INSNuction exception LINX_EXCP_INSN */
    LINX_EXCP_INSN = 0x0,
    LINX_EXCP_INSN_ACCESS = 0x0,
    LINX_EXCP_INSN_TRANSLATION,
    LINX_EXCP_INSN_MISALIGNED,
    LINX_EXCP_INSN_ILLEGAL,
    LINX_EXCP_INSN_PERMISSION,
    LINX_EXCP_INSN_PAGEFAULT,
    LINX_EXCP_INSN_BUS,
    LINX_EXCP_INSN_PARAM,

    /* data exception LINX_EXCP_DATA */
    LINX_EXCP_DATA = 0x100,  /* trapnum = 1, syndrome = 0 */
    LINX_EXCP_DATA_LD_ACCESS = 0x100,
    LINX_EXCP_DATA_LD_MISALIGNED,
    LINX_EXCP_DATA_LD_PAGEFAULT,
    LINX_EXCP_DATA_ST_ACCESS,
    LINX_EXCP_DATA_ST_MISALIGNED,
    LINX_EXCP_DATA_ST_PAGEFAULT,
    LINX_EXCP_DATA_RANGE,
    LINX_EXCP_DATA_BUS,

    /* block exception */
    LINX_EXCP_BLOCK = 0x400, /* trapnum = 4, syndrome = 0 */
    LINX_EXCP_BLK_IVLD_SET = 0x400,
    LINX_EXCP_BLK_IVLD_GET,
    LINX_EXCP_BLK_IVLD_PARM,
    LINX_EXCP_BLK_DUP_SET,
    LINX_EXCP_BLK_IVLD_FIXUP,

    /* assert */
    LINX_EXCP_ASSERT = 0x0f00,  /* trapnum = 15 */

    /* scall */
    LINX_EXCP_SCALL = 0x1000,  /* trapnum = 16 */

    /* ebreak */
    LINX_EXCP_BREAKPOINT = 0x1100, /* trapnum = 17 */

    /* illssr */
    LINX_EXCP_ILLSSR = 0x3e00,   /* trapnum = 62 */
} LINXException;

typedef enum {
    CONSTRAINT_A6_4,
    CONSTRAINT_A6_8,
    CONSTRAINT_A6_9,
    CONSTRAINT_A6_10,
    CONSTRAINT_A7_4,
    CONSTRAINT_A7_8,
    CONSTRAINT_A7_9,
    CONSTRAINT_A7_10,
    CONSTRAINT_A8_7,
    CONSTRAINT_A8_8,
    CONSTRAINT_A8_9,
    CONSTRAINT_A9_7,
    CONSTRAINT_A10_6,
} LINXConstraint;

#define LINX_EXCP_TABLESIZE                 0xFFFF

typedef struct {
    uint64_t priv[16];
} linx_exception_route;

#define LINX_EXCP_TRAPNUM                 0xFF00
#define LINX_EXCP_SYNDROME                0x00FF

#define LINX_EXCP_INT_FLAG                0x80000000
#define LINX_EXCP_INT_MASK                0x7fffffff

/* Interrupt causes */
#define ACR0_EI                            0
#define ACR0_TI                            1
#define ACR0_SI                            2
#define ACR1_EI                            3
#define ACR1_TI                            4
#define ACR1_SI                            5

#define IRQ_MASK                           0x3f
#define PER_ACR_IRQ_NUM                    3

#define IPENDING_EI                        (1 << ACR0_EI)
#define IPENDING_TI                        (1 << ACR0_TI)
#define IPENDING_SI                        (1 << ACR0_SI)

/*
 * Below is commit argument register related macros. Commit argument register
 * has different meanings in different branch types:
 *
 * 1. fall through:       delay instruction information (bit7-bit4)
 *                        redo ecall when bit0 is set.
 * 2. indirect jump/call: target address (bit63-bit0, bit3-bit0 alway 0)
 * 3. condition jump:     indicate jump or not (bit4)
 */
#define BSTATE_CARG_COND_JUMP          1

/* exception instruction information */
#define BSTATE_CARG_CLEAN                   0
#define BSTATE_CARG_DELAY_SCALL             0x1
#define BSTATE_CARG_DELAY_SFENCE_VMA        0x2
#define BSTATE_CARG_DELAY_ACRE_NBSTATE      0x3
#define BSTATE_CARG_DELAY_ACRE_BSTATE       0x4
#define BSTATE_CARG_DELAY_WFI_NBSTATE       0x7
#define BSTATE_CARG_REDO_SCALL              0x8

#define BSTATE_CARG_TEMPLATE_BLOCK          0x9

/* carg flag information */
#define CARG_FLAG_PREDICATE    1
#define CARG_FLAG_NEGATIVE     2
#define CARG_FLAG_ZERO         4
#define CARG_FLAG_CARRY        8
#define CARG_FLAG_OVERFLOW     16
/* carg_flag bit information */
#define CARG_FLAG_PREDICATE_BIT        0

/* scall imm argument */
#define SCT_SYS_REQ     0
#define SCT_SYS_MAC     1
#define SCT_SYS_SEC     2

/* futo field */
#define EC_LOAD_ACCESS            (1 << 0)
#define EC_MISALIGNED             (1 << 1)
#define EC_STORE_A_ACCESS         (1 << 2)
#define EC_STORE_A_MISALIGNED     (1 << 3)

/* SrcType and flag */
#define INSTR_TYPE_AU  1   /* arithmetic inst */
#define INSTR_TYPE_LU  2   /* logic inst */
#define INSTR_TYPE_FP  3
/* Load/Store support only EXT_NONE, EXT_SW, EXT_UW, except AU_NEG */
#define INSTR_TYPE_CMP_SETC_LD_ST  4
/* CMP/SETC syntax only exposes .sw/.uw; SrcRType=3 is treated as none. */
#define INSTR_TYPE_CMP_SETC_SWUW  5

/* register ext operation type*/
#define REG_EXT_SW    0
#define REG_EXT_UW    1
#define REG_EXT_NOT   2
#define REG_EXT_NONE  3


#define AU_SW  0
#define AU_UW  1
#define AU_NEG  2
#define AU_NONE  3

#define ATOMIC_B   0
#define ATOMIC_H   1
#define ATOMIC_S   2
#define ATOMIC_D   3

#define SETC_SW  0
#define SETC_UW 1
#define SETC_NONE  3

#define SRC_FVEC_VT_1       0b0000000
#define SRC_FVEC_VT_4       0b0000011
#define SRC_FVEC_VU_1       0b0001000
#define SRC_FVEC_VU_4       0b0001011
#define SRC_FVEC_VM_1       0b0010000
#define SRC_FVEC_VM_4       0b0010011
#define SRC_FVEC_VN_1       0b0011000
#define SRC_FVEC_VN_4       0b0011011
#define SRC_FVEC_RI0        0b0100000
#define SRC_FVEC_RI11       0b0101011
#define SRC_FVEC_T_1        0b0111000
#define SRC_FVEC_T_4        0b0111011
#define SRC_FVEC_U_1        0b0111100
#define SRC_FVEC_U_4        0b0111111
#define SRC_FVEC_VT_REUSE_1 0b1100000
#define SRC_FVEC_VT_REUSE_4 0b1100011
#define SRC_FVEC_VU_REUSE_1 0b1101000
#define SRC_FVEC_VU_REUSE_4 0b1101011
#define SRC_FVEC_VM_REUSE_1 0b1110000
#define SRC_FVEC_VM_REUSE_4 0b1110011
#define SRC_FVEC_VN_REUSE_1 0b1111000
#define SRC_FVEC_VN_REUSE_4 0b1111011

#define SRC_FVRC_REG_MASK   0b011111

#define SRC_FVEC_LC0    0b1000000
#define SRC_FVEC_LB0    0b1000001

#define SRC_FVEC_LC1    0b1000100
#define SRC_FVEC_LB1    0b1000101

#define SRC_FVEC_LC2    0b1001000
#define SRC_FVEC_LB2    0b1001001

#define SRC_FVEC_TA    0b1010000
#define SRC_FVEC_TB    0b1010001
#define SRC_FVEC_TC    0b1010010
#define SRC_FVEC_TD    0b1010011
#define SRC_FVEC_TE    0b1010100
#define SRC_FVEC_TF    0b1010101
#define SRC_FVEC_TG    0b1010110
#define SRC_FVEC_TH    0b1010111
#define SRC_FVEC_TO    0b1011000
#define SRC_FVEC_TO1   0b1011001
#define SRC_FVEC_TO2   0b1011010
#define SRC_FVEC_TO3   0b1011011
#define SRC_FVEC_ZERO  0b1011111

#define DST_FVEC_VT     0b00
#define DST_FVEC_VU     0b01
#define DST_FVEC_VM     0b10
#define DST_FVEC_VN     0b11
#define DST_FVEC_RO0    0b0100000
#define DST_FVEC_RO1    0b0100001
#define DST_FVEC_RO2    0b0100010
#define DST_FVEC_RO3    0b0100011
#define DST_FVEC_U      0b0111110
#define DST_FVEC_T      0b0111111
#define DST_FVEC_ZERO   0b1011111
#define SRC_FVEC_PRED   0b1011100
#define DST_FVEC_PRED   SRC_FVEC_PRED
#define MAX_NUM_UNSIGNED_LONG 0xFFFFFFFFFFFFFFFFULL

enum scall_request_type {
    SCT_MAC,
    SCT_SYS,
    SCT_SEC,
    SCT_MAX_NUMBER
};

/* futo field */
#define EC_LOAD_ACCESS            (1 << 0)
#define EC_MISALIGNED             (1 << 1)
#define EC_STORE_A_ACCESS         (1 << 2)
#define EC_STORE_A_MISALIGNED     (1 << 3)

#endif
