.text
.globl _start
_start:
  C.BSTART
  addtpc %tpcrel_hi(.Ldata), ->a0
  addi a0, %tpcrel_lo(.Ldata), ->a0
  addtpc %tpcrel_hi(.Lflag0), ->a4
  addi a4, %tpcrel_lo(.Lflag0), ->a4
  addtpc %tpcrel_hi(.Loutside), ->a5
  addi a5, %tpcrel_lo(.Loutside), ->a5
  ssrget 0x0802, ->a7
  C.BSTOP

  C.BSTART COND, .Lpe0
  setc.eq a7, zero
  C.BSTOP
  C.BSTART COND, .Lpe1
  addi zero, 1, ->a6
  setc.eq a7, a6
  C.BSTOP
  C.BSTART DIRECT, .Lpark
  C.BSTOP

.Lpe0:
  C.BSTART
  addi zero, 1, ->a1
  LR.W [a0], ->a2
  swi a1, [a4, 0]
  C.BSTOP
.Lpe0_wait:
  C.BSTART COND, .Lpe0_wait
  lwi [a4, 4], ->a2
  setc.eq a2, zero
  C.BSTOP
  C.BSTART
  SC.W [a0], a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a3, zero
  C.BSTOP

  # A same-value write to a different byte in the reservation granule still
  # invalidates the monitor (ABA does not preserve it).
  C.BSTART
  LR.W [a0], ->a2
  swi a1, [a4, 8]
  C.BSTOP
.Lpe0_wait_second:
  C.BSTART COND, .Lpe0_wait_second
  lwi [a4, 12], ->a2
  setc.eq a2, zero
  C.BSTOP
  C.BSTART
  SC.W [a0], a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.eq a3, zero
  C.BSTOP

  # A successful peer SC is a committed write and invalidates this PE's
  # reservation when it touches the same granule.
  C.BSTART
  LR.W [a0], ->a2
  swi a1, [a4, 16]
  C.BSTOP
.Lpe0_wait_third:
  C.BSTART COND, .Lpe0_wait_third
  lwi [a4, 20], ->a2
  setc.eq a2, zero
  C.BSTOP
  C.BSTART
  SC.W [a0], a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.eq a3, zero
  C.BSTOP
  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpe1:
.Lpe1_wait:
  C.BSTART COND, .Lpe1_wait
  lwi [a4, 0], ->a2
  setc.eq a2, zero
  C.BSTOP
  C.BSTART
  addi zero, 1, ->a1
  swi a1, [a5, 0]
  swi a1, [a4, 4]
  C.BSTOP
.Lpe1_wait_second:
  C.BSTART COND, .Lpe1_wait_second
  lwi [a4, 8], ->a2
  setc.eq a2, zero
  C.BSTOP
  C.BSTART
  addi a0, 4, ->a6
  sbi zero, [a6, 0]
  swi a1, [a4, 12]
  C.BSTOP
.Lpe1_wait_third:
  C.BSTART COND, .Lpe1_wait_third
  lwi [a4, 16], ->a2
  setc.eq a2, zero
  C.BSTOP
  C.BSTART
  addi a0, 8, ->a6
  LR.W [a6], ->a2
  SC.W [a6], a1, ->a3
  swi a1, [a4, 20]
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a3, zero
  C.BSTOP
.Lpark:
  C.BSTART DIRECT, .Lpark
  C.BSTOP

.Lfail:
  C.BSTART
  hl.liu 13107, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.data
.balign 64
.Ldata:
  .4byte 7
  .space 60
.Lflag0:
  .4byte 0, 0, 0, 0
  .space 48
.Loutside:
  .4byte 9
  .space 60
