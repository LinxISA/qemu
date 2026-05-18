#include "block_type.h"

.global blk_cmp_geu_tt
.section .text
.align 4
blk_cmp_geu_tt:
    block_std_head blk_cmp_geu_tt
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_geu_tt
    get a0
    get a1
    cmp.geu t#2, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_geu_tt

.global blk_cmp_geu_tr
.section .text
.align 4
blk_cmp_geu_tr:
    block_std_head blk_cmp_geu_tr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_geu_tr
    get a0
    cmp.geu t#1, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_geu_tr


.global blk_cmp_geu_rr
.section .text
.align 4
blk_cmp_geu_rr:
    block_std_head blk_cmp_geu_rr
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_geu_rr
    cmp.geu a0, a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_geu_rr

.global blk_cmp_geu_rtl
.section .text
.align 4
blk_cmp_geu_rtl:
    block_std_head blk_cmp_geu_rtl
    bget ra,a0,a1
    bset a0
    bnext.ret
    block_text_begin blk_cmp_geu_rtl
    get a1
    cmp.geu a0, t#1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_geu_rtl

.global blk_cmp_geu_rth
.section .text
.align 4
blk_cmp_geu_rth:
    block_std_head blk_cmp_geu_rth
    bget ra,a1,a0,a4
    bset a0,a4
    bnext.ret
    block_text_begin blk_cmp_geu_rth
    get a1
    set t#1, a4
    get a0
    cmp.geu  t#1, a4
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_cmp_geu_rth

