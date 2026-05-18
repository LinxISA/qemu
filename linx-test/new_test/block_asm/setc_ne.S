#include "block_type.h"

.global blk_setc_ne_tt
.section .text
.align 4
blk_setc_ne_tt:
    block_std_head blk_setc_ne_tt
    bget a0,a1
    bset a0
    bnext.cond _flag_ne_tt
    block_text_begin blk_setc_ne_tt
    get a0
    get a1
    setc.ne t#1, t#2
    set t#1, a0
    block_text_end blk_setc_ne_tt

_no_jump_ne_tt:
    block_std_head _no_jump_ne_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ne_tt
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ne_tt


_flag_ne_tt:
    block_std_head _flag_ne_tt
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ne_tt
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ne_tt


.global blk_setc_ne_tr
.section .text
.align 4
blk_setc_ne_tr:
    block_std_head blk_setc_ne_tr
    bget a0,a1
    bset a0
    bnext.cond _flag_ne_tr
    block_text_begin blk_setc_ne_tr
    get a0
    setc.ne t#1, a1
    set t#1, a0
    block_text_end blk_setc_ne_tr

_no_jump_ne_tr:
    block_std_head _no_jump_ne_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ne_tr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_ne_tr


_flag_ne_tr:
    block_std_head _flag_ne_tr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ne_tr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_ne_tr


.global blk_setc_ne_rr
.section .text
.align 4
blk_setc_ne_rr:
    block_std_head blk_setc_ne_rr
    bset a0
    bget a0,a1
    bnext.cond _flag_ne_rr
    block_text_begin blk_setc_ne_rr
    setc.ne a0, a1
    set t#1, a0
    block_text_end blk_setc_ne_rr

_no_jump_ne_rr:
    block_std_head _no_jump_ne_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_ne_rr
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end _no_jump_ne_rr


_flag_ne_rr:
    block_std_head _flag_ne_rr
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_ne_rr
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    setc.tgt ra
    block_text_end _flag_ne_rr
