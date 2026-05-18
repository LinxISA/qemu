#include "block_type.h"

.global blk_mempop
.section .text
.align 3
blk_mempop:
    .word 0x00030200
    .word 0x00020018
    block_std_head blk_mempop
    bget ra
    bnext.ret
    block_text_begin blk_mempop
    setc.tgt ra
    block_text_end blk_mempop

.global blk_mempp
.section .text
.align 3
blk_mempp:
    .word 0x00020200
    .word 0x00180002
    block_std_head blk_mempp
    bget ra
    bnext.ret
    block_text_begin blk_mempp
    setc.tgt ra
    block_text_end blk_mempp

.global blk_mempop_cover
.section .text
.align 3
blk_mempop_cover:
    .word 0x00030200
    .word 0x0002000c
    block_std_head blk_mempop_cover
    bget ra
    bnext.ret
    block_text_begin blk_mempop_cover
    setc.tgt ra
    block_text_end blk_mempop_cover

.global blk_mempp_cover
.section .text
.align 3
blk_mempp_cover:
    .word 0x00020200
    .word 0x000c0002
    block_std_head blk_mempp_cover
    bget ra
    bnext.ret
    block_text_begin blk_mempp_cover
    setc.tgt ra
    block_text_end blk_mempp_cover


.global blk_mempop14
.section .text
.align 3
blk_mempop14:
    .word 0x00030200
    .word 0x0002fffc
    block_std_head blk_mempop14
    bget ra
    bnext.ret
    block_text_begin blk_mempop14
    setc.tgt ra
    block_text_end blk_mempop14
.global blk_mempp14
.section .text
.align 3
blk_mempp14:
    .word 0x00020200
    .word 0xffff0002
    block_std_head blk_mempp14
    bget ra
    bnext.ret
    block_text_begin blk_mempp14
    setc.tgt ra
    block_text_end blk_mempp14
