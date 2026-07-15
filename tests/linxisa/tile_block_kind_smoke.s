.text
.globl _start
_start:
  C.BSTART

  # Minimal tile decoupled-header smoke: ensure TMA block kind is observed.
  BSTART.TLOAD INT32
  B.TEXT __linx_tile_empty_body
  C.BSTART

  # canonical finisher PASS value 0x5555 (virt exit MMIO at 0x10009000).
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 2
__linx_tile_empty_body:
  C.BSTOP
