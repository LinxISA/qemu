#include "block_type.h"

.global blk_setc_ge_tt
.section .text
.align 4
blk_setc_ge_tt:
    block_std_head blk_setc_ge_tt
    bget a0,a1
    bset a0
    bnext.cond _flag_ge_tt
    block_text_begin blk_setc_ge_tt
    get a0
    get a1
    setc.ge t#2, t#1
    set t#1, a0
    block_text_end blk_setc_ge_tt

_no_jump_ge_tt:
    block_std_head _no_jump_ge_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ge_tt
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ge_tt


_flag_ge_tt:
    block_std_head _flag_ge_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ge_tt
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ge_tt


.global blk_setc_ge_tr
.section .text
.align 4
blk_setc_ge_tr:
    block_std_head blk_setc_ge_tr
    bget a0,a1
    bset a0
    bnext.cond _flag_ge_tr
    block_text_begin blk_setc_ge_tr
    get a0
    get a1
    setc.ge t#2, t#1
    set t#1, a0
    block_text_end blk_setc_ge_tr

_no_jump_ge_tr:
    block_std_head _no_jump_ge_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ge_tr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ge_tr


_flag_ge_tr:
    block_std_head _flag_ge_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ge_tr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ge_tr


.global blk_setc_ge_rr
.section .text
.align 4
blk_setc_ge_rr:
    block_std_head blk_setc_ge_rr
    bset a0
    bget a0,a1
    bnext.cond _flag_ge_rr
    block_text_begin blk_setc_ge_rr
    get a0
    get a1
    setc.ge t#2, t#1
    set t#1, a0
    block_text_end blk_setc_ge_rr

_no_jump_ge_rr:
    block_std_head _no_jump_ge_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ge_rr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ge_rr


_flag_ge_rr:
    block_std_head _flag_ge_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ge_rr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ge_rr


.global blk_setc_ge_rtl
.section .text
.align 4
blk_setc_ge_rtl:
    block_std_head blk_setc_ge_rtl
    bset a0
    bget a0,a1
    bnext.cond _flag_ge_rtl
    block_text_begin blk_setc_ge_rtl
    get a0
    get a1
    setc.ge t#2, t#1
    set t#1, a0
    block_text_end blk_setc_ge_rtl

_no_jump_ge_rtl:
    block_std_head _no_jump_ge_rtl
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ge_rtl
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ge_rtl


_flag_ge_rtl:
    block_std_head _flag_ge_rtl
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ge_rtl
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ge_rtl


.global blk_setc_ge_rth
.section .text
.align 4
blk_setc_ge_rth:
    block_std_head blk_setc_ge_rth
    bset a0, a4
    bget a0, a1,a4
    bnext.cond _flag_ge_rth
    block_text_begin blk_setc_ge_rth
    get a0
    get a1
    setc.ge t#2, t#1
    set t#1, a0
    block_text_end blk_setc_ge_rth

_no_jump_ge_rth:
    block_std_head _no_jump_ge_rth
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ge_rth
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ge_rth


_flag_ge_rth:
    block_std_head _flag_ge_rth
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ge_rth
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ge_rth

