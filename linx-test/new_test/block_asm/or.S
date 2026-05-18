#include "block_type.h"

.global blk_or_tt
.section .text
.align 4
blk_or_tt:
    block_std_head blk_or_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_or_tt
    get a0
    get a1
    or t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_or_tt

.global blk_or_tr
.section .text
.align 4
blk_or_tr:
    block_std_head blk_or_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_or_tr
    get a0
    or t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_or_tr

.global blk_or_rr
.section .text
.align 4
blk_or_rr:
    block_std_head blk_or_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_or_rr
    or a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_or_rr

