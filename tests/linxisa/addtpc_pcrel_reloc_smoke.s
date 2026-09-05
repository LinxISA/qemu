.text
.globl _start
_start:
  C.BSTART
.Lpc:
  addtpc %tpcrel_hi(.Lword), ->a1
  addi zero, 0, ->a0
  addi a1, %tpcrel_lo(.Lpc), ->a1
  ldi [a1, 0], ->a0
  hl.liu 21845, ->a2
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.data
.p2align 12
.Lword:
  .quad 0x5555
