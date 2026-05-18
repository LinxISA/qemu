#include "block_type.h"

.global blk_sd_sp_t
.section .text
.align 4
blk_sd_sp_t:
    block_std_head blk_sd_sp_t
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sd_sp_t
    get a0
    get a1
    set t#2, sp
    sd t#2, [sp, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sd_sp_t

