#include "block_type.h"

.global blk_xor_tt
.section .text
.align 4
blk_xor_tt:
    block_std_head blk_xor_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_xor_tt
    get a0
    get a1
    xor t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_xor_tt

.global blk_xor_tr
.section .text
.align 4
blk_xor_tr:
    block_std_head blk_xor_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_xor_tr
    get a0
    xor t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_xor_tr

.global blk_xor_rr
.section .text
.align 4
blk_xor_rr:
    block_std_head blk_xor_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_xor_rr
    xor a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_xor_rr

