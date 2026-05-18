#include "block_type.h"

.global blk_cmp_geui_t
.section .text
.align 4
blk_cmp_geui_t:
    block_std_head blk_cmp_geui_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_cmp_geui_t
    get a0
    cmp.geui t#1, 0x07
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_geui_t

.global blk_cmp_geui_r
.section .text
.align 4
blk_cmp_geui_r:
    block_std_head blk_cmp_geui_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_cmp_geui_r
    cmp.geui a0, 0x07
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_geui_r

