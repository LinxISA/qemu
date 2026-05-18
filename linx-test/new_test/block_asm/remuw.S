#include "block_type.h"

.global blk_remuw
.section .text
.align 4
blk_remuw:
    block_aux_head blk_remuw
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_remuw
    get a0
	get a1
    remuw t#2, t#1
    set t#1,a0
    setc.tgt ra
    block_text_end blk_remuw
