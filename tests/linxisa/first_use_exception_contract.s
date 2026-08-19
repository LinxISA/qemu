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
  .if HANDLER_MODE >= 0
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

.if HANDLER_MODE >= 0
.p2align 3
first_use_handler:
  C.BSTART COND, first_use_fail
  # Validate the architectural trap state written before EVBASE dispatch.
  hl.ssrget 0x1f02, ->a0  # TRAPNO_ACR1
  addi zero, 3, ->a1
  slli a1, 62, ->a1
  addi zero, 4, ->a6
  slli a6, 24, ->a6
  or a1, a6, ->a1
  xor a0, a1, ->a0
  hl.ssrget 0x1f03, ->a2  # TRAPARG0_ACR1
  xori a2, EXPECTED_KIND, ->a2
  hl.ssrget 0x1f00, ->a3  # ECSTATE_ACR1
  xori a3, 2, ->a3
  hl.ssrget 0x1f41, ->a4  # EBARG_BPC_CUR_ACR1
  hl.ssrget 0x1f43, ->a5  # EBARG_TPC_ACR1
  xor a4, a5, ->a4
  hl.ssrget 0x1f40, ->a6  # EBARG0_ACR1: no block state allocated
  or a0, a2, ->a0
  or a0, a3, ->a0
  or a0, a4, ->a0
  or a0, a6, ->a0
  setc.ne a0, zero
  C.BSTOP

  .if HANDLER_MODE == 0
first_use_handler_pass:
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

first_use_handler_pass_hang:
  C.BSTART DIRECT, first_use_handler_pass_hang
  C.BSTOP
  .else
first_use_handler_resume:
  C.BSTART
    .if CROSS_KIND == 1
    .if RETRY_KIND == 0
  addtpc %tpcrel_hi(first_use_opposite_cube), ->a4
  addi a4, %tpcrel_lo(first_use_opposite_cube), ->a4
    .else
  addtpc %tpcrel_hi(first_use_opposite_vector), ->a4
  addi a4, %tpcrel_lo(first_use_opposite_vector), ->a4
    .endif
  .else
  hl.ssrget 0x1f43, ->a4
  .endif
  .if RETRY_KIND == 0
  addi zero, 2, ->a5
  .else
  addi zero, 1, ->a5
  .endif
  slli a5, 32, ->a5
  addi a5, 8, ->a5
  hl.ssrset a5, 0x1f07
  .if HANDLER_MODE == 1
  addtpc %tpcrel_hi(first_use_retry_complete), ->a6
  addi a6, %tpcrel_lo(first_use_retry_complete), ->a6
  hl.ssrset a6, 0x1f01
  .endif
  hl.ssrset a4, 0x1f41
  hl.ssrset a4, 0x1f43
  acre 0
  .endif

first_use_fail:
  C.BSTART
  hl.lui 13107, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

first_use_retry_complete:
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
.endif

.p2align 3
first_use_opposite_cube:
  BSTART.TMATMUL FP16
  C.BSTOP

.p2align 3
first_use_opposite_vector:
  .2byte 0x88c0  # C.BSTART.VPAR FALL
  C.BSTOP

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
  # BSTART.TMATMUL family with forbidden DataType=15. Decode legality must
  # reject it before the first-use check.
  .4byte 0x78031181
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

.p2align 3
first_use_halt:
  C.BSTART DIRECT, first_use_halt
  C.BSTOP
