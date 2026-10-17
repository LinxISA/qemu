# SPDX-License-Identifier: GPL-2.0-or-later
.text
.globl _start
_start:
  C.BSTART
  addtpc %tpcrel_hi(.Lhandler), ->a2
  addi a2, %tpcrel_lo(.Lhandler), ->a2
  hl.ssrset a2, 0x1f01
  addtpc %tpcrel_hi(.Ltarget), ->a0
  addi a0, %tpcrel_lo(.Ltarget), ->a0
  addi zero, 1, ->a1
  ssrset a1, 0x0f00
  ssrset a0, 0x0f41
  ssrset a0, 0x0f43
  acre 0
  C.BSTOP

.p2align 3
.Ltarget:
  C.BSTART
  hl.liu 291, ->a1
  C.BSTOP

  C.BSTART
  # Raw LB with SrcRType=11, RegDst=a1 and every source/shamt zero.
  # The reserved selector must trap before changing the destination.
  .4byte 0x06000189
  C.BSTOP
  C.BSTART DIRECT, .Lfail
  C.BSTOP

.Lhandler:
  C.BSTART COND, .Lfail
  xori a1, 291, ->a0
  setc.ne a0, zero
  C.BSTOP
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
