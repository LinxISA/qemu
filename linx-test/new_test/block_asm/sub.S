#include "block_type.h"

.global blk_sub_tt
.section .text
.align 4
blk_sub_tt:
    block_std_head blk_sub_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sub_tt
    get a0
    get a1
    sub t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sub_tt

.global blk_sub_tr
.section .text
.align 4
blk_sub_tr:
    block_std_head blk_sub_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sub_tr
    get a0
    sub t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sub_tr

.global blk_sub_rr
.section .text
.align 4
blk_sub_rr:
    block_std_head blk_sub_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sub_rr
    sub a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sub_rr

.global blk_sub_rtl
.section .text
.align 4
blk_sub_rtl:
    block_std_head blk_sub_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sub_rtl
    get a1
    sub a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sub_rtl

.global blk_sub_rth
.section .text
.align 4
blk_sub_rth:
    block_std_head blk_sub_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_sub_rth
    get a0
    set t#1, a4
    get a1
    sub a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sub_rth

