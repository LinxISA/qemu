#include "block_type.h"

.global blk_ftexit
.section .text
.align 3
blk_ftexit:
    f.texit [a1, a2, a3, a4, a5, a6, a7], a0!, ra, 2040
    block_std_head blk_ftexit
    bget ra
    bnext.ret
    block_text_begin blk_ftexit
    setc.tgt ra
    block_text_end blk_ftexit

.global blk_ftexit_fentry
.section .text
.align 3
blk_ftexit_fentry:
    f.entry [a1, a2, a3, a4, a5, a6, a7], a0!, 960
    block_std_head blk_ftexit_fentry
    bget ra
    bnext.ret
    block_text_begin blk_ftexit_fentry
    setc.tgt ra
    block_text_end blk_ftexit_fentry
.global blk_ftexit_1
.section .text
.align 3
blk_ftexit_1:
    f.texit [a1, a2, a3, a4, a5, a6, a7], a0!, ra, -2048
    block_std_head blk_ftexit_1
    bget ra
    bnext.ret
    block_text_begin blk_ftexit_1
    setc.tgt ra
    block_text_end blk_ftexit_1

.global blk_ftexit_1_fentry
.section .text
.align 3
blk_ftexit_1_fentry:
    f.entry [a1, a2, a3, a4, a5, a6, a7], a0!, 960
    block_std_head blk_ftexit_1_fentry
    bget ra
    bnext.ret
    block_text_begin blk_ftexit_1_fentry
    setc.tgt ra
    block_text_end blk_ftexit_1_fentry
