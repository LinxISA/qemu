#include "block_type.h"

.global blk_bc_tt
.section .text
.align 4
blk_bc_tt:
    block_stdh_head blk_bc_tt
    bget ra,a0,a1
    bset a0,a1
    bnext.ret
    block_text_begin blk_bc_tt
    get a0
    get a1
    const 4
    cmp.eq t#1, t#2
    bc t#1, t#2
    set t#2, a1
    get a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_bc_tt

.global blk_bc_tt_1
.section .text
blk_bc_tt_1:
    block_std_head blk_bc_tt_1
    bget a0,ra
    bset a0
    bnext.ret
    block_text_begin blk_bc_tt_1
    get a0
    addi t#1, 0
    addi t#2, 1
    const -5
    const 0xff
    cmp.ne t#3, t#1
    addi t#4, 0
    bc t#2, t#4
    set t#2, a0
    get ra
    setc.tgt t#1
    block_text_end blk_bc_tt_1

.global blk_bc_tr
.section .text
.align 4
blk_bc_tr:
    block_stdh_head blk_bc_tr
    bget ra,a0,a1,a2
    bset a0,a1,a2
    bnext.ret
    block_text_begin blk_bc_tr
    const 4
    set t#1, a2
    cmp.eq a0, a1
    bc t#1, a2
    set t#2, a1
    get a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_bc_tr

.global blk_bc_rr
.section .text
.align 4
blk_bc_rr:
    block_stdh_head blk_bc_rr
    bget ra,a0,a1,a2,a3
    bset a0,a1,a2,a3
    bnext.ret
    block_text_begin blk_bc_rr
    const 4
    set t#1, a2
    cmp.eq a0, a1
    set t#1, a3
    bc a3, a2
    set t#2, a1
    get a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_bc_rr

.global blk_bc_rt
.section .text
.align 4
blk_bc_rt:
    block_stdh_head blk_bc_rt
    bget ra,a0,a1,a2,a3
    bset a0,a1,a2,a3
    bnext.ret
    block_text_begin blk_bc_rt
    const 4
    cmp.eq a0, a1
    set t#1, a2
    bc a2, t#3
    set t#2, a1
    get a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_bc_rt

