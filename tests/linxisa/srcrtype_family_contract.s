# SPDX-License-Identifier: GPL-2.0-or-later
.text
.globl _start
_start:
  # CSEL raw 11 (.neg) negates the false value.
  C.BSTART
  hl.liu 0, ->a0
  hl.liu 7, ->a1
  csel a0, a1, a1.neg, ->a2
  sub zero, a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a2, a3
  C.BSTOP

  # CSEL raw 10 is an unmodified alias.
  C.BSTART
  .4byte 0x14318277
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a2, a1
  C.BSTOP

  # Restricted CMP/SETC raw 11 is an unmodified alias.
  C.BSTART
  hl.liu 7, ->a0
  hl.liu 7, ->a1
  .4byte 0x06310245
  hl.liu 1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a2, a3
  C.BSTOP
  C.BSTART
  hl.lis -7, ->a1
  C.BSTOP
  C.BSTART COND, .Lfail
  .4byte 0x06310065
  C.BSTOP

  # Comparison logical raw 11 is .not for AND and OR.
  C.BSTART
  hl.liu 1, ->a0
  hl.lis -2, ->a1
  cmp.and a0, a1.not, ->a2
  hl.liu 1, ->a3
  hl.liu 0, ->a4
  hl.lis -1, ->a5
  cmp.or a4, a5.not, ->a6
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a2, a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a6, zero
  C.BSTOP

  # Logical SETC raw 11 is .not.
  C.BSTART
  hl.liu 0, ->a0
  hl.lis -1, ->a1
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.or a0, a1.not
  C.BSTOP

  # Address mapping is raw 00=none, 01=.sw, 10=.uw.
  C.BSTART
  hl.liu 4096, ->a0
  hl.liu 1, ->a1
  slli a1, 32, ->a1
  hl.liu 2147483652, ->a2
  add a1, a2, ->a1
  hl.prf.a.l1 [a0, a1], ->a3
  hl.liu 1, ->a4
  slli a4, 32, ->a4
  hl.liu 2147487748, ->a5
  add a4, a5, ->a4
  hl.prf.a.l1 [a0, a1.sw], ->a5
  hl.lis 2147487748, ->a6
  hl.prf.a.l1 [a0, a1.uw], ->a7
  hl.liu 2147487748, ->a0
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a3, a4
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a5, a6
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a7, a0
  C.BSTOP

.Lpass:
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
