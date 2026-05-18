#include "block_type.h"

.global blk_setc_lt_tt
.section .text
.align 4
blk_setc_lt_tt:
    block_std_head blk_setc_lt_tt
    bget a0,a1
    bset a0
    bnext.cond _flag_lt_tt
    block_text_begin blk_setc_lt_tt
    get a0
    get a1
    setc.lt t#2, t#1
    set t#1, a0
    block_text_end blk_setc_lt_tt

_no_jump_lt_tt:
    block_std_head _no_jump_lt_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_lt_tt
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_lt_tt


_flag_lt_tt:
    block_std_head _flag_lt_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_lt_tt
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_lt_tt


.global blk_setc_lt_tr
.section .text
.align 4
blk_setc_lt_tr:
    block_std_head blk_setc_lt_tr
    bget a0,a1
    bset a0
    bnext.cond _flag_lt_tr
    block_text_begin blk_setc_lt_tr
    get a0
    setc.lt t#1, a1
    set t#1, a0
    block_text_end blk_setc_lt_tr

_no_jump_lt_tr:
    block_std_head _no_jump_lt_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_lt_tr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_lt_tr


_flag_lt_tr:
    block_std_head _flag_lt_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_lt_tr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_lt_tr


.global blk_setc_lt_rr
.section .text
.align 4
blk_setc_lt_rr:
    block_std_head blk_setc_lt_rr
    bset a0
    bget a0,a1
    bnext.cond _flag_lt_rr
    block_text_begin blk_setc_lt_rr
    setc.lt a0, a1
    set t#1, a0
    block_text_end blk_setc_lt_rr

_no_jump_lt_rr:
    block_std_head _no_jump_lt_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_lt_rr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_lt_rr


_flag_lt_rr:
    block_std_head _flag_lt_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_lt_rr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_lt_rr


.global blk_setc_lt_rtl
.section .text
.align 4
blk_setc_lt_rtl:
    block_std_head blk_setc_lt_rtl
    bset a0
    bget a0,a1
    bnext.cond _flag_lt_rtl
    block_text_begin blk_setc_lt_rtl
    get a1
    setc.lt a0, t#1
    set t#1, a0
    set.g t#1, a0
    block_text_end blk_setc_lt_rtl

_no_jump_lt_rtl:
    block_std_head _no_jump_lt_rtl
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_lt_rtl
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_lt_rtl


_flag_lt_rtl:
    block_std_head _flag_lt_rtl
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_lt_rtl
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_lt_rtl


.global blk_setc_lt_rth
.section .text
.align 4
blk_setc_lt_rth:
    block_std_head blk_setc_lt_rth
    bset a0, a4
    bget a0, a1, a4
    bnext.cond _flag_lt_rth
    block_text_begin blk_setc_lt_rth
    get a1
    get a0
    set t#1, a4
    setc.lt a4, t#3
    set t#1, a0
    block_text_end blk_setc_lt_rth

_no_jump_lt_rth:
    block_std_head _no_jump_lt_rth
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_lt_rth
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_lt_rth


_flag_lt_rth:
    block_std_head _flag_lt_rth
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_lt_rth
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_lt_rth

