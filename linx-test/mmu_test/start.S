#include "./include/csr.h"
#include "./include/block_type.h"

.global _start
.section .text
.align 3
_start:
    block_std_head _start
    bset sp
    bnext.direct call_cmain
    block_text_begin _start
    # set sp = 0x80100000
    const 0x80
    slli t#1, 4
    addi t#1, 0x01
    slli t#1, 20
    set  sp, t#1
    block_text_end _start

.global test_ld
.section .text
.align 3
test_ld:
    block_std_head test_ld
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin test_ld
    get a0
    get a1
    lb [t#2, 0]
    sb t#2, [t#3, 0]
    lb [t#4, 0]
    set a0, t#1
    get ra
    setc.tgt t#1
    block_text_end test_ld

.global test_sd
.section .text
.align 3
test_sd:
    block_std_head test_sd
    bget ra,a0,a1
    bnext.ret
    block_text_begin test_sd
    get a0
    get a1
    sd t#1, [t#2, 0]
    get ra
    setc.tgt t#1
    block_text_end test_sd

.global my_eret
.section .text
.align 3
my_eret:
    block_sys_head my_eret
    bnext.fall
    block_text_begin my_eret
    const 0
    .2byte (0b0000000010010011)
    block_text_end my_eret

.global call_cmain
.section .text
.align 3
call_cmain:
    block_std_head call_cmain
    bnext.call main
    block_text_begin call_cmain
    block_text_end call_cmain

.global loop1
.section .text
.align 3
loop1:
    block_std_head loop1
    bnext.direct loop1
    block_text_begin loop1
    const 0
    block_text_end loop1