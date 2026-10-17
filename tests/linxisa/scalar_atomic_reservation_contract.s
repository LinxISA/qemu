.macro CHECK_ZERO reg
  C.BSTART COND, .Lfail
  setc.ne \reg, zero
  C.BSTOP
.endm

.macro CHECK_NONZERO reg
  C.BSTART COND, .Lfail
  setc.eq \reg, zero
  C.BSTOP
.endm

.text
.globl _start
_start:
  C.BSTART
  addtpc %tpcrel_hi(.Lreservation_line), ->a0
  addi a0, %tpcrel_lo(.Lreservation_line), ->a0
  addtpc %tpcrel_hi(.Lother_line), ->a4
  addi a4, %tpcrel_lo(.Lother_line), ->a4
  addtpc %tpcrel_hi(.Lnegative_word), ->a6
  addi a6, %tpcrel_lo(.Lnegative_word), ->a6
  ldi [a6, 0], ->a7
  addi a0, 4, ->a5
  addi zero, 1, ->a1
  C.BSTOP

  # Word-valued fetch atomics sign-extend their returned old value to XLEN.
  C.BSTART
  SWAPW [a4], a1, ->a2
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a2, a7
  C.BSTOP

  C.BSTART
  swi a7, [a4, 0]
  LW.ADD [a4], a1, ->a2
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a2, a7
  C.BSTOP

  # Ordinary scalar stores share the same commit-time reservation rule.
  C.BSTART
  LR.W [a0], ->a2
  swi a1, [a4, 0]
  SC.W [a0], a1, ->a3
  C.BSTOP
  CHECK_ZERO a3

  C.BSTART
  addi a0, 8, ->a5
  LR.W [a0], ->a2
  swi a1, [a5, 0]
  SC.W [a0], a1, ->a3
  C.BSTOP
  CHECK_NONZERO a3

  # A successful non-overlapping RMW preserves the reservation. SC.B uses a
  # different address and width in the same 64-byte reservation granule.
  C.BSTART
  LR.W [a0], ->a2
  SW.ADD [a4], a1
  SC.B [a5], a1, ->a3
  C.BSTOP
  CHECK_ZERO a3

  # A successful overlapping RMW invalidates the reservation.
  C.BSTART
  LR.W [a0], ->a2
  SW.ADD [a5], a1
  SC.W [a0], a1, ->a3
  C.BSTOP
  CHECK_NONZERO a3

  # CAS mismatch is read-only and preserves the reservation.
  C.BSTART
  addi zero, 99, ->a6
  addi zero, 7, ->a7
  LR.W [a0], ->a2
  CASW [a0], a6, a7, ->a2
  SC.W [a0], a1, ->a3
  C.BSTOP
  CHECK_ZERO a3

  # CAS match performs an overlapping write and invalidates the reservation.
  C.BSTART
  LR.W [a0], ->a2
  CASW [a0], a1, a7, ->a2
  SC.W [a0], a1, ->a3
  C.BSTOP
  CHECK_NONZERO a3

  # A reservation miss is probe-free: even a misaligned address returns
  # failure instead of raising DataAlignment.
  C.BSTART
  addi a0, 1, ->a5
  SC.W [a5], a1, ->a3
  C.BSTOP
  CHECK_NONZERO a3

  # DMA snapshots all 64 source bytes before committing an overlapping copy,
  # then invalidates a reservation whose granule overlaps the destination.
  C.BSTART
  addtpc %tpcrel_hi(.Ldma_bytes), ->a6
  addi a6, %tpcrel_lo(.Ldma_bytes), ->a6
  addi a6, 8, ->a7
  LR.W [a6], ->a2
  DMA [a6], a7
  lbui [a7, 0], ->a2
  lbui [a7, 8], ->a4
  SC.W [a6], a1, ->a3
  C.BSTOP
  CHECK_ZERO a2
  C.BSTART COND, .Lfail
  addi zero, 8, ->a5
  setc.ne a4, a5
  C.BSTOP
  CHECK_NONZERO a3

.Lpass:
  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lfail:
  C.BSTART
  hl.liu 13107, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.data
.balign 64
.Lreservation_line:
  .4byte 5
  .space 60
.Lother_line:
  .4byte 0x80000000
  .space 60
.Lnegative_word:
  .quad 0xffffffff80000000
.balign 64
.Ldma_bytes:
  .byte 0, 1, 2, 3, 4, 5, 6, 7
  .byte 8, 9, 10, 11, 12, 13, 14, 15
  .byte 16, 17, 18, 19, 20, 21, 22, 23
  .byte 24, 25, 26, 27, 28, 29, 30, 31
  .byte 32, 33, 34, 35, 36, 37, 38, 39
  .byte 40, 41, 42, 43, 44, 45, 46, 47
  .byte 48, 49, 50, 51, 52, 53, 54, 55
  .byte 56, 57, 58, 59, 60, 61, 62, 63
  .space 8
