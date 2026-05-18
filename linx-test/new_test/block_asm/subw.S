#include "block_type.h"

.global blk_subw_tt
.section .text
.align 4
blk_subw_tt:
    block_std_head blk_subw_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_subw_tt
    get a0
    get a1
    subw t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_subw_tt

.global blk_subw_tr
.section .text
.align 4
blk_subw_tr:
    block_std_head blk_subw_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_subw_tr
    get a0
    subw t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_subw_tr

.global blk_subw_rr
.section .text
.align 4
blk_subw_rr:
    block_std_head blk_subw_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_subw_rr
    subw a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_subw_rr

.global blk_subw_rtl
.section .text
.align 4
blk_subw_rtl:
    block_std_head blk_subw_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_subw_rtl
    get a1
    subw a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_subw_rtl

.global blk_subw_rth
.section .text
.align 4
blk_subw_rth:
    block_std_head blk_subw_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_subw_rth
    get a0
    set t#1, a4
    get a1
    subw a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_subw_rth

