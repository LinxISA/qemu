.text
.globl _start
_start:
  C.BSTART
  c.movi 0, ->a0
  c.movi 2, ->a1
  C.BSTOP

.Lloop:
  C.BSTART
  BSTART.TLOAD INT32
  C.BSTOP

  C.BSTART
  addi a0, 1, ->a0
  setc.ltu a0, a1
  C.BSTOP

  C.BSTART COND, .Lloop
  C.BSTOP

  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
