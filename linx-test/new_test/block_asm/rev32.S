#include "block_type.h"

.global blk_rev32
.section .text
.align 4
blk_rev32:
    block_aux_head blk_rev32
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_rev32
    get a0
    rev32 t#1
    set t#1,a0
    setc.tgt ra
    block_text_end blk_rev32

