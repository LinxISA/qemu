#include "block_type.h"

.global blk_ag_sxtw_a
.section .text
.align 4
blk_ag_sxtw_a:
    block_std_head blk_ag_sxtw_a
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtw_a
    get a0
    get a1
    ag.sxtw t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtw_a

.global blk_ag_sxtw_b
.section .text
.align 4
blk_ag_sxtw_b:
    block_std_head blk_ag_sxtw_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtw_b
    get a0
    ag.sxtw t#1, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtw_b

.global blk_ag_sxtw_c
.section .text
.align 4
blk_ag_sxtw_c:
    block_std_head blk_ag_sxtw_c
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtw_c
    get a1
    ag.sxtw a0, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtw_c

.global blk_ag_sxtw_d
.section .text
.align 4
blk_ag_sxtw_d:
    block_std_head blk_ag_sxtw_d
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtw_d
    ag.sxtw a0, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtw_d
