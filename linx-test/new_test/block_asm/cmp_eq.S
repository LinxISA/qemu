#include "block_type.h"

.global blk_cmp_eq_tt
.section .text
.align 4
blk_cmp_eq_tt:
    block_std_head blk_cmp_eq_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_eq_tt
    get a0
    get a1
    cmp.eq t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_eq_tt

.global blk_cmp_eq_tr
.section .text
.align 4
blk_cmp_eq_tr:
    block_std_head blk_cmp_eq_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_eq_tr
    get a0
    cmp.eq t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_eq_tr


.global blk_cmp_eq_rr
.section .text
.align 4
blk_cmp_eq_rr:
    block_std_head blk_cmp_eq_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_eq_rr
    cmp.eq a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_eq_rr

