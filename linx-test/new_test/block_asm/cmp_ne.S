#include "block_type.h"

.global blk_cmp_ne_tt
.section .text
.align 4
blk_cmp_ne_tt:
    block_std_head blk_cmp_ne_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ne_tt
    get a0
    get a1
    cmp.ne t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ne_tt

.global blk_cmp_ne_tr
.section .text
.align 4
blk_cmp_ne_tr:
    block_std_head blk_cmp_ne_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ne_tr
    get a0
    cmp.ne t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ne_tr


.global blk_cmp_ne_rr
.section .text
.align 4
blk_cmp_ne_rr:
    block_std_head blk_cmp_ne_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ne_rr
    cmp.ne a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ne_rr

.global blk_cmp_ne_rtl
.section .text
.align 4
blk_cmp_ne_rtl:
    block_std_head blk_cmp_ne_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ne_rtl
    get a1
    cmp.ne a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ne_rtl

.global blk_cmp_ne_rth
.section .text
.align 4
blk_cmp_ne_rth:
    block_std_head blk_cmp_ne_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_cmp_ne_rth
    get a1
    set t#1, a4
    get a0
    cmp.ne a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ne_rth


