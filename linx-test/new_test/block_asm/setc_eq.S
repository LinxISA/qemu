#include "block_type.h"

.global blk_setc_eq_tt
.section .text
.align 4
blk_setc_eq_tt:
    block_std_head blk_setc_eq_tt
    bget a0,a1
    bnext.cond _flag_tt
    block_text_begin blk_setc_eq_tt
    get a0
    get a1
    setc.eq t#1, t#2
    block_text_end blk_setc_eq_tt

_no_jump_tt:
    block_std_head _no_jump_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_tt
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_tt


_flag_tt:
    block_std_head _flag_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_tt
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_tt


.global blk_setc_eq_tr
.section .text
.align 4
blk_setc_eq_tr:
    block_std_head blk_setc_eq_tr
    bget a0,a1
    bset a0
    bnext.cond _flag_tr
    block_text_begin blk_setc_eq_tr
    get a0
    setc.eq t#1, a1
    set t#1, a0
    block_text_end blk_setc_eq_tr

_no_jump_tr:
    block_std_head _no_jump_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_tr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_tr


_flag_tr:
    block_std_head _flag_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_tr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_tr


.global blk_setc_eq_rr
.section .text
.align 4
blk_setc_eq_rr:
    block_std_head blk_setc_eq_rr
    bset a0
    bget a0,a1
    bnext.cond _flag_rr
    block_text_begin blk_setc_eq_rr
    setc.eq a0, a1
    set t#1, a0
    block_text_end blk_setc_eq_rr

_no_jump_rr:
    block_std_head _no_jump_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_rr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_rr


_flag_rr:
    block_std_head _flag_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_rr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_rr

