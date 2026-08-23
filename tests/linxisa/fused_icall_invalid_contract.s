.text
.globl _start
_start:
  C.BSTART
  addtpc %tpcrel_hi(fused_icall_invalid_target), ->a0
  addi a0, %tpcrel_lo(fused_icall_invalid_target), ->a0
  addi a0, 2, ->a0
  c.setc.tgt a0
  C.BSTOP

.globl fused_icall_invalid_target
fused_icall_invalid_target:
  .4byte 0x50166001
  C.BSTOP

  # Reaching a finisher proves validation happened too late or not at all.
  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
