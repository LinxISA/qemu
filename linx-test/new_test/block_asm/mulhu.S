#include "block_type.h"
.global blk_mulhu
.section .text
.align 4
blk_mulhu:
    block_aux_head blk_mulhu
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_mulhu
    get a0
    get a1
    mulhu t#1, t#2
    set t#1, a0
    setc.tgt ra
    block_text_end blk_mulhu

