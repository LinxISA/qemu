#include "block_type.h"

.global blk_setc_lti_t
.section .text
.align 4
blk_setc_lti_t:
    block_std_head blk_setc_lti_t
    bget a0
    bset a0
    bnext.cond _flag_lti_t
    block_text_begin blk_setc_lti_t
    get a0
    setc.lti t#1, 1
    set t#1, a0
    block_text_end blk_setc_lti_t

_no_jump_lti_t:
    block_std_head _no_jump_lti_t
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_lti_t
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_lti_t


_flag_lti_t:
    block_std_head _flag_lti_t
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_lti_t
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_lti_t


.global blk_setc_lti_r
.section .text
.align 4
blk_setc_lti_r:
    block_std_head blk_setc_lti_r
    bset a0
    bget a0
    bnext.cond _flag_lti_r
    block_text_begin blk_setc_lti_r
    setc.lti a0, 1
    set t#1, a0
    block_text_end blk_setc_lti_r

_no_jump_lti_r:
    block_std_head _no_jump_lti_r
    bget ra
    bset a0
    bnext.ret
    block_text_begin _no_jump_lti_r
    const 0
    const 1
    add t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _no_jump_lti_r


_flag_lti_r:
    block_std_head _flag_lti_r
    bget ra
    bset a0
    bnext.ret
    block_text_begin _flag_lti_r
    const 8
    const 8
    add t#1, t#2
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end _flag_lti_r
