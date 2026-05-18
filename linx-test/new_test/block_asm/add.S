#include "block_type.h"

.global blk_add_tt
.section .text

.align 4
blk_add_tt:
    block_std_head blk_add_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_add_tt
    get a0
    get a1
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_add_tt

.global blk_add_tr
.section .text

.align 4
blk_add_tr:
    block_std_head blk_add_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_add_tr
    get a0
    add t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_add_tr

.global blk_add_rr
.section .text

.align 4
blk_add_rr:
    block_std_head blk_add_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_add_rr
    add a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_add_rr

