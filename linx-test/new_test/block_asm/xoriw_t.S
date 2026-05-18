.global INST_IMM(blk_xoriw_t, IMMEDIATE)
.section .text
.align 4
INST_IMM(blk_xoriw_t, IMMEDIATE):
    block_std_head INST_IMM(blk_xoriw_t, IMMEDIATE)
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin INST_IMM(blk_xoriw_t, IMMEDIATE)
    get a0
    xoriw t#1, IMMEDIATE
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end INST_IMM(blk_xoriw_t, IMMEDIATE)

