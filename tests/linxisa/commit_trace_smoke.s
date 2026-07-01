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

  # exit code = 0 (virt exit MMIO at 0x10000004)
  addi zero, 0, ->a0
  hl.lui 268435460, ->t
  swi a0, [t#1, 0]

  C.BSTOP

.data
.p2align 3
.Lword:
  .quad 0
