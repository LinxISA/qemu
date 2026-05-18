#include "block_type.h"

.global blk_bci_t
.section .text
.align 4
blk_bci_t:
    block_stdh_head blk_bci_t
    bget ra,a0,a1
    bset a0,a1
    bnext.ret
    block_text_begin blk_bci_t
    get a0
    get a1
    const 4
    cmp.eq t#1, t#2
    #bci t#1, 4
    set t#2, a1
    get a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_bci_t


.global blk_bci_r
.section .text
.align 4
blk_bci_r:
    block_stdh_head blk_bci_r
    bget ra,a0,a1,a2
    bset a0,a1,a2
    bnext.ret
    block_text_begin blk_bci_r
    const 4
    set t#1, a2
    cmp.eq a0, a1
    set t#1, a2
    #bci a2, 4
    set t#2, a1
    get a1
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_bci_r

