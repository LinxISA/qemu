#include "block_type.h"

.global blk_sra_tt
.section .text
.align 4
blk_sra_tt:
    block_std_head blk_sra_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sra_tt
    get a0
    get a1
    sra t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sra_tt

.global blk_sra_tr
.section .text
.align 4
blk_sra_tr:
    block_std_head blk_sra_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_sra_tr
    get a0
    sra t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sra_tr

.global blk_sra_rr
.section .text
.align 4
blk_sra_rr:
    block_std_head blk_sra_rr
    bget ra,a1,a0
    bset a0
    bnext.ret
    block_text_begin blk_sra_rr
    sra a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sra_rr


.global blk_sra_rtl
.section .text
.align 4
blk_sra_rtl:
    block_std_head blk_sra_rtl
    bget ra,a1,a0
    bset a0
    bnext.ret
    block_text_begin blk_sra_rtl
    get a1
    sra a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sra_rtl

.global blk_sra_rth
.section .text
.align 4
blk_sra_rth:
    block_std_head blk_sra_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_sra_rth
    get a1
    set t#1, a4
    get a0
    sra a4, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_sra_rth
