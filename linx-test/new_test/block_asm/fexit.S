#include "block_type.h"

.global blk_fexit
.section .text
.align 3
blk_fexit:
    block_std_head blk_add
    bget a0
    bset a0
    block_text_begin blk_add
    const 2040
    add a0, t#1
    set t#1, a0
    block_text_end blk_add
    f.entry [ra], a0!, 0
    block_std_head blk_sub
    bget a0
    bset a0
    block_text_begin blk_sub
    const 2040
    sub a0, t#1
    set t#1, a0
    block_text_end blk_sub
    f.exit [ra, a2, a3, a4, a5, a6, a7], a0!, 2040
    block_std_head blk_fexit
    bget ra
    bnext.ret
    block_text_begin blk_fexit
    setc.tgt ra
    block_text_end blk_fexit

.global blk_fexit_entry
.section .text
.align 3
blk_fexit_entry:
    f.entry [ra, a2, a3, a4, a5, a6, a7], a0!, 960
    block_std_head blk_fexit_entry
    bget ra
    bnext.ret
    block_text_begin blk_fexit_entry
    setc.tgt ra
    block_text_end blk_fexit_entry

.global blk_fexit_1
.section .text
.align 3
blk_fexit_1:
    block_std_head blk_add1
    bget a0
    bset a0
    block_text_begin blk_add1
    const -2048
    add a0, t#1
    set t#1, a0
    block_text_end blk_add1
    f.entry [ra], a0!, 0
    block_std_head blk_sub1
    bget a0
    bset a0
    block_text_begin blk_sub1
    const -2048
    sub a0, t#1
    set t#1, a0
    block_text_end blk_sub1
    f.exit [ra, a2, a3, a4, a5, a6, a7], a0!, -2048
    block_std_head blk_fexit_1
    bget ra
    bnext.ret
    block_text_begin blk_fexit_1
    setc.tgt ra
    block_text_end blk_fexit_1

.global blk_fexit_1_entry
.section .text
.align 3
blk_fexit_1_entry:
    f.entry [ra, a2, a3, a4, a5, a6, a7], a0!, 2040
    block_std_head blk_fexit_1_entry
    bget ra
    bnext.ret
    block_text_begin blk_fexit_1_entry
    setc.tgt ra
    block_text_end blk_fexit_1_entry
