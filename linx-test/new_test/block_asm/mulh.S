#include "block_type.h"
.global blk_mulh
.section .text
.align 4
blk_mulh:
    block_aux_head blk_mulh
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_mulh
    get a0
    get a1
    mulh t#1, t#2
    set t#1, a0
    setc.tgt ra
    block_text_end blk_mulh
