#include "block_type.h"

.global blk_cmp_lt_tt
.section .text
.align 4
blk_cmp_lt_tt:
    block_std_head blk_cmp_lt_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_lt_tt
    get a0
    get a1
    cmp.lt t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_lt_tt

.global blk_cmp_lt_tr
.section .text
.align 4
blk_cmp_lt_tr:
    block_std_head blk_cmp_lt_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_lt_tr
    get a0
    cmp.lt t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_lt_tr


.global blk_cmp_lt_rr
.section .text
.align 4
blk_cmp_lt_rr:
    block_std_head blk_cmp_lt_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_lt_rr
    cmp.lt a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_lt_rr

.global blk_cmp_lt_rtl
.section .text
.align 4
blk_cmp_lt_rtl:
    block_std_head blk_cmp_lt_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_lt_rtl
    get a1
    cmp.lt a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_lt_rtl

.global blk_cmp_lt_rth
.section .text
.align 4
blk_cmp_lt_rth:
    block_std_head blk_cmp_lt_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_cmp_lt_rth
    get a1
    set t#1, a4
    get a0
    cmp.lt t#1, a4
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_lt_rth

