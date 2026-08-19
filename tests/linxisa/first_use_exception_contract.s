# Linx VECTOR/CUBE first-use exception executable fixture.
#
# SPDX-License-Identifier: GPL-2.0-or-later

.text
.globl _start
_start:
.if SOURCE_ACR == 0
  C.BSTART DIRECT, first_use_target
  C.BSTOP
.else
  C.BSTART
  .if RETRY_KIND >= 0
  addtpc %tpcrel_hi(first_use_handler), ->a2
  addi a2, %tpcrel_lo(first_use_handler), ->a2
  hl.ssrset a2, 0x1f01
  .endif
  addtpc %tpcrel_hi(first_use_target), ->a0
  addi a0, %tpcrel_lo(first_use_target), ->a0
  addi zero, SOURCE_ACR, ->a1
  ssrset a1, 0x0f00
  ssrset a0, 0x0f41
  ssrset a0, 0x0f43
  acre 0
  C.BSTOP
.endif

.if RETRY_KIND >= 0
.p2align 3
first_use_handler:
  C.BSTART
  hl.ssrget 0x1f43, ->a4
  .if RETRY_KIND == 0
  hl.lui 8589934592, ->a5
  .else
  hl.lui 4294967296, ->a5
  .endif
  addi a5, 8, ->a5
  hl.ssrset a5, 0x1f07
  hl.ssrset a4, 0x1f41
  hl.ssrset a4, 0x1f43
  acre 0
.endif

.p2align 3
first_use_target:
.if TEST_CASE == 0
  BSTART.MPAR 0
.elseif TEST_CASE == 1
  BSTART.MSEQ 0
.elseif TEST_CASE == 2
  BSTART.VPAR 0
.elseif TEST_CASE == 3
  BSTART.VSEQ 0
.elseif TEST_CASE == 4
  .2byte 0x08c0  # C.BSTART.MPAR FALL
.elseif TEST_CASE == 5
  .2byte 0x48c0  # C.BSTART.MSEQ FALL
.elseif TEST_CASE == 6
  .2byte 0x88c0  # C.BSTART.VPAR FALL
.elseif TEST_CASE == 7
  .2byte 0xc8c0  # C.BSTART.VSEQ FALL
.elseif TEST_CASE == 8
  BSTART.TMATMUL FP16
.elseif TEST_CASE == 9
  BSTART.TMATMUL.BIAS FP16
.elseif TEST_CASE == 10
  BSTART.TMATMUL.ACC FP16
.elseif TEST_CASE == 11
  BSTART.TMATMULMX FP16
.elseif TEST_CASE == 12
  BSTART.TMATMULMX.BIAS FP16
.elseif TEST_CASE == 13
  BSTART.TMATMULMX.ACC FP16
.elseif TEST_CASE == 14
  BSTART.TGEMV FP16
.elseif TEST_CASE == 15
  BSTART.TGEMV.BIAS FP16
.elseif TEST_CASE == 16
  BSTART.TGEMV.ACC FP16
.elseif TEST_CASE == 17
  BSTART.TGEMVMX FP16
.elseif TEST_CASE == 18
  BSTART.TGEMVMX.BIAS FP16
.elseif TEST_CASE == 19
  BSTART.TGEMVMX.ACC FP16
.elseif TEST_CASE == 20
  BSTART.TEPL 0, 1, FP16
.elseif TEST_CASE == 21
  .4byte 0xffffffff
.elseif TEST_CASE == 22
  .2byte 0x88c0  # C.BSTART.VPAR FALL
  C.BSTOP
  BSTART.TMATMUL FP16
.elseif TEST_CASE == 23
  BSTART.TMATMUL FP16
  C.BSTOP
  .2byte 0x88c0  # C.BSTART.VPAR FALL
.else
  .error "unknown first-use executable TEST_CASE"
.endif
  C.BSTOP

first_use_pass:
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
