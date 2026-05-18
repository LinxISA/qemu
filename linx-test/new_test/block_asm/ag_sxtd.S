#include "block_type.h"

.global blk_ag_sxtd_a
.section .text
.align 4
blk_ag_sxtd_a:
    block_std_head blk_ag_sxtd_a
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtd_a
    get a0
    get a1
    ag.sxtd t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtd_a

.global blk_ag_sxtd_b
.section .text
.align 4
blk_ag_sxtd_b:
    block_std_head blk_ag_sxtd_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtd_b
    get a0
    ag.sxtd t#1, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtd_b

.global blk_ag_sxtd_c
.section .text
.align 4
blk_ag_sxtd_c:
    block_std_head blk_ag_sxtd_c
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtd_c
    get a1
    ag.sxtd a0, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtd_c

.global blk_ag_sxtd_d
.section .text
.align 4
blk_ag_sxtd_d:
    block_std_head blk_ag_sxtd_d
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_sxtd_d
    ag.sxtd a0, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_sxtd_d
