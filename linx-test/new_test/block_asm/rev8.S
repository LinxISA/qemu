#include "block_type.h"

.global blk_rev8
.section .text
.align 4
blk_rev8:
    block_aux_head blk_rev8
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_rev8
    get a0
    rev8 t#1
    set t#1,a0
    setc.tgt ra
    block_text_end blk_rev8

