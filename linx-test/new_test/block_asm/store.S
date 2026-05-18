#include "block_type.h"

.global blk_sw_bd
.section .text
.align 4
blk_sw_bd:
    block_std_head blk_sw_bd
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sw_bd
    get a0
    get a1
    sw t#1, [a0, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sw_bd

.global blk_sw_db
.section .text
.align 4
blk_sw_db:
    block_std_head blk_sw_db
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sw_db
    get a0
    get a1
    sw a1, [t#2, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sw_db

.global blk_sw_rr1
.section .text
.align 4
blk_sw_rr1:
    block_std_head blk_sw_rr1
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sw_rr1
    get a0
    get a1
    sw a1, [a0, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sw_rr1

.global blk_sw_rr0
.section .text
.align 4
blk_sw_rr0:
    block_std_head blk_sw_rr0
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sw_rr0
    get a0
    get a1
    sw a1, [a0, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sw_rr0

.global blk_sw_t
.section .text
.align 4
blk_sw_t:
    block_std_head blk_sw_t
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sw_t
    get a0
    get a1
    sw t#1, [t#2, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sw_t

.global blk_sh_t
.section .text
.align 4
blk_sh_t:
    block_std_head blk_sh_t
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sh_t
    get a0
    get a1
    sh t#1, [t#2, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sh_t

.global blk_sb_t
.section .text
.align 4
blk_sb_t:
    block_std_head blk_sb_t
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sb_t
    get a0
    get a1
    sb t#1, [t#2, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sb_t

.global blk_sd_bd
.section .text
.align 4
blk_sd_bd:
    block_std_head blk_sd_bd
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sd_bd
    get a0
    get a1
    sd t#1, [a0, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sd_bd

.global blk_sd_db
.section .text
.align 4
blk_sd_db:
    block_std_head blk_sd_db
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sd_db
    get a0
    get a1
    sd a1, [t#2, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sd_db

.global blk_sd_rr0
.section .text
.align 4
blk_sd_rr0:
    block_std_head blk_sd_rr0
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sd_rr0
    get a0
    get a1
    sd a1, [a0, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sd_rr0

.global blk_sd_rr1
.section .text
.align 4
blk_sd_rr1:
    block_std_head blk_sd_rr1
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sd_rr1
    get a0
    get a1
    sd a1, [a0, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sd_rr1

.global blk_sd_t
.section .text
.align 4
blk_sd_t:
    block_std_head blk_sd_t
    bget ra,a0,a1
    bnext.ret
    block_text_begin blk_sd_t
    get a0
    get a1
    sd t#1, [t#2, 0]
    get ra
    setc.tgt t#1
    block_text_end blk_sd_t

