.text
.globl _start
_start:
  C.BSTART

  # r/w to a data word (exercise wb + mem trace)
  addtpc .Lword, ->a1
  addi a1, .Lword, ->a1
  addi zero, 0x678, ->a0
  sdi a0, [a1, 0]
  ldi [a1, 0], ->a2

  # canonical finisher PASS value 0x5555 (virt exit MMIO at 0x10009000)
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]

  C.BSTOP

.data
.p2align 3
.Lword:
  .quad 0
