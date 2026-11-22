.text
.globl _start
_start:
  C.BSTART
  hl.liu 268472320, ->a0
  hl.liu 21845, ->a1
  swapw.private [a0], a1, ->a2
  C.BSTOP
