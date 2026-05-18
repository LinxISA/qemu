#include "./include/init.h"

extern unsigned long page_table_0[0x200];
extern unsigned long page_table_1[0x200];
extern unsigned long page_table_2[0x200];
extern unsigned long page_table_3[0x200];
extern unsigned long page_table_4[0x200];
extern unsigned long page_table_5[0x200];
extern unsigned long page_table_data[0x200];
extern void _trap_entry();
extern void trap_handler();
extern void fault_loop();
extern void my_eret();
extern unsigned long test_ld(unsigned long long addr, unsigned long val);
extern void test_sd(unsigned long addr, unsigned long val);
extern void _putc(unsigned long ch1, unsigned long ch2);

void test_start_Q_0_super()
{
    int flag = 0;
    unsigned long rdata = 0;
    unsigned long addr = 0x1000080004001;
    unsigned long val = 0x42;
    rdata = test_ld(addr, val);
    if (rdata == 0x42) {
        _putc('P', 'S');
    } else {
        _putc('F', 'L');
    }
}

void set_pri_to_N_with_enable_mmu_Q_0_super(int priv)
{
    csr_write(A0_ECSTATE, priv);
    csr_write(A0_ELINK, test_start_Q_0_super);
    unsigned long tn0pb = (unsigned long)page_table_0 >> 14 << 2;
    csr_write(A1_MMTBASE, tn0pb);
    my_eret();
}

void set_exception_vector_LEVEL_5_Q_0_super()
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
    tne2 = (unsigned long)page_table_3>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = (unsigned long)page_table_4>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = 0x8000200000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x02, tne2, 0x00, tne3, 0x02, tne4)

    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (unsigned long)page_table_4>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = 0x8000100000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x02, tne2, 0x00, tne3, 0x01, tne4)

    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (unsigned long)page_table_4>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = 0x1000000000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x00, tne2, 0x40, tne3, 0x00, tne4)

    /* set  SP PA:0x800ffxxx == VA:0x800ffxxx */
    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (unsigned long)page_table_4>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = 0x800ff00000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x02, tne2, 0x00, tne3, 0xff, tne4)

    /* while fetch instruction, let instruction PA: 0x80000xxx == VA:0x80000xxx */
    tne0 = (unsigned long)page_table_1>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne1 = (unsigned long)page_table_2>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne2 = (unsigned long)page_table_3>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne3 = (unsigned long)page_table_4>>12<<32 | 0 | PTE_RWXP_NEXT_PTE | PTE_V;
    tne4 = 0x8000000000000 | PTE_RWXP_READ_WRITE_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x02, tne2, 0x00, tne3, 0x00, tne4)

    // Setting Exception Vectors.
    unsigned long value = (((0xe << 12) | (0xe << 21) | (0xeull << 30)) & (0x00000000FFFFFFFFULL));
    tne4 = (unsigned long)_trap_entry>>12<<32 | PTE_RWXP_READ_EXEC | PTE_V | 0;
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x0e, tne2, 0x0e, tne3, 0x0e, tne4);
    csr_write(A1_EVBASE, value);
    csr_write(A1_MMCONFIG, 2); // VA39
}

void tlb_fill_LEVEL_5_Q_0_super()
{
    unsigned long tne0 = 0;
    unsigned long tne1 = 0;
    unsigned long tne2 = 0;
    unsigned long tne3 = 0;
    unsigned long tne4 = 0;
    unsigned long tne5 = 0;
    tne0 =  0x00 | PTE_V | PTE_RWXP_READ_WRITE| 0;
    PT_FILL_LEVEL_5(0x01, tne0, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00);

    tne0 = (((unsigned long)page_table_1 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne1 = (((unsigned long)page_table_2 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne2 =  (((unsigned long)page_table_data >> 20 << 40) | PTE_V | PTE_RWXP_READ_WRITE| 0);
    PT_FILL_LEVEL_5(0x00,tne0, 0x00, tne1, 0x04, 0, 0x00, 0x00, 0x04, 0x00);

    tne0 = (((unsigned long)page_table_1 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne1 = (((unsigned long)page_table_2 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne2 = (((unsigned long)page_table_3 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne3 = (((unsigned long)page_table_4 >> 12 << 32) | PTE_V | PTE_RWXP_NEXT_PTE | 0);
    tne4 =  (((unsigned long)page_table_data >> 12 << 32) | PTE_V | PTE_RWXP_READ_WRITE | 0);
    PT_FILL_LEVEL_5(0x00, tne0, 0x00, tne1, 0x08, tne2, 0x00, tne3, 0x00, tne4);
}

int main()
{
    tlb_fill_LEVEL_5_Q_0_super();
    set_exception_vector_LEVEL_5_Q_0_super();
    set_pri_to_N_with_enable_mmu_Q_0_super(1);
    return 0;
}