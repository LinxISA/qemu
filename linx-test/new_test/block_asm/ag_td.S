#include "block_type.h"

.global blk_ag_td_a
.section .text
.align 4
blk_ag_td_a:
    block_std_head blk_ag_td_a
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_td_a
    get a0
    get a1
    ag.td t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_td_a

.global blk_ag_td0_b
.section .text
.align 4
blk_ag_td0_b:
    block_std_head blk_ag_td0_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_td0_b
    get a0
    ag.td t#1, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_td0_b

.global blk_ag_td1_b
.section .text
.align 4
blk_ag_td1_b:
    block_std_head blk_ag_td1_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_td1_b
    get a1
    ag.td a0, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_td1_b

.global blk_ag_td_d
.section .text
.align 4
blk_ag_td_d:
    block_std_head blk_ag_td_d
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_td_d
    ag.td a0, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_td_d
