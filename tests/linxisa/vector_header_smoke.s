.text
.globl _start
_start:
  # Header-only vector lane-control smoke using fixed compressed forms.
  .2byte 0x88c0  # C.BSTART.VPAR FALL
  C.BSTOP

  .2byte 0xc8c0  # C.BSTART.VSEQ FALL
  C.BSTOP

  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
