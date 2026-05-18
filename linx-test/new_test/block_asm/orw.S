#include "block_type.h"

.global blk_orw_tt
.section .text
.align 4
blk_orw_tt:
    block_std_head blk_orw_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_orw_tt
    get a0
    get a1
    orw t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_orw_tt

.global blk_orw_tr
.section .text
.align 4
blk_orw_tr:
    block_std_head blk_orw_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_orw_tr
    get a0
    orw t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_orw_tr

.global blk_orw_rr
.section .text
.align 4
blk_orw_rr:
    block_std_head blk_orw_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_orw_rr
    orw a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_orw_rr

