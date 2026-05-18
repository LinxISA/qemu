#include "block_type.h"

.global blk_and_tt
.section .text
.align 4
blk_and_tt:
    block_std_head blk_and_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_and_tt
    get a0
    get a1
    and t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_and_tt

.global blk_and_tr
.section .text
.align 4
blk_and_tr:
    block_std_head blk_and_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_and_tr
    get a0
    and t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_and_tr

.global blk_and_rr
.section .text
.align 4
blk_and_rr:
    block_std_head blk_and_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_and_rr
    and a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_and_rr

