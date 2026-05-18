#include "block_type.h"

.global blk_divuw
.section .text
.align 4
blk_divuw:
    block_aux_head blk_divuw
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_divuw
    get a0
    get a1
    divuw t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_divuw
