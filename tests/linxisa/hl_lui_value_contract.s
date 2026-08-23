.text
.globl _start
_start:
  C.BSTART
  hl.lui 65536, ->a0
  hl.liu 1, ->a1
  hl.liu 48, ->a2
  sll a1, a2, ->a1
  hl.liu 4294967295, ->a3
  hl.lis -1, ->a4
  hl.liu 32, ->a7
  srl a4, a7, ->a5
  hl.liu 1, ->a6
  sub zero, a6, ->a6
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a0, a1
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a3, a5
  C.BSTOP

  C.BSTART COND, .Lfail
  setc.ne a4, a6
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
