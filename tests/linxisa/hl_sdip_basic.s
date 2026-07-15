.text
.globl _start
_start:
  C.BSTART
  addtpc .Lvals, ->a5
  addi a5, .Lvals, ->a5
  ldi [a5, 0], ->a1
  ldi [a5, 8], ->a2
  addtpc .Lbuf, ->a0
  addi a0, .Lbuf, ->a0
  C.BSTOP

  # Aligned, scaled offset case: simm=4 means +32 bytes for HL.SDIP.
  # Encoding is emitted raw because current llvm-mc parser rejects SrcD1 text
  # operands for hl.sdip{.u}, but decode/execute/disasm all support it.
  C.BSTART
  .4byte 0xb059011e
  .2byte 0x0821
  C.BSTOP

  C.BSTART
  ldi [a0, 32], ->a3
  ldi [a0, 40], ->a4
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a3, a1
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a4, a2
  C.BSTOP

  # Unaligned, unscaled offset case: simm=1 means +1 byte for HL.SDIP.U.
  C.BSTART
  .4byte 0xf059011e
  .2byte 0x0221
  C.BSTOP

  C.BSTART
  lbui [a0, 1], ->a3
  lbui [a0, 8], ->a4
  lbui [a0, 9], ->a5
  lbui [a0, 16], ->a6
  addi zero, 136, ->s0
  addi zero, 17, ->s1
  addi zero, 0, ->s2
  addi zero, 153, ->s3
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a3, s0
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a4, s1
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a5, s2
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a6, s3
  C.BSTOP

  # PASS: write 0x5555 to the canonical finisher (0x10009000).
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpass_hang:
  C.BSTART DIRECT, .Lpass_hang
  C.BSTOP

.Lfail:
  # FAIL: write 0x3333 to the canonical finisher.
  C.BSTART
  hl.lui 13107, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lfail_hang:
  C.BSTART DIRECT, .Lfail_hang
  C.BSTOP

.data
.p2align 3
.Lvals:
  .quad 0x1122334455667788
  .quad 0x99aabbccddeeff00

.bss
.p2align 3
.Lbuf:
  .zero 80
