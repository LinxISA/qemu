#include "block_type.h"

.global blk_get
.section .text
.align 4
blk_get:
    block_std_head blk_get
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_get
    get a0
    set t#1, a1
    get a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_get

