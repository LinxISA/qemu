#include "block_type.h"

.global blk_ag_tw_a
.section .text
.align 4
blk_ag_tw_a:
    block_std_head blk_ag_tw_a
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_tw_a
    get a0
    get a1
    ag.tw t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_tw_a

.global blk_ag_tw0_b
.section .text
.align 4
blk_ag_tw0_b:
    block_std_head blk_ag_tw0_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_tw0_b
    get a0
    ag.tw t#1, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_tw0_b

.global blk_ag_tw1_b
.section .text
.align 4
blk_ag_tw1_b:
    block_std_head blk_ag_tw1_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_tw1_b
    get a1
    ag.tw a0, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_tw1_b

.global blk_ag_tw_d
.section .text
.align 4
blk_ag_tw_d:
    block_std_head blk_ag_tw_d
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_tw_d
    ag.tw a0, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_tw_d
