#include "block_type.h"

.global blk_getwu
.section .text
.align 4
blk_getwu:
    block_std_head blk_getwu
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_getwu
    get a0
    set t#1, a1
    getwu a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_getwu

