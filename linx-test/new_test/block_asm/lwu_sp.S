#include "block_type.h"

.global blk_lwu_sp
.section .text
.align 4
blk_lwu_sp:
    block_std_head blk_lwu_sp
    bget ra,a0,sp
    bset a0
    bnext.ret
    block_text_begin blk_lwu_sp
    get a0
    set t#1, a0
    lwu [sp, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lwu_sp

