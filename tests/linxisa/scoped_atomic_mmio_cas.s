.text
.globl _start
_start:
  C.BSTART
  hl.liu 268472320, ->a0
  hl.liu 0, ->a1
  hl.liu 21845, ->a2
  casw.private [a0], a1, a2, ->a3
  C.BSTOP
