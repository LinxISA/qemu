#include "block_type.h"

.global blk_addtpc_t
.section .text
.align 4
blk_addtpc_t:
    block_std_head blk_addtpc_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_addtpc_t
    get a0
    add tpc, t#1
    set t#2, a0
    get ra
    setc.tgt t#1
    block_text_end blk_addtpc_t

.global blk_addtpc_r
.section .text
.align 4
blk_addtpc_r:
    block_std_head blk_addtpc_r
    bget ra,a0
    bset a0,a1
    bnext.ret
    block_text_begin blk_addtpc_r
    add tpc, a0
    set a0, a0
    get ra
    setc.tgt t#1
    block_text_end blk_addtpc_r

