#include "block_type.h"

.global blk_lb_t
.section .text
.align 4
blk_lb_t:
    block_std_head blk_lb_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lb_t
    get a0
    lb [t#1, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lb_t

.global blk_lb_r
.section .text
.align 4
blk_lb_r:
    block_std_head blk_lb_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lb_r
    get a0
    lb [a0, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lb_r

.global blk_lw_r
.section .text
.align 4
blk_lw_r:
    block_std_head blk_lw_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lw_r
    get a0
    lw [a0, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lw_r

.global blk_lw_t
.section .text
.align 4
blk_lw_t:
    block_std_head blk_lw_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lw_t
    get a0
    lw [t#1, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lw_t

.global blk_lwu_t
.section .text
.align 4
blk_lwu_t:
    block_std_head blk_lwu_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lwu_t
    get a0
    lwu [t#1, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lwu_t

.global blk_lwu_r
.section .text
.align 4
blk_lwu_r:
    block_std_head blk_lwu_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lwu_r
    get a0
    lwu [a0, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lwu_r

.global blk_lhu_t
.section .text
.align 4
blk_lhu_t:
    block_std_head blk_lhu_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lhu_t
    get a0
    lhu [t#1, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lhu_t

.global blk_lhu_r
.section .text
.align 4
blk_lhu_r:
    block_std_head blk_lhu_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lhu_r
    get a0
    lhu [a0, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lhu_r

.global blk_lh_t
.section .text
.align 4
blk_lh_t:
    block_std_head blk_lh_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lh_t
    get a0
    lh [t#1, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lh_t

.global blk_lh_r
.section .text
.align 4
blk_lh_r:
    block_std_head blk_lh_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lh_r
    get a0
    lh [a0, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lh_r

.global blk_ld_t
.section .text
.align 4
blk_ld_t:
    block_std_head blk_ld_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_ld_t
    get a0
    ld [t#1, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_ld_t

.global blk_ld_r
.section .text
.align 4
blk_ld_r:
    block_std_head blk_ld_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_ld_r
    get a0
    ld [a0, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_ld_r

.global blk_lbu_r
.section .text
.align 4
blk_lbu_r:
    block_std_head blk_lbu_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lbu_r
    get a0
    lbu [a0, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lbu_r

.global blk_lbu_t
.section .text
.align 4
blk_lbu_t:
    block_std_head blk_lbu_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_lbu_t
    get a0
    lbu [t#1, 0]
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lbu_t

