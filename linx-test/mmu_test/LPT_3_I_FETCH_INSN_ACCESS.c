#include "./include/init.h"

extern unsigned long page_table_0[0x200];
extern unsigned long page_table_1[0x200];
extern unsigned long page_table_2[0x200];
extern unsigned long page_table_3[0x200];
extern unsigned long page_table_4[0x200];
extern unsigned long page_table_5[0x200];
extern unsigned long page_table_data[0x200];
extern void _trap_entry();

void set_exception_vector_LEVEL_3_Q_0_fetch()
{
    unsigned long tne0 = 0;
    unsigned long tne1 = 0;
    unsigned long tne2 = 0;
    unsigned long tne3 = 0;
    unsigned long tne4 = 0;
    unsigned long tne5 = 0;
    /* while fetch instruction, let instruction PA: 0x80001xxx == VA:0x80001xxx */
    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = 0x8000100000000 | PTE_RWXP_READ | PTE_V | 0;
    PT_FILL_LEVEL_3(0x02, tne0, 0x00, tne1, 0x01, tne2)

    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = 0x1000000000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_3(0x00, tne0, 0x80, tne1, 0x00, tne2)

    /* set  SP PA:0x800ffxxx == VA:0x800ffxxx */
    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = 0x800ff00000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_3(0x02, tne0, 0x00, tne1, 0xff, tne2)

    /* while fetch instruction, let instruction PA: 0x80000xxx == VA:0x80000xxx */
    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = 0x8000000000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_3(0x02, tne0, 0x00, tne1, 0x00, tne2)

    // Setting Exception Vectors.
    unsigned long value = (((0xe << 12) | (0xe << 21) | (0xeull << 30)) & (0x00000000FFFFFFFFULL));
    tne2 = (unsigned long)_trap_entry>>12<<32 | PTE_RWXP_READ_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_3(0x0e, tne0, 0x0e, tne1, 0x0e, tne2);
    csr_write(A1_EVBASE, value);
    csr_write(A1_MMCONFIG, 0); // VA39
}

void tlb_fill_LEVEL_3_Q_0_fetch()
{
    unsigned long tne0 = 0;
    unsigned long tne1 = 0;
    unsigned long tne2 = 0;
    unsigned long tne3 = 0;
    unsigned long tne4 = 0;
    unsigned long tne5 = 0;
    tne0 = (((unsigned long)page_table_1 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne1 = (((unsigned long)page_table_2 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne2 =  (((unsigned long)page_table_data >> 12 << 32) | PTE_V | PTE_RWXP_READ_WRITE| 0);
    PT_FILL_LEVEL_3(0x01, tne0, 0x01, tne1, 0x02, tne2);

    tne0 = (((unsigned long)page_table_1 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne1 = (((unsigned long)page_table_2 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne2 =  (((unsigned long)page_table_data >> 12 << 32) | PTE_V | PTE_RWXP_READ_WRITE| 0);
    PT_FILL_LEVEL_3(0x04, tne0, 0x02, tne1, 0x02, tne2);

    tne0 = (((unsigned long)page_table_1 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne1 = (((unsigned long)page_table_2 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne2 =  (((unsigned long)page_table_data >> 12 << 32) | PTE_V | PTE_RWXP_READ_WRITE | 0);
    PT_FILL_LEVEL_3(0x08, tne0, 0x00, tne1, 0x00, tne2);
}

int main()
{
    tlb_fill_LEVEL_3_Q_0_fetch();
    set_exception_vector_LEVEL_3_Q_0_fetch();
    set_pri_to_N_with_enable_mmu_Q_0(1);
    return 0;
}