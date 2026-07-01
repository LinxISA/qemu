.text
.globl _start
_start:
  # Block 1: initialize registers (coupled block).
  C.BSTART
  # src = 0x20000, dst = 0x21000
  lui 32, ->a0
  lui 33, ->a1
  # size = 64 bytes, value = 0x5a
  addi zero, 64, ->a2
  addi zero, 90, ->a3
  C.BSTOP

  # Blocks 2-4: template blocks (standalone). Encode as raw words since the
  # current Linx asm parser treats '[' as a memory operand.
  #
  # MSET  [a0, a3, a2]
  .long 0x20511031
  # MSET  [a1, zero, a2]
  .long 0x20019031
  # MCOPY [a1, a0, a2]
  .long 0x20218031

  # Block 5: verify a word and exit (COND block branches to fail on mismatch).
  C.BSTART COND, .Lfail
  lwi [a1, 0], ->a4
  lui 0x5a5a5, ->a5
  addi a5, 0xa5a, ->a5
  setc.ne a4, a5
  addi zero, 0, ->a0
  C.BSTOP

  # PASS: write 0 to the virt exit MMIO register (0x10000004).
  C.BSTART
  hl.lui 268435460, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpass_hang:
  # If QEMU does not terminate immediately on the MMIO exit request, avoid
  # falling through into the FAIL block below.
  C.BSTART DIRECT, .Lpass_hang
  C.BSTOP

.Lfail:
  # FAIL: write 1 to the virt exit MMIO register.
  C.BSTART
  addi zero, 1, ->a0
  hl.lui 268435460, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lfail_hang:
  C.BSTART DIRECT, .Lfail_hang
  C.BSTOP
