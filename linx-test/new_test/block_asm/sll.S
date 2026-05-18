#include "block_type.h"

.global blk_sll_tt
.section .text
.align 4
blk_sll_tt:
    block_std_head blk_sll_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sll_tt
    get a0
    get a1
    sll t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sll_tt

.global blk_sll_tr
.section .text
.align 4
blk_sll_tr:
    block_std_head blk_sll_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sll_tr
    get a0
    sll t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sll_tr

.global blk_sll_rr
.section .text
.align 4
blk_sll_rr:
    block_std_head blk_sll_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sll_rr
    sll a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sll_rr

.global blk_sll_rtl
.section .text
.align 4
blk_sll_rtl:
    block_std_head blk_sll_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sll_rtl
    get a1
    sll a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sll_rtl

.global blk_sll_rth
.section .text
.align 4
blk_sll_rth:
    block_std_head blk_sll_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_sll_rth
    get a1
    set t#1, a4
    get a0
    sll a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sll_rth

