.text
.globl _start
_start:
  C.BSTART

  # Minimal Tile decoupled-header smoke: ensure TLSU block kind is observed.
  BSTART.TLOAD S32
  B.TEXT __linx_tile_empty_body
  C.BSTART

  # canonical finisher PASS value 0x5555 (virt exit MMIO at 0x10009000).
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 2
__linx_tile_empty_body:
  C.BSTOP
