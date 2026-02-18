.text
.globl _start
_start:
  C.BSTART

  # Minimal tile decoupled-header smoke: ensure TMA block kind is observed.
  BSTART.TLOAD INT32
  B.TEXT __linx_tile_empty_body
  C.BSTART

  # Exit code = 0 (virt exit MMIO at 0x10000004).
  addi zero, 0, ->a0
  hl.lui 268435460, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 2
__linx_tile_empty_body:
  C.BSTOP
