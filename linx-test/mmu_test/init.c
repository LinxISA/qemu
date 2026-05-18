#include "./include/init.h"
#include <stdio.h>
extern void _trap_entry();
extern void trap_handler();
extern void fault_loop();
extern void my_eret();
extern unsigned long test_ld(unsigned long long addr, unsigned long val);
extern void test_sd(unsigned long addr, unsigned long val);
extern void _putc(unsigned long ch1, unsigned long ch2);
unsigned long page_table_0[0x200] = {0};
unsigned long page_table_1[0x200] = {0};
unsigned long page_table_2[0x200] = {0};
unsigned long page_table_3[0x200] = {0};
unsigned long page_table_4[0x200] = {0};
unsigned long page_table_5[0x200] = {0};

unsigned long page_table_data[0x200] ={
    0x7766554433221100, 0xffeeddccbbaa9988,
    0xfedcba9876543210, 0x0123456789abcdef,
    0xfedcba9876543210, 0x0123456789abcdef
};

void set_pri_to_N_with_enable_mmu(int priv)
{
    csr_write(A0_ECSTATE, priv);
    csr_write(A0_ELINK, test_start_Q_0);
    unsigned long tn0pb = ((unsigned long)page_table_0 >> 12 << 2)  + 0x01;
    csr_write(A1_MMTBASE, tn0pb);
    my_eret();
}

void test_start_Q_0()
{
    int flag = 0;
    unsigned long rdata = 0;
    unsigned long addr = 0x001 | (0x02 << 12) | (0x01 << 21) | (0x01 << 30);
    unsigned long val = 0x42;
    rdata = test_ld(addr, val);
    if (rdata == 0x42) {
        _putc('P', 'S');
    } else {
        _putc('F', 'L');
    }
}

void set_pri_to_N_with_enable_mmu_Q_0(int priv)
{
    csr_write(A0_ECSTATE, priv);
    csr_write(A0_ELINK, test_start_Q_0);
    unsigned long tn0pb = (unsigned long)page_table_0 >> 12 << 2 | 0x01;
    csr_write(A1_MMTBASE, tn0pb);
    my_eret();
}

void set_exception_vector()
{
    unsigned long tne0 = 0;
    unsigned long tne1 = 0;
    unsigned long tne2 = 0;
    unsigned long tne3 = 0;
    unsigned long tne4 = 0;
    unsigned long tne5 = 0;
    
#ifdef LEVEL_3_Q_0
    csr_write(A1_MMTBASE, 8);
    // ACR0: VA(0x80000xxx) == PA(0x80000xxx) when mmu is enabled.
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = 0x80000000 | PTE_RWXP_READ_WRITE | PTE_V; // because it's ACR0, U-bit should be 0.
    PT_FILL_LEVEL_3(0x08, tne0, 0x00, tne1, 0x00, tne2)

    // Setting Exception Vectors.
    tne2 = (unsigned long)_trap_entry>>32<<32 | PTE_RWXP_READ_WRITE | PTE_V;
    PT_FILL_LEVEL_3(0x0e, tne0, 0x0e, tne1, 0x0e, tne2);
    csr_write(A1_EVBASE, ((0xe << 12) | (0xe << 21) | (0xeull << 30)));
    csr_write(A1_MMCONFIG, 0); // VA39
#endif

#ifdef LEVEL_3_Q_1
    // ACR0: VA(0x80000xxx) == PA(0x80000xxx) when mmu is enabled.
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = 0x80000000 | PTE_RWXP_READ_WRITE | PTE_V; // because it's ACR0, U-bit should be 0.
    PT_FILL_LEVEL_3(0x08, tne0, 0x00, tne1, 0x00, tne2)

    // Setting Exception Vectors.
    tne2 = (unsigned long)_trap_entry>>32<<32 | PTE_RWXP_READ_WRITE | PTE_V;
    PT_FILL_LEVEL_3(0x0e, tne0, 0x0e, tne1, 0x0e, tne2);
    csr_write(A1_EVBASE, ((0xe << 12) | (0xe << 20) | (0xe << 28)));
    csr_write(A1_MMCONFIG, 0x10); // VA39
#endif

#ifdef LEVEL_4_Q_0
    // ACR0: VA(0x80000xxx) == PA(0x80000xxx) when mmu is enabled.
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = 0x80000000 | PTE_RWXP_READ_WRITE | PTE_V; // because it's ACR0, U-bit should be 0.
    PT_FILL_LEVEL_4(0x00, tne0, 0x08, tne1, 0x00, tne2, 0x00, tne3);

    // Setting Exception Vectors.
    tne3 = (unsigned long)_trap_entry>>32<<32 |  PTE_RWXP_READ_WRITE | PTE_V;
    PT_FILL_LEVEL_4(0x00, tne0, 0x0e, tne1, 0x0e, tne2, 0x0e, tne3);
    csr_write(A1_EVBASE, ((0xe << 12) | (0xe << 21) | (0xeull << 30)) | (0xe << 39));
    csr_write(A1_MMCONFIG, 1);
#endif

#ifdef LEVEL_4_Q_1
    // ACR0: VA(0x80000xxx) == PA(0x80000xxx) when mmu is enabled.
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = 0x80000000 | PTE_RWXP_READ_WRITE | PTE_V; // because it's ACR0, U-bit should be 0.
    PT_FILL_LEVEL_4(0x00, tne0, 0x08, tne1, 0x00, tne2, 0x00, tne3);

    // Setting Exception Vectors.
    tne3 = (unsigned long)_trap_entry>>32<<32 |  PTE_RWXP_READ_WRITE | PTE_V;
    PT_FILL_LEVEL_4(0x00, tne0, 0x0e, tne1, 0x0e, tne2, 0x0e, tne3);
    csr_write(A1_EVBASE, ((0xe << 12) | (0xe << 20) | (0xe << 28)) | (0xe << 36));
    csr_write(A1_MMCONFIG, 0x11);
#endif

#ifdef LEVEL_5_Q_0
    // ACR0: VA(0x80000xxx) == PA(0x80000xxx) when mmu is enabled.
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (unsigned long)page_table_4>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = 0x80000000 | PTE_RWXP_READ_WRITE | PTE_V; // because it's ACR0, U-bit should be 0.
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x08, tne2, 0x00, tne3, 0x00, tne4);

    // Setting Exception Vectors.
    tne4 = (unsigned long)_trap_entry>>32<<32 | PTE_RWXP_READ_WRITE | PTE_V;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x0e, tne2, 0x0e, tne3, 0x0e, tne4);
    csr_write(A1_EVBASE, ((0xe << 12) | (0xe << 21) | (0xeull << 30)) | (0xe << 39) | (0xe << 48));
    csr_write(A1_MMCONFIG, 2);
