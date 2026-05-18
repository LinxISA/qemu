#include "./include/csr.h"
#include "./include/block_type.h"
.section .text
.globl _putc
.align 4
_putc:       /* put two char */
    block_std_head _putc
    bget a0, a1, ra
    bnext.ret
	block_text_begin _putc
        const 0x100
        slli t#1, 20      /* 0x10000000 */
        const 0
        get a0
        add t#1, t#2
        sb t#1,[t#4,0]
        get a1
        add t#1, t#5
        sb t#1,[t#7,0]
        get ra
        setc.tgt t#1
	block_text_end _putc