#ifndef __CSR_H__
#define __CSR_H__
#define CSTATE           0x0
/* FIXME: this is temp id, need to be modified */
#define LXLCID           0x1
#define TIME             0x2

/* ACR0 System Register Address Macro Definition */
#define  A0_ECSTATE      0xf000
#define  A0_EVBASE       0xf001
#define  A0_ECAUSE       0xf002
#define  A0_EARG0        0xf003
#define  A0_ELINK        0xf004
#define  A0_ETEMP        0xf005
#define  A0_FUTO         0xf006
#define  A0_IENABLE      0xf007
#define  A0_IPENDING     0xf008
#define  A0_TOPEI        0xf009
#define  A0_EOIEI        0xf00a
#define  A0_MMTBASE      0xf00b
#define  A0_MMCONFIG     0xf00c
#define  A0_TIME         0xf020
#define  A0_TIMECMP      0xf021
#define  A0_TIMEDELTA    0xf022

/* ACR1 System Register Address Macro Definition */
#define  A1_ECSTATE      0xf100
#define  A1_EVBASE       0xf101
#define  A1_ECAUSE       0xf102
#define  A1_EARG0        0xf103
#define  A1_ELINK        0xf104
#define  A1_ETEMP        0xf105
#define  A1_FUTO         0xf106
#define  A1_IENABLE      0xf107
#define  A1_IPENDING     0xf108
#define  A1_TOPEI        0xf109
#define  A1_EOIEI        0xf10a
#define  A1_MMTBASE      0xf10b
#define  A1_MMCONFIG     0xf10c
#define  A1_TIME         0xf120
#define  A1_TIMECMP      0xf121
#define  A1_TIMEDELTA    0xf122
#endif
