#include "block_type.h"

.global blk_ag_uxtd_a
.section .text
.align 4
blk_ag_uxtd_a:
    block_std_head blk_ag_uxtd_a
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtd_a
    get a0
    get a1
    ag.uxtd t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtd_a

.global blk_ag_uxtd_b
.section .text
.align 4
blk_ag_uxtd_b:
    block_std_head blk_ag_uxtd_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtd_b
    get a0
    ag.uxtd t#1, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtd_b

.global blk_ag_uxtd_c
.section .text
.align 4
blk_ag_uxtd_c:
    block_std_head blk_ag_uxtd_c
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtd_c
    get a1
    ag.uxtd a0, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtd_c

.global blk_ag_uxtd_d
.section .text
.align 4
blk_ag_uxtd_d:
    block_std_head blk_ag_uxtd_d
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtd_d
    ag.uxtd a0, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtd_d
