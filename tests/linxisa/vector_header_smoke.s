.text
.globl _start
_start:
  # ET_EXEC entry points must begin at a loader-recognized block header.
  C.BSTART

  # Header-only vector lane-control smoke using fixed compressed forms.
  .2byte 0x88c0  # C.BSTART.VPAR FALL
  C.BSTOP

  .2byte 0xc8c0  # C.BSTART.VSEQ FALL
  C.BSTOP

  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
