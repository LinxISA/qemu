#include "block_type.h"

.global blk_mempush
.section .text
.align 3
blk_mempush:
    b.mpush [a0,a1], [a2!]
    block_std_head blk_mempush
    bget ra
    bnext.ret
    block_text_begin blk_mempush
    setc.tgt ra
    block_text_end blk_mempush



.global blk_mempush_cover
.section .text
.align 3
blk_mempush_cover:
    .word 0x00020200
    .word 0x000c0002
    block_std_head blk_mempush_cover
    bget ra
    bnext.ret
    block_text_begin blk_mempush_cover
    setc.tgt ra
    block_text_end blk_mempush_cover



.global blk_mempush16
.section .text
.align 3
blk_mempush16:
    block_std_head blk_mempush16
    bset a1, a2, a3, a4, a5, a6, a7, s0, s1, s2, s3, s4, s5
    block_text_begin blk_mempush16
    const 13
    set.g t#1, a1
    set.g t#2, a2
    set.g t#3, s0
    set.g t#4, s1
    set.g t#5, s2
    const 15
    set.g t#1, s3
    set.g t#2, s4
    set.g t#3, s5
    set.g t#4, a3
    set.g t#5, a4
    set.g t#6, a5
    set.g t#7, a6
    set.g t#8, a7
    block_text_end blk_mempush16
    b.mpush [ra, sp, a0, a1, a2, a3, a4, a5, a6, a7, s0, s1, s2, s3, s4, s5], [a0!]
    block_std_head blk_ret
    bget ra
    bnext.ret
    block_text_begin blk_ret
    setc.tgt ra
    block_text_end blk_ret
