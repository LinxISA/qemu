.text
.globl _start
_start:
.if CASE == 0
.globl fpatr_position_non_cube
fpatr_position_non_cube:
  .4byte 0x00011181
  .4byte 0x00002023
.elseif CASE == 1
.globl fpatr_position_duplicate
fpatr_position_duplicate:
  .4byte 0x00031181
  .4byte 0x00002023
  .4byte 0x00002023
.elseif CASE == 2
  C.BSTART DIRECT, .Lfpatr_post_ior
  C.BSTOP
  .p2align 12
.Lfpatr_post_ior_page:
  .space 4088
.globl fpatr_position_post_ior
fpatr_position_post_ior:
.Lfpatr_post_ior:
  .4byte 0x00031181
  .4byte 0x00000013
.globl fpatr_position_cross_tb
fpatr_position_cross_tb:
.if ((fpatr_position_cross_tb - .Lfpatr_post_ior_page) & 4095) != 0
  .error "post-IOR B.FPATR must start at a page boundary"
.endif
  .4byte 0x00002023
.elseif CASE == 3
.globl fpatr_position_post_iot
fpatr_position_post_iot:
  .4byte 0x00031181
  .4byte 0x0000c013
  .4byte 0x00002023
.else
  .error "unknown B.FPATR placement case"
.endif

  # Every case must trap before reaching this observable side effect.
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
