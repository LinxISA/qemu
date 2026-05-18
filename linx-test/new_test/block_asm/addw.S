#include "block_type.h"

.global blk_addw_tt
.section .text
.align 4
blk_addw_tt:
    block_std_head blk_addw_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_addw_tt
    get a0
    get a1
    addw t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_addw_tt

.global blk_addw_tr
.section .text
.align 4
blk_addw_tr:
    block_std_head blk_addw_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_addw_tr
    get a0
    addw t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_addw_tr

.global blk_addw_rr
.section .text
.align 4
blk_addw_rr:
    block_std_head blk_addw_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_addw_rr
    addw a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_addw_rr

