#include "block_type.h"
.global blk_mul
.section .text
.align 4
blk_mul:
    block_aux_head blk_mul
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_mul
    get a0
    get a1
    mul t#1, t#2
    set t#1, a0
    setc.tgt ra
    block_text_end blk_mul