#endif

#ifdef LEVEL_5_Q_1
    // ACR0: VA(0x80000xxx) == PA(0x80000xxx) when mmu is enabled.
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (unsigned long)page_table_4>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = 0x80000000 | PTE_RWXP_READ_WRITE | PTE_V; // because it's ACR0, U-bit should be 0.
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x08, tne2, 0x00, tne3, 0x00, tne4);

    // Setting Exception Vectors.
    tne4 = (unsigned long)_trap_entry>>32<<32 | PTE_RWXP_READ_WRITE | PTE_V;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x0e, tne2, 0x0e, tne3, 0x0e, tne4);
    csr_write(A1_EVBASE, ((0xe << 12) | (0xe << 20) | (0xe << 28)) | (0xe << 36) | (0xe << 42));
    csr_write(A1_MMCONFIG, 0x12);
#endif

}

void test_tlb_fill()
{
    unsigned long tne0 = 0;
    unsigned long tne1 = 0;
    unsigned long tne2 = 0;
    unsigned long tne3 = 0;
    unsigned long tne4 = 0;
#ifdef  LEVEL_3
    tne0 = (((unsigned long)page_table_1 >> 14 << 14) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne1 = (((unsigned long)page_table_2 >> 14 << 14) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne2 =  (((unsigned long)page_table_data >> 14 << 14) | PTE_V | PTE_RWXP_READ_WRITE);
    PT_FILL_LEVEL_3(0x01, tne0, 0x01, tne1, 0x01, tne2);
#endif

#ifdef LEVEL_4
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (((unsigned long)page_table_data >> 32 << 32) | PTE_V | PTE_RWXP_READ_WRITE);
    PT_FILL_LEVEL_4(0x01, tne0, 0x01, tne1, 0x01, tne2, 0x01, tne3);
#endif

#ifdef LEVEL_5
    tne0 = (unsigned long)page_table_1>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (unsigned long)page_table_4>>32<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = (((unsigned long)page_table_data >> 32 << 32) | PTE_V | PTE_RWXP_READ_WRITE);
    PT_FILL_LVEL_5(0x01, tne0, 0x01, tne1, 0x01, tne2, 0x01, tne3, 0x01, tne4);
#endif
}
