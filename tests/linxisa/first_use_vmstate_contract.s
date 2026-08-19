# Linx ECONFIG bank/mask migration contract.
#
# SPDX-License-Identifier: GPL-2.0-or-later

.text
.globl _start
_start:
  C.BSTART
  addi zero, 1, ->a0
  slli a0, 32, ->a0
  addi a0, 8, ->a0
  ssrset a0, 0x0f07

  addi zero, 2, ->a1
  slli a1, 32, ->a1
  addi a1, 8, ->a1
  hl.ssrset a1, 0x1f07

  addi zero, 3, ->a2
  slli a2, 32, ->a2
  addi a2, 8, ->a2
  hl.ssrset a2, 0x2f07

  addi zero, 7, ->a3
  slli a3, 32, ->a3
  addi a3, 15, ->a3
  hl.ssrset a3, 0x3f07

  # Emit the ready marker before a long bounded loop. The host stops the
  # source in this loop, migrates it, then requires the destination to finish.
  addi zero, 82, ->a4
  hl.lui 268435456, ->t
  swi a4, [t#1, 0]
  hl.lui 100000000, ->a5
  C.BSTOP

.Lmigration_wait:
  C.BSTART COND, .Lmigration_wait
  subi a5, 1, ->a5
  setc.ne a5, zero
  C.BSTOP

  C.BSTART COND, .Lfail
  ssrget 0x0f07, ->a0
  addi zero, 1, ->a4
  slli a4, 32, ->a4
  addi a4, 8, ->a4
  xor a0, a4, ->a0

  hl.ssrget 0x1f07, ->a1
  addi zero, 2, ->a4
  slli a4, 32, ->a4
  addi a4, 8, ->a4
  xor a1, a4, ->a1

  hl.ssrget 0x2f07, ->a2
  addi zero, 3, ->a4
  slli a4, 32, ->a4
  addi a4, 8, ->a4
  xor a2, a4, ->a2

  hl.ssrget 0x3f07, ->a3
  addi zero, 3, ->a4
  slli a4, 32, ->a4
  addi a4, 15, ->a4
  xor a3, a4, ->a3

  or a0, a1, ->a0
  or a0, a2, ->a0
  or a0, a3, ->a0
  setc.ne a0, zero
  C.BSTOP

.Lpass:
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
