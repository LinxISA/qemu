.text
.globl _start
_start:
  # Seed an address calculation whose result is nonzero and observable.
  C.BSTART
  lui 1, ->a0
  addi zero, 7, ->a1
  addi zero, 0, ->a2
  C.BSTOP

  # Canonical HL.PRF with zero base/index, L1 model, and shift zero.
  # Keep this raw so the regression tests QEMU's exact 0x00e low-form decode
  # independently of the assembler mnemonic surface.
  C.BSTART
  .4byte 0x7009000e
  .2byte 0x0000
  C.BSTOP

  # Raw HL.PRF.A.L1 [a0, a1.sw << 3], ->a2.
  # RegDst=4 (a2), SrcL=2 (a0), SrcR=3 (a1), SrcRType=0,
  # model=0, shamt=3.  Expected EA is 0x1000 + (7 << 3) = 0x1038.
  C.BSTART
  .4byte 0x7209001e
  .2byte 0x1831
  C.BSTOP

  C.BSTART COND, .Lfail
  addi a0, 56, ->a3
  setc.ne a2, a3
  C.BSTOP

  # PASS: both raw forms decoded and HL.PRF.A wrote the expected EA.
  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpass_hang:
  C.BSTART DIRECT, .Lpass_hang
  C.BSTOP

.Lfail:
  C.BSTART
  hl.liu 13107, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lfail_hang:
  C.BSTART DIRECT, .Lfail_hang
  C.BSTOP
