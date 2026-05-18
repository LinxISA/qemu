#include "block_type.h"

.global blk_ld_ip
.section .text
.align 4
blk_ld_ip:
    block_std_head blk_ld_ip
    bget ra
    bset a0
    bnext.ret
    block_text_begin blk_ld_ip
    ld.ip 1f
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end blk_ld_ip

.pushsection .text.body
1:
    .8byte 0x123456789abcdef0

