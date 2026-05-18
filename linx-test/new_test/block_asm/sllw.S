#include "block_type.h"

.global blk_sllw_tt
.section .text
.align 4
blk_sllw_tt:
    block_std_head blk_sllw_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sllw_tt
    get a0
    get a1
    sllw t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sllw_tt

.global blk_sllw_tr
.section .text
.align 4
blk_sllw_tr:
    block_std_head blk_sllw_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sllw_tr
    get a0
    sllw t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sllw_tr

.global blk_sllw_rr
.section .text
.align 4
blk_sllw_rr:
    block_std_head blk_sllw_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sllw_rr
    sllw a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sllw_rr

.global blk_sllw_rtl
.section .text
.align 4
blk_sllw_rtl:
    block_std_head blk_sllw_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sllw_rtl
    get a1
    sllw a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sllw_rtl

.global blk_sllw_rth
.section .text
.align 4
blk_sllw_rth:
    block_std_head blk_sllw_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_sllw_rth
    get a1
    set t#1, a4
    get a0
    sllw a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sllw_rth

