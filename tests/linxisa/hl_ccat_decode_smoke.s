.macro CHECK_PAIR offset0, offset1
  C.BSTART
  ldi [a6, \offset0], ->a4
  ldi [a6, \offset1], ->a5
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a2, a4
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a3, a5
  C.BSTOP
.endm

.text
.globl _start
_start:
  C.BSTART
  addtpc .Lvalues, ->a6
  addi a6, .Lvalues, ->a6
  ldi [a6, 0], ->a0
  ldi [a6, 8], ->a1
  C.BSTOP

  # Raw HL.CCAT a0, a1, 0, ->a2, a3.
  C.BSTART
  .4byte 0x125d280e
  .2byte 0x0031
  C.BSTOP
  CHECK_PAIR 16, 24

  # Shift 63 sets both formerly-fixed shamt[1:0] encoding bits.
  C.BSTART
  .4byte 0x125d280e
  .2byte 0x7e31
  C.BSTOP
  CHECK_PAIR 32, 40

  # Exercise the 128-bit half boundary and both sides of it.
  C.BSTART
  .4byte 0x125d280e
  .2byte 0x8031
  C.BSTOP
  CHECK_PAIR 48, 56

  C.BSTART
  .4byte 0x125d280e
  .2byte 0x8231
  C.BSTOP
  CHECK_PAIR 64, 72

  C.BSTART
  .4byte 0x125d280e
  .2byte 0xfe31
  C.BSTOP
  CHECK_PAIR 80, 88

  # Raw HL.CCATW verifies lane sign extension and 32/64-bit boundaries.
  C.BSTART
  .4byte 0x225d280e
  .2byte 0x0031
  C.BSTOP
  CHECK_PAIR 96, 104

  C.BSTART
  .4byte 0x225d280e
  .2byte 0x3e31
  C.BSTOP
  CHECK_PAIR 112, 120

  C.BSTART
  .4byte 0x225d280e
  .2byte 0x4031
  C.BSTOP
  CHECK_PAIR 128, 136

  C.BSTART
  .4byte 0x225d280e
  .2byte 0x7e31
  C.BSTOP
  CHECK_PAIR 144, 152

  C.BSTART
  .4byte 0x225d280e
  .2byte 0x8031
  C.BSTOP
  CHECK_PAIR 160, 168

  # PASS: write 0x5555 to the canonical finisher.
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpass_hang:
  C.BSTART DIRECT, .Lpass_hang
  C.BSTOP

.Lfail:
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
.Lvalues:
  .quad 0x8123456789abcdef
  .quad 0xfedcba9876543210
  # HL.CCAT expected pairs for shifts 0, 63, 64, 65, and 127.
  .quad 0xfedcba9876543210, 0x8123456789abcdef
  .quad 0x02468acf13579bdf, 0x0000000000000001
  .quad 0x8123456789abcdef, 0x0000000000000000
  .quad 0x4091a2b3c4d5e6f7, 0x0000000000000000
  .quad 0x0000000000000001, 0x0000000000000000
  # HL.CCATW expected sign-extended pairs for shifts 0, 31, 32, 63, and 64.
  .quad 0x0000000076543210, 0xffffffff89abcdef
  .quad 0x0000000013579bde, 0x0000000000000001
  .quad 0xffffffff89abcdef, 0x0000000000000000
  .quad 0x0000000000000001, 0x0000000000000000
  .quad 0x0000000000000000, 0x0000000000000000
