#include "block_type.h"

.global blk_sd_sp_r
.section .text
.align 4
blk_sd_sp_r:
    block_std_head blk_sd_sp_r
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sd_sp_r
    get a0
    get a1
    set t#2, sp
    sd a1, [sp, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sd_sp_r

