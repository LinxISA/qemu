.text
.globl _start
_start:
  # Install a minimal trap vector for ACR0 so unexpected MMU faults exit.
  C.BSTART
  addtpc .Ltrap_vector, ->a0
  addi a0, .Ltrap_vector, ->a0
  ssrset a0, 0x0f01
  C.BSTOP

  # Program TTBR0/TTBR1 + enable MME (ACR1 bank) and then jump to a TTBR1-mapped
  # high-half alias of high_entry. If TTBR1 selection or the page-walk logic is
  # broken, the program will trap and exit via .Ltrap_vector.
  C.BSTART IND
  # TTBR0 = &L0_ttbr0
  addtpc L0_ttbr0, ->a0
  addi a0, L0_ttbr0, ->a0
  hl.ssrset a0, 0x1f10
  # TTBR1 = &L0_ttbr1
  addtpc L0_ttbr1, ->a0
  addi a0, L0_ttbr1, ->a0
  hl.ssrset a0, 0x1f11
  # TCR = MME=1, T0SZ=16, T1SZ=16
  addi zero, 0x821, ->a0
  hl.ssrset a0, 0x1f12

  # target = HIGH_BASE + phys(high_entry)
  addtpc .Lhigh_base, ->a1
  addi a1, .Lhigh_base, ->a1
  ldi [a1, 0], ->a1
  addtpc high_entry, ->a2
  addi a2, high_entry, ->a2
  add a1, a2, ->a0
  setc.tgt a0
  C.BSTOP

high_entry:
  # PASS: write 0x5555 to the canonical finisher (0x10009000).
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpass_hang:
  C.BSTART DIRECT, .Lpass_hang
  C.BSTOP

.Ltrap_vector:
  # FAIL: write 0x3333 to the canonical finisher (0x10009000).
  C.BSTART
  hl.lui 13107, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Ltrap_hang:
  C.BSTART DIRECT, .Ltrap_hang
  C.BSTOP

.data
.p2align 3
.Lhigh_base:
  .quad 0xffff800000000000

/*
 * TTBR0 page tables: map VA[47]=0 region 0..2MiB identity using an L2 block.
 * This keeps execution working after enabling MME and avoids relying on large
 * block mappings in early bring-up.
 */
.p2align 12
L0_ttbr0:
  .quad L1_ttbr0 + 3
  .zero 8 * (512 - 1)

.p2align 12
L1_ttbr0:
  .quad L2_ttbr0 + 3
  .zero 8 * (512 - 1)

.p2align 12
L2_ttbr0:
  # L2[0] block: base=0, AttrIdx=1, AF=1, U/X/W/R=1, type=Block(10)
  .quad 0xfe
  # L2[128] block: base=0x10000000 (virt MMIO window), AttrIdx=0 (device), AF=1, U/W/R=1
  .zero 8 * 127
  .quad 0x1000006e
  .zero 8 * (512 - 129)

/*
 * TTBR1 page tables: map HIGH_BASE + 0x10000 page to PA 0x10000 so we can jump
 * to a high-half alias of the current code.
 */
.p2align 12
L0_ttbr1:
  .zero 8 * 256
  .quad L1_ttbr1 + 3
  .zero 8 * (512 - 257)

.p2align 12
L1_ttbr1:
  .quad L2_ttbr1 + 3
  .zero 8 * (512 - 1)

.p2align 12
L2_ttbr1:
  .quad L3_ttbr1 + 3
  .zero 8 * (512 - 1)

.p2align 12
L3_ttbr1:
  .zero 8 * 16
  # L3[16] page: base=0x10000, AttrIdx=1, AF=1, U/X/W/R=1, type=Page(01)
  .quad 0x100fd
  .zero 8 * (512 - 17)
