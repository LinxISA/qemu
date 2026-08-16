.macro SET_EXPECT value
  C.BSTART
  addi zero, \value, ->a0
  C.BSTOP
.endm

.macro CHECK_MARKER
  C.BSTART COND, .Lfail
  setc.ne a1, a0
  C.BSTOP
.endm

.text
.globl _start
_start:
  # 32-bit baseline: call=P+simm12*2, ra=P+2+uimm5*2.
  SET_EXPECT 17
.Lcall32_base:
  .4byte 0x50160002 | (((.Lcallee17 - .Lcall32_base) / 2 & 0xfff) << 4) | (((.Lret32_base - (.Lcall32_base + 2)) / 2 & 0x1f) << 22)
  C.BSTOP
  .2byte 0xffff
.Lret32_base:
  CHECK_MARKER

  # Changing only uimm5 must preserve the callee and move RA by two bytes.
  SET_EXPECT 17
.Lcall32_ret_mut:
  .4byte 0x50160002 | (((.Lcallee17 - .Lcall32_ret_mut) / 2 & 0xfff) << 4) | (((.Lret32_mut - (.Lcall32_ret_mut + 2)) / 2 & 0x1f) << 22)
  C.BSTOP
  .2byte 0xffff
  .2byte 0xffff
.Lret32_mut:
  CHECK_MARKER

  # Changing only simm12 must select a different callee without moving RA.
  SET_EXPECT 34
.Lcall32_call_mut:
  .4byte 0x50160002 | (((.Lcallee34 - .Lcall32_call_mut) / 2 & 0xfff) << 4) | (((.Lret32_call_mut - (.Lcall32_call_mut + 2)) / 2 & 0x1f) << 22)
  C.BSTOP
  .2byte 0xffff
.Lret32_call_mut:
  CHECK_MARKER

  # 48-bit baseline: call=P+simm25*2, ra=P+4+uimm5*2.
  SET_EXPECT 51
.Lcall48_base:
  .4byte 0x00000011 | (((.Lcallee51 - .Lcall48_base) / 2 & 0x1ffffff) << 7)
  .2byte 0x5016 | (((.Lret48_base - (.Lcall48_base + 4)) / 2 & 0x1f) << 6)
  C.BSTOP
  .2byte 0xffff
.Lret48_base:
  CHECK_MARKER

  # Changing only the embedded uimm5 moves RA but not the call target.
  SET_EXPECT 51
.Lcall48_ret_mut:
  .4byte 0x00000011 | (((.Lcallee51 - .Lcall48_ret_mut) / 2 & 0x1ffffff) << 7)
  .2byte 0x5016 | (((.Lret48_mut - (.Lcall48_ret_mut + 4)) / 2 & 0x1f) << 6)
  C.BSTOP
  .2byte 0xffff
  .2byte 0xffff
.Lret48_mut:
  CHECK_MARKER

  # Changing only simm25 selects the second 48-bit callee.
  SET_EXPECT 68
.Lcall48_call_mut:
  .4byte 0x00000011 | (((.Lcallee68 - .Lcall48_call_mut) / 2 & 0x1ffffff) << 7)
  .2byte 0x5016 | (((.Lret48_call_mut - (.Lcall48_call_mut + 4)) / 2 & 0x1f) << 6)
  C.BSTOP
  .2byte 0xffff
.Lret48_call_mut:
  CHECK_MARKER

  # Continue with exact forms whose bytes straddle an instruction page.
  SET_EXPECT 85
  C.BSTART DIRECT, .Lcall48_page_m4
  C.BSTOP

.Lpass:
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lcallee17:
  C.BSTART.STD RET
  addi zero, 17, ->a1
  c.setc.tgt ra
  C.BSTOP

.Lcallee34:
  C.BSTART.STD RET
  addi zero, 34, ->a1
  c.setc.tgt ra
  C.BSTOP

.Lcallee51:
  C.BSTART.STD RET
  addi zero, 51, ->a1
  c.setc.tgt ra
  C.BSTOP

.Lcallee68:
  C.BSTART.STD RET
  addi zero, 68, ->a1
  c.setc.tgt ra
  C.BSTOP

.Lfail:
  C.BSTART
  hl.lui 13107, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

  # P=page_end-4: the low 32 bits end on the first page and uimm5 lives on
  # the adjacent page. Returning to P+10 proves the extension was included.
  .p2align 12
  .space 4092
.Lcall48_page_m4:
  .4byte 0x00000011 | (((.Lcallee85 - .Lcall48_page_m4) / 2 & 0x1ffffff) << 7)
  .2byte 0x5016 | (((.Lret48_page_m4 - (.Lcall48_page_m4 + 4)) / 2 & 0x1f) << 6)
  C.BSTOP
  .2byte 0xffff
.Lret48_page_m4:
  CHECK_MARKER
  SET_EXPECT 102
  C.BSTART DIRECT, .Lcall48_page_m2
  C.BSTOP
.Lcallee85:
  C.BSTART.STD RET
  addi zero, 85, ->a1
  c.setc.tgt ra
  C.BSTOP

  # P=page_end-2 is also legal because Linx instructions are 2-byte aligned.
  # Here both the high half of simm25 and the return field cross the boundary.
  .p2align 12
  .space 4094
.Lcall48_page_m2:
  .4byte 0x00000011 | (((.Lcallee102 - .Lcall48_page_m2) / 2 & 0x1ffffff) << 7)
  .2byte 0x5016 | (((.Lret48_page_m2 - (.Lcall48_page_m2 + 4)) / 2 & 0x1f) << 6)
  C.BSTOP
  .2byte 0xffff
.Lret48_page_m2:
  CHECK_MARKER

  # The exact 0.58.1 fused ICALL snapshots the retiring FALL block's BPCN.
  # A later SETC.TGT deliberately installs a poison target; both the normal
  # path and the page-boundary TB split must still call .Lcallee119.
  C.BSTART DIRECT, fused_icall_setup
  C.BSTOP

  # Put the four-byte fused ICALL at the end of a page. Its body starts in a
  # different TB, making the durable snapshot requirement executable without
  # relying on a debug accelerator mode that splits every architectural block.
  .p2align 12
.Lfused_page:
  .space 4070
.globl fused_icall_setup
fused_icall_setup:
  SET_EXPECT 119
  C.BSTART
  addtpc %tpcrel_hi(.Lcallee119), ->a2
  addi a2, %tpcrel_lo(.Lcallee119), ->a2
  c.setc.tgt a2
  C.BSTOP
.globl fused_icall_positive
.globl fused_icall_cross_tb
fused_icall_positive:
fused_icall_cross_tb:
  .4byte 0x50166001 | (((.Lfused_icall_return - (fused_icall_positive + 2)) / 2 & 0x1f) << 22)
.if ((fused_icall_positive - .Lfused_page + 4) & 4095) != 0
  .error "fused ICALL must end at a page boundary"
.endif
  addtpc %tpcrel_hi(fused_icall_poison), ->a2
  addi a2, %tpcrel_lo(fused_icall_poison), ->a2
  c.setc.tgt a2
  C.BSTOP
.Lfused_icall_return:
  CHECK_MARKER
  C.BSTART DIRECT, .Lpass
  C.BSTOP
.Lcallee102:
  C.BSTART.STD RET
  addi zero, 102, ->a1
  c.setc.tgt ra
  C.BSTOP

fused_icall_poison:
  C.BSTART.STD RET
  addi zero, 1, ->a1
  c.setc.tgt ra
  C.BSTOP

.Lcallee119:
  C.BSTART.STD RET
  addi zero, 119, ->a1
  c.setc.tgt ra
  C.BSTOP
