.text
.globl _start
_start:
  C.BSTART
  # Minimal tile descriptor/header path that both QEMU and pyCircuit can run.
  BSTART.TLOAD S32
  B.TEXT __linx_tile_empty_body
  C.BSTART

  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 2
__linx_tile_empty_body:
  C.BSTOP
