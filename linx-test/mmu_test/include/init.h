#ifndef INIT_H
#define INIT_H

#include <stdio.h>
#include <stdlib.h>
#include "csr.h"

#define PTE_V   0x1
#define PTE_RWXP_NEXT_PTE   0b0000
#define PTE_RWXP_EXEC       0b0010
#define PTE_RWXP_READ       0b1000
#define PTE_RWXP_READ_EXEC  0b1010
#define PTE_RWXP_READ_WRITE 0b1100
#define PTE_RWXP_READ_WRITE_EXEC    0b1110
#define PTE_PV  0x10
#define PTE_G   0x100
#define PTE_MT  0xf000
#define PTE_FSWU    1f0000
#define PTE_D   0x200000
#define PTE_A   0x400000

#ifdef __ASSEMBLY__
#define __ASM_STR(x)	x
#else
#define __ASM_STR(x)	#x
#endif

#define PUSH_BLOCK_TEXT_BODY_SECTION ".pushsection \".text.body\",\"ax\"\n"
#define POP_BLOCK_TEXT_BODY_SECTION  ".popsection\n"

#define csr_read(csr)						\
({								\
	register unsigned long __v;				\
	__asm__ __volatile__ (					\
		"bstart 1f\n"					\
		"b.sys\n"					\
		"bnext.fall\n"					\
		"bset %0\n"					\
		"bstop 2f\n"					\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"1:\n"						\
			"sysget " __ASM_STR(csr)"\n"		\
			"set %0, t#1\n"				\
		"2:\n"						\
		POP_BLOCK_TEXT_BODY_SECTION			\
		: "=r" (__v) :					\
		: "memory");					\
	__v;							\
})

#define csr_write(csr, val)					\
({								\
	unsigned long __v = (unsigned long)(val);		\
	__asm__ __volatile__ (					\
		"bstart 1f\n"					\
		"b.sys\n"					\
		"bnext.fall\n"					\
		"bget %0\n"					\
		"bstop 2f\n"					\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"1:\n"						\
			"get %0\n"				\
			"sysset " __ASM_STR(csr)", t#1\n"	\
		"2:\n"						\
		POP_BLOCK_TEXT_BODY_SECTION			\
		: : "r" (__v)					\
		: "memory");					\
})

#define PT_FILL_LEVEL_3(vas0, tne0, vas1, tne1, vas2, tne2) \
    page_table_0[vas0] = tne0;                              \
    page_table_1[vas1] = tne1;                              \
    page_table_2[vas2] = tne2;          

#define PT_FILL_LEVEL_4(vas0, tne0, vas1, tne1, vas2, tne2, vas3, tne3) \
    page_table_0[vas0] = tne0;                                          \
    page_table_1[vas1] = tne1;                                          \
    page_table_2[vas2] = tne2;                                          \
    page_table_3[vas3] = tne3;

#define PT_FILL_LEVEL_5(vas0, tne0, vas1, tne1, vas2, tne2, vas3, tne3, vas4, tne4) \
    page_table_0[vas0] = tne0;                                                      \
    page_table_1[vas1] = tne1;                                                      \
    page_table_2[vas2] = tne2;                                                      \
    page_table_3[vas3] = tne3;                                                      \
    page_table_4[vas4] = tne4;

#define PT_FILL_LEVEL_6(vas0, tne0, vas1, tne1, vas2, tne2, vas3, tne3, vas4, tne4, vas5, tne5) \
    page_table_0[vas0] = tne0;                                                                  \
    page_table_1[vas1] = tne1;                                                                  \
    page_table_2[vas2] = tne2;                                                                  \
    page_table_3[vas3] = tne3;                                                                  \
    page_table_4[vas4] = tne4;                                                                  \
    page_table_5[vas5] = tne5;

unsigned long get_test_addr();
void test_tlb_fill();
void trap_handler();
void set_pri_to_N_with_enable_mmu_Q_0(int priv);
void set_exception_vector();
void test_start_Q_0();
void set_pri_to_N_with_enable_mmu(int priv);

#endif
