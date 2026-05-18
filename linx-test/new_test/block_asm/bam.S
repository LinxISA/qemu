#include "block_type.h"

.global blk_bam
.section .text
.align 4
blk_bam:
    block_aux_head blk_bam
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_bam
    get a0
	get a1
    bam t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_bam
