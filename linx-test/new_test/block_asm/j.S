#include "block_type.h"

.global blk_j
.section .text
.align 4
blk_j:
    block_stdh_head blk_j
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_j
    get a0
    get a1
    j 1f
    add t#2, t#3
1:
    set t#2, a0
    get ra
    setc.tgt t#1
    block_text_end blk_j
