.text
.globl _start
_start:
  C.BSTART

  # Mainline toolchain ADDTPC: destination = (TPC & ~0xFFF) + (SignExtend(imm) << 12).
  addtpc 2, ->a1
  addtpc -2, ->a2
  hl.addtpc 2, ->a3
  hl.addtpc -2, ->a4

  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]

  C.BSTOP
