.text
.globl _start
_start:
  C.BSTART
  hl.liu 0, ->a0
  hl.liu 7, ->a1
  hl.liu 5, ->a2
  csel a0, a1, a2, ->a3
  csel a0, a1, a2.neg, ->a4
  hl.liu 5, ->a5
  hl.liu 1, ->a6
  sub zero, a5, ->a7
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a3, a5
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a4, a7
  C.BSTOP

  C.BSTART
  hl.liu 1, ->a0
  csel a0, a1, a2, ->a3
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a3, a1
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
