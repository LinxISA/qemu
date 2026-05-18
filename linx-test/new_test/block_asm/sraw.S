#include "block_type.h"

.global blk_sraw_tt
.section .text
.align 4
blk_sraw_tt:
    block_std_head blk_sraw_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sraw_tt
    get a0
    get a1
    sraw t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sraw_tt

.global blk_sraw_tr
.section .text
.align 4
blk_sraw_tr:
    block_std_head blk_sraw_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sraw_tr
    get a0
    sraw t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sraw_tr

.global blk_sraw_rr
.section .text
.align 4
blk_sraw_rr:
    block_std_head blk_sraw_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sraw_rr
    sraw a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sraw_rr


.global blk_sraw_rtl
.section .text
.align 4
blk_sraw_rtl:
    block_std_head blk_sraw_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sraw_rtl
    get a1
    sraw a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sraw_rtl

.global blk_sraw_rth
.section .text
.align 4
blk_sraw_rth:
    block_std_head blk_sraw_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_sraw_rth
    get a1
    set t#1, a4
    get a0
    sraw a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sraw_rth
