#include "block_type.h"

.global blk_cmp_ltui_t
.section .text
.align 4
blk_cmp_ltui_t:
    block_std_head blk_cmp_ltui_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ltui_t
    get a0
    cmp.ltui t#1, 0x09
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ltui_t

.global blk_cmp_ltui_r
.section .text
.align 4
blk_cmp_ltui_r:
    block_std_head blk_cmp_ltui_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_cmp_ltui_r
    cmp.ltui a0, 0x09
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_ltui_r

