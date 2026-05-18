#include "block_type.h"

.global blk_addbpc_t
.section .text

.align 4
blk_addbpc_t:
    block_std_head blk_addbpc_t
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_addbpc_t
    get a0
    add bpc, t#1
    set t#2, a0
    get ra
    setc.tgt t#1
    block_text_end blk_addbpc_t

.global blk_addbpc_r
.section .text
.align 4
blk_addbpc_r:
    block_std_head blk_addbpc_r
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin blk_addbpc_r
    add bpc, a0
    set a0, a0
    get ra
    setc.tgt t#1
    block_text_end blk_addbpc_r

