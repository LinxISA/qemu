#include "block_type.h"

.global blk_ag_uxtw_a
.section .text
.align 4
blk_ag_uxtw_a:
    block_std_head blk_ag_uxtw_a
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtw_a
    get a0
    get a1
    ag.uxtw t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtw_a

.global blk_ag_uxtw_b
.section .text
.align 4
blk_ag_uxtw_b:
    block_std_head blk_ag_uxtw_b
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtw_b
    get a0
    ag.uxtw t#1, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtw_b

.global blk_ag_uxtw_c
.section .text
.align 4
blk_ag_uxtw_c:
    block_std_head blk_ag_uxtw_c
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtw_c
    get a1
    ag.uxtw a0, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtw_c

.global blk_ag_uxtw_d
.section .text
.align 4
blk_ag_uxtw_d:
    block_std_head blk_ag_uxtw_d
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_ag_uxtw_d
    ag.uxtw a0, a1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_ag_uxtw_d
