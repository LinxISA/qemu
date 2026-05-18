#include "block_type.h"

.global blk_xorw_tt
.section .text
.align 4
blk_xorw_tt:
    block_std_head blk_xorw_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_xorw_tt
    get a0
    get a1
    xorw t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_xorw_tt

.global blk_xorw_tr
.section .text
.align 4
blk_xorw_tr:
    block_std_head blk_xorw_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_xorw_tr
    get a0
    xorw t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_xorw_tr

.global blk_xorw_rr
.section .text
.align 4
blk_xorw_rr:
    block_std_head blk_xorw_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_xorw_rr
    xorw a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_xorw_rr

