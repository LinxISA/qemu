#include "block_type.h"

.global blk_cmp_ge_tt
.section .text
.align 4
blk_cmp_ge_tt:
    block_std_head blk_cmp_ge_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ge_tt
    get a0
    get a1
    cmp.ge t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ge_tt

.global blk_cmp_ge_tr
.section .text
.align 4
blk_cmp_ge_tr:
    block_std_head blk_cmp_ge_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ge_tr
    get a0
    cmp.ge t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ge_tr


.global blk_cmp_ge_rr
.section .text
.align 4
blk_cmp_ge_rr:
    block_std_head blk_cmp_ge_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ge_rr
    cmp.ge a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ge_rr

.global blk_cmp_ge_rtl
.section .text
.align 4
blk_cmp_ge_rtl:
    block_std_head blk_cmp_ge_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ge_rtl
    get a1
    cmp.ge a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ge_rtl

.global blk_cmp_ge_rth
.section .text
.align 4
blk_cmp_ge_rth:
    block_std_head blk_cmp_ge_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_cmp_ge_rth
    get a1
    set t#1, a4
    get a0
    cmp.ge t#1, a4
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ge_rth

