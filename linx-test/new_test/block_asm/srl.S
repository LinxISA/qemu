#include "block_type.h"

.global blk_srl_tt
.section .text
.align 4
blk_srl_tt:
    block_std_head blk_srl_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_srl_tt
    get a0
    get a1
    srl t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srl_tt

.global blk_srl_tr
.section .text
.align 4
blk_srl_tr:
    block_std_head blk_srl_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_srl_tr
    get a0
    srl t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srl_tr

.global blk_srl_rr
.section .text
.align 4
blk_srl_rr:
    block_std_head blk_srl_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_srl_rr
    srl a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srl_rr


.global blk_srl_rtl
.section .text
.align 4
blk_srl_rtl:
    block_std_head blk_srl_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_srl_rtl
    get a1
    srl a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srl_rtl

.global blk_srl_rth
.section .text
.align 4
blk_srl_rth:
    block_std_head blk_srl_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_srl_rth
    get a1
    set t#1, a4
    get a0
    srl a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_srl_rth

