.text
.globl _start
_start:
  C.BSTART
  addtpc %tpcrel_hi(.Ldata), ->a0
  addi a0, %tpcrel_lo(.Ldata), ->a0
  addtpc %tpcrel_hi(.Lphase), ->a4
  addi a4, %tpcrel_lo(.Lphase), ->a4
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
  lr.w.intra [a0], ->a2
  swi a1, [a4, 0]
  C.BSTOP
.Lwait2:
  C.BSTART COND, .Lwait2
  lwi [a4, 0], ->a2
  addi zero, 2, ->a6
  setc.ne a2, a6
  C.BSTOP
  C.BSTART
  sc.w.intra [a0], a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.eq a3, zero
  C.BSTOP

  C.BSTART
  lr.w.private [a0], ->a2
  addi zero, 3, ->a6
  swi a6, [a4, 0]
  C.BSTOP
.Lwait4:
  C.BSTART COND, .Lwait4
  lwi [a4, 0], ->a2
  addi zero, 4, ->a6
  setc.ne a2, a6
  C.BSTOP
  C.BSTART
  sc.w.private [a0], a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a3, zero
  C.BSTOP

  C.BSTART
  lr.w.intra [a0], ->a2
  addi a0, 1, ->a6
  # Scope mismatch is a probe-free miss even at a misaligned address.
  sc.w.inter [a6], a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.eq a3, zero
  C.BSTOP

  C.BSTART
  lr.w.inter [a0], ->a2
  addi zero, 5, ->a6
  swi a6, [a4, 0]
  C.BSTOP
.Lwait6:
  C.BSTART COND, .Lwait6
  lwi [a4, 0], ->a2
  addi zero, 6, ->a6
  setc.ne a2, a6
  C.BSTOP
  C.BSTART
  sc.w.inter [a0], a1, ->a3
  C.BSTOP
  C.BSTART COND, .Lfail
  setc.ne a3, zero
  C.BSTOP
  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpe1:
.Lwait1:
  C.BSTART COND, .Lwait1
  lwi [a4, 0], ->a2
  addi zero, 1, ->a6
  setc.ne a2, a6
  C.BSTOP
  C.BSTART
  sw.add.intra [a0], a1
  addi zero, 2, ->a6
  swi a6, [a4, 0]
  C.BSTOP
.Lwait3:
  C.BSTART COND, .Lwait3
  lwi [a4, 0], ->a2
  addi zero, 3, ->a6
  setc.ne a2, a6
  C.BSTOP
  C.BSTART
  sw.add.private [a0], a1
  addi zero, 4, ->a6
  swi a6, [a4, 0]
  C.BSTOP
.Lwait5:
  C.BSTART COND, .Lwait5
  lwi [a4, 0], ->a2
  addi zero, 5, ->a6
  setc.ne a2, a6
  C.BSTOP
  C.BSTART
  sw.add.inter [a5], a1
  addi zero, 6, ->a6
  swi a6, [a4, 0]
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
.Lphase:
  .4byte 0
  .space 60
.Loutside:
  .4byte 9
  .space 60
