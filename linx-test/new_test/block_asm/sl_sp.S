#include "block_type.h"

.global blk_sw_lw_sp
.section .text
.align 4
blk_sw_lw_sp:
    block_std_head blk_sw_lw_sp
    bget ra, a0
    bset a0
    bnext.ret
    block_text_begin blk_sw_lw_sp
    sw a0, [sp,0]
    lw [sp,0]
    cmp.eq t#1, a0
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sw_lw_sp

.global blk_sd_ld_sp
.section .text
.align 4
blk_sd_ld_sp:
    block_std_head blk_sd_ld_sp
    bget ra, a0
    bset a0
    bnext.ret
    block_text_begin blk_sd_ld_sp
    sd a0, [sp,0]
    ld [sp,0]
    cmp.eq t#1, a0
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sd_ld_sp
