#include "block_type.h"

.global blk_srlw_tt
.section .text
.align 4
blk_srlw_tt:
    block_std_head blk_srlw_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_srlw_tt
    get a0
    get a1
    srlw t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srlw_tt

.global blk_srlw_tr
.section .text
.align 4
blk_srlw_tr:
    block_std_head blk_srlw_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_srlw_tr
    get a0
    srlw t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srlw_tr

.global blk_srlw_rr
.section .text
.align 4
blk_srlw_rr:
    block_std_head blk_srlw_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_srlw_rr
    srlw a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srlw_rr


.global blk_srlw_rtl
.section .text
.align 4
blk_srlw_rtl:
    block_std_head blk_srlw_rtl
    bget ra,a1,a0
    bset a0
    bnext.ret
    block_text_begin blk_srlw_rtl
    get a1
    srlw a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srlw_rtl

.global blk_srlw_rth
.section .text
.align 4
blk_srlw_rth:
    block_std_head blk_srlw_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_srlw_rth
    get a1
    set t#1, a4
    get a0
    srlw a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srlw_rth

