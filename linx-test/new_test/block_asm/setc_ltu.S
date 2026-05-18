#include "block_type.h"

.global blk_setc_ltu_tt
.section .text
.align 4
blk_setc_ltu_tt:
    block_std_head blk_setc_ltu_tt
    bget a0,a1
    bset a0
    bnext.cond _flag_ltu_tt
    block_text_begin blk_setc_ltu_tt
    get a0
    get a1
    setc.ltu t#2, t#1
    set t#1, a0
    block_text_end blk_setc_ltu_tt

_no_jump_ltu_tt:
    block_std_head _no_jump_ltu_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ltu_tt
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ltu_tt


_flag_ltu_tt:
    block_std_head _flag_ltu_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ltu_tt
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ltu_tt


.global blk_setc_ltu_tr
.section .text
.align 4
blk_setc_ltu_tr:
    block_std_head blk_setc_ltu_tr
    bget a0,a1
    bset a0
    bnext.cond _flag_ltu_tr
    block_text_begin blk_setc_ltu_tr
    get a0
    setc.ltu t#1, a1
    set t#1, a0
    block_text_end blk_setc_ltu_tr

_no_jump_ltu_tr:
    block_std_head _no_jump_ltu_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ltu_tr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ltu_tr


_flag_ltu_tr:
    block_std_head _flag_ltu_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ltu_tr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ltu_tr


.global blk_setc_ltu_rr
.section .text
.align 4
blk_setc_ltu_rr:
    block_std_head blk_setc_ltu_rr
    bset a0
    bget a0,a1
    bnext.cond _flag_ltu_rr
    block_text_begin blk_setc_ltu_rr
    setc.ltu a0, a1
    set t#1, a0
    block_text_end blk_setc_ltu_rr

_no_jump_ltu_rr:
    block_std_head _no_jump_ltu_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ltu_rr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ltu_rr


_flag_ltu_rr:
    block_std_head _flag_ltu_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ltu_rr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ltu_rr


.global blk_setc_ltu_rtl
.section .text
.align 4
blk_setc_ltu_rtl:
    block_std_head blk_setc_ltu_rtl
    bset a0
    bget a0,a1
    bnext.cond _flag_ltu_rtl
    block_text_begin blk_setc_ltu_rtl
    get a1
    setc.ltu a0, t#1
    set t#1, a0
    block_text_end blk_setc_ltu_rtl

_no_jump_ltu_rtl:
    block_std_head _no_jump_ltu_rtl
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ltu_rtl
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ltu_rtl


_flag_ltu_rtl:
    block_std_head _flag_ltu_rtl
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ltu_rtl
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ltu_rtl


.global blk_setc_ltu_rth
.section .text
.align 4
blk_setc_ltu_rth:
    block_std_head blk_setc_ltu_rth
    bset a0, a4
    bget a0, a1, a4
    bnext.cond _flag_ltu_rth
    block_text_begin blk_setc_ltu_rth
    get a1
    get a0
    set t#1, a4
    setc.ltu a4, t#3
    set t#1, a0
    block_text_end blk_setc_ltu_rth

_no_jump_ltu_rth:
    block_std_head _no_jump_ltu_rth
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ltu_rth
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ltu_rth


_flag_ltu_rth:
    block_std_head _flag_ltu_rth
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ltu_rth
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ltu_rth

