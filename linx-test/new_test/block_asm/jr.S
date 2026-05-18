#include "block_type.h"

.global blk_jr_t
.section .text
.align 4
blk_jr_t:
    block_stdh_head blk_jr_t
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_jr_t
    get a0
    get a1
    const 4
    jr t#1
    add t#1, t#2
    add t#1, t#2
    add t#1, t#2
    add a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_jr_t

.global blk_jr_r
.section .text
.align 4
blk_jr_r:
    block_stdh_head blk_jr_r
    bget ra,a0,a1
    bset a0,a2
    bnext.ret
    block_text_begin blk_jr_r
    get a0
    get a1
    const 4
    set t#1, a2
    jr a2
    add t#1, t#2
    add t#1, t#2
    add t#1, t#2
    add a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_jr_r
