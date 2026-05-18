#include "./include/csr.h"
#include "./include/block_type.h"

.globl _trap_entry
.align 12
_trap_entry:     /* interrupt and exception entry */
/* C routine */
    block_aux_head _trap_entry
    bget sp
    bset a0, ra
    bnext.call trap_handler
    block_text_begin _trap_entry
    block_text_end _trap_entry

.global trap_handler
.section .text
.align 3
trap_handler:
    block_std_head trap_handler
    bset a0, a1, a2
    bnext.direct fault_loop
    block_text_begin trap_handler
    const 0x65
    .4byte (0xf103001e)
    set a0, t#1
    set a1, t#3
    .4byte (0xf102001e)
    set a2, t#1
    block_text_end trap_handler

.global fault_loop
.section .text
.align 3
fault_loop:
    block_std_head fault_loop
    bnext.direct fault_loop
    block_text_begin fault_loop
    const 1
    block_text_end fault_loop



