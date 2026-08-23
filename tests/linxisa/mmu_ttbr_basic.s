.text
.globl _start
.ifndef FAULT_KIND
.set FAULT_KIND, -1
.endif

_start:
  # Install a minimal trap vector for ACR0 so unexpected MMU faults exit.
  C.BSTART
  addtpc .Ltrap_vector, ->a0
  addi a0, .Ltrap_vector, ->a0
  ssrset a0, 0x0f01
  hl.ssrset a0, 0x1f01
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
  # Enter the TTBR1 high alias explicitly as ACR2.  A mapped-user TLSU
  # regression must exercise CPU MMU translation under the user ring, not only
  # the same virtual address while remaining privileged in ACR0.
  addi zero, 2, ->a1
  ssrset a1, 0x0f00
  ssrset a0, 0x0f41
  ssrset a0, 0x0f43
  acre 0
  C.BSTOP

high_entry:
  # Fail closed unless ACRE actually installed ACR2 in CSTATE.ACR.
  C.BSTART COND, .Ltrap_vector
  ssrget 0x0020, ->a4
  xori a4, 2, ->a4
  setc.ne a4, zero
  C.BSTOP

  # Exercise ordinary TLSU through the active CPU MMU.  Both buffers live in
  # the high alias page mapped by TTBR1; an IOMMU/physical-mask shortcut would
  # access an unrelated physical address and fail this result check.
  C.BSTART
  addtpc tile_input, ->a0
  addi a0, tile_input, ->a0
  addtpc tile_output, ->a1
  addi a1, tile_output, ->a1
  addi zero, 16, ->a3
  C.BSTOP

  BSTART.TLOAD FP32
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 4, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 4, ->lb2
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->t<128B>

  BSTART.TSTORE FP32
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 4, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 4, ->lb2
  B.IOR [a1,a3],[]
  B.IOT t#1, mask=0001, last

  C.BSTART COND, .Ltrap_vector
  lwi [a0, 0], ->a2
  lwi [a1, 0], ->a3
  setc.ne a2, a3
  C.BSTOP

.if FAULT_KIND == 0
  # An unmapped TLOAD must report its original ACR2 VA and must not publish an
  # output tile before the fault is handled.
  C.BSTART
  addtpc .Lfault_load_va, ->a0
  addi a0, .Lfault_load_va, ->a0
  ldi [a0, 0], ->a0
  addi zero, 16, ->a3
  C.BSTOP

  BSTART.TLOAD FP32
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 4, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 4, ->lb2
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->t<128B>

.elseif FAULT_KIND == 1
  # Re-create a live source tile, then fault TSTORE.  The trap snapshot must
  # retain that source rather than consuming it before any store beat succeeds.
  C.BSTART
  addtpc tile_input, ->a0
  addi a0, tile_input, ->a0
  addtpc .Lfault_store_va, ->a1
  addi a1, .Lfault_store_va, ->a1
  ldi [a1, 0], ->a1
  addi zero, 16, ->a3
  C.BSTOP

  BSTART.TLOAD FP32
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 4, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 4, ->lb2
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->t<128B>

  BSTART.TSTORE FP32
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 4, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 4, ->lb2
  B.IOR [a1,a3],[]
  B.IOT t#1, mask=0001, last
.endif

.if FAULT_KIND >= 0
  # Reaching the following block means the unmapped access did not fault.
  C.BSTART DIRECT, .Ltrap_fail
  C.BSTOP
.endif

  # PASS: write 0x5555 to the canonical finisher (0x10009000).
  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lpass_hang:
  C.BSTART DIRECT, .Lpass_hang
  C.BSTOP

.Ltrap_vector:
.if FAULT_KIND == 0
  C.BSTART COND, .Ltrap_fail
  hl.ssrget 0x1f02, ->a0
  addi zero, 3, ->a1
  slli a1, 62, ->a1
  addi zero, 2, ->a2
  slli a2, 24, ->a2
  or a1, a2, ->a1
  ori a1, 1, ->a1
  xor a0, a1, ->a0
  hl.ssrget 0x1f03, ->a2
  addtpc .Lfault_load_va, ->a3
  addi a3, .Lfault_load_va, ->a3
  ldi [a3, 0], ->a3
  xor a2, a3, ->a2
  or a0, a2, ->a0
  # EBARG_TQ0 remains empty until TLOAD publishes a complete output.
  hl.ssrget 0x1f45, ->a4
  or a0, a4, ->a0
  setc.ne a0, zero
  C.BSTOP
.elseif FAULT_KIND == 1
  C.BSTART COND, .Ltrap_fail
  hl.ssrget 0x1f02, ->a0
  addi zero, 3, ->a1
  slli a1, 62, ->a1
  addi zero, 5, ->a2
  slli a2, 24, ->a2
  or a1, a2, ->a1
  ori a1, 1, ->a1
  xor a0, a1, ->a0
  hl.ssrget 0x1f03, ->a2
  addtpc .Lfault_store_va, ->a3
  addi a3, .Lfault_store_va, ->a3
  ldi [a3, 0], ->a3
  xor a2, a3, ->a2
  or a0, a2, ->a0
  setc.ne a0, zero
  C.BSTOP

.else
  C.BSTART DIRECT, .Ltrap_fail
  C.BSTOP
.endif

  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Ltrap_fail:
  # FAIL: write 0x3333 to the canonical finisher (0x10009000).
  C.BSTART
  hl.liu 13107, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Ltrap_hang:
  C.BSTART DIRECT, .Ltrap_hang
  C.BSTOP

.p2align 4
tile_input:
  .4byte 0x3f800000, 0x40000000, 0x40400000, 0x40800000
tile_output:
  .zero 16
.Lfault_load_va:
  .quad 0xffff800000211000
.Lfault_store_va:
  .quad 0xffff800000212000

.data
.p2align 3
.Lhigh_base:
  # Keep bits[28:0] different from the mapped PA.  This makes the regression
  # fail on the retired physical-mask shortcut (VA & 0x1fffffff) while the CPU
  # page-table walk resolves the alias to PA 0x10000.
  .quad 0xffff800000200000

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
  .zero 8
  .quad L3_ttbr1 + 3
  .zero 8 * (512 - 2)

.p2align 12
L3_ttbr1:
  .zero 8 * 16
  # L3[16] page: base=0x10000, AttrIdx=1, AF=1, U/X/W/R=1, type=Page(01)
  .quad 0x100fd
  .zero 8 * (512 - 17)
