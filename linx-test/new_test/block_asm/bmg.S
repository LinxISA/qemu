#include "block_type.h"

.global blk_bmg
.section .text
.align 4
blk_bmg:
    block_aux_head blk_bmg
    bget ra, a0, a1
    bset a0
    bnext.ret
    block_text_begin blk_bmg
    get a0
	get a1
    bmg t#2, t#1
    set t#1, a0
    setc.tgt ra
    block_text_end blk_bmg
