#include "block_type.h"

.global blk_lw_ip
.section .text
.align 4
blk_lw_ip:
    block_std_head blk_lw_ip
    bget ra
    bset a0
    bnext.ret
    block_text_begin blk_lw_ip
    lw.ip 1f
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_lw_ip
.pushsection .text.body
1:
    .8byte 0x202302f3173024

