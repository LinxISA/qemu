.global INST_IMM(blk_srai_r, IMMEDIATE)
.section .text
.align 4
INST_IMM(blk_srai_r, IMMEDIATE):
    block_std_head INST_IMM(blk_srai_r, IMMEDIATE)
    bget ra,a0
    bset a0
    bnext.ret
    block_text_begin INST_IMM(blk_srai_r, IMMEDIATE)
    get a0
    srai t#1, IMMEDIATE
    set t#1, a0
    get ra
    setc.tgt t#1
    block_text_end INST_IMM(blk_srai_r, IMMEDIATE)

