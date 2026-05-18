#include "block_type.h"

.global blk_getw
.section .text
.align 4
blk_getw:
    block_std_head blk_getw
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_getw
    get a0
    set t#1, a1
    getw a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_getw

