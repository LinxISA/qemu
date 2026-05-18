#include "block_type.h"

.global blk_remu
.section .text
.align 4
blk_remu:
    block_aux_head blk_remu
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_remu
    get a0
	get a1
    remu t#2, t#1
    set t#1,a0
    setc.tgt ra
    block_text_end blk_remu

