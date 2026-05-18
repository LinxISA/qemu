#include "block_type.h"

.global blk_fentry
.section .text
.align 3
blk_fentry:
    f.entry [a0, a1, a2, a3, a4, a5], a6!, 16376
    block_std_head blk_fentry
    bget ra
    bnext.ret
    block_text_begin blk_fentry
    setc.tgt ra
    block_text_end blk_fentry

.global blk_fentry_1
.section .text
.align 3
blk_fentry_1:
    f.entry [a0, a1, a2, a3, a4, a5, a6], a0!, -2048
    block_std_head blk_fentry_1
    bget ra
    bnext.ret
    block_text_begin blk_fentry_1
    setc.tgt ra
    block_text_end blk_fentry_1

.global blk_fentry_2
.section .text
.align 3
blk_fentry_2:
    block_std_head blk_fentry_2
    bset a1, a2, a3, a4, a5, a6, a7, s0, s1, s2, s3, s4, s5
    block_text_begin blk_fentry_2
    const 12
    set.g t#1, a1
    const 11
    set.g t#1, a2
    const 10
    set.g t#1, s0
    const 9
    set.g t#1, s1
    const 8
    set.g t#1, s2
    const 7
    set.g t#1, s3
    const 6
    set.g t#1, s4
    const 5
    set.g t#1, s5
    const 4
    set.g t#1, a3
    const 3
    set.g t#1, a4
    const 2
    set.g t#1, a5
    const 1
    set.g t#1, a6
    const 0
    set.g t#1, a7
    block_text_end blk_fentry_2
    f.entry [ra, sp, a0, a1, a2, a3, a4,\
    a5, a6, a7, s0, s1, s2, s3, s4, s5], a0!, -16384
    block_std_head blk_ret
    bget ra
    bnext.ret
    block_text_begin blk_ret
    setc.tgt ra
    block_text_end blk_ret
