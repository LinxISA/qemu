.text
.globl _start
_start:
  C.BSTART
  addtpc mx_a_fp16, ->a0
  addtpc mx_b_e4m3, ->a1
  addtpc mx_scale_b, ->a2
  addtpc mx_out0, ->a4
  C.BSTOP
  BSTART.TLOAD FP16
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a0,zero],[]
  B.IOT mask=0001, last, ->t<128B>
  BSTART.TLOAD E4M3
  B.DATR ND2N8, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a1,zero],[]
  B.IOT mask=0001, last, ->u<128B>
  BSTART.TLOAD E8M0
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a2,zero],[]
  B.IOT mask=0001, last, ->m<128B>
  BSTART.TMATMULMX FP16
  B.DATR NORM, E4M3, Zero
  B.FPATR 0, 0, 0, 0, 0, 0, 0, 0, 0
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 1, ->lb2
  B.IOT t#1, u#1, mask=0001
  B.IOT m#1, mask=0001, last, ->m<128B>
  BSTART.TSTORE FP32
  B.DATR M162ND, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a4,zero],[]
  B.IOT m#1, mask=0001, last

  C.BSTART
  addtpc mx_a_e2m1, ->a0
  addtpc mx_scale_a, ->a1
  addtpc mx_b_bf16, ->a2
  addtpc mx_out1, ->a4
  C.BSTOP
  BSTART.TLOAD E2M1X2
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a0,zero],[]
  B.IOT mask=0001, last, ->t<128B>
  BSTART.TLOAD E8M0
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a1,zero],[]
  B.IOT mask=0001, last, ->m<128B>
  BSTART.TLOAD BF16
  B.DATR ND2N8, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a2,zero],[]
  B.IOT mask=0001, last, ->u<128B>
  BSTART.TMATMULMX E2M1X2
  B.DATR NORM, BF16, Zero
  B.FPATR 0, 0, 0, 0, 0, 0, 0, 0, 0
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 1, ->lb2
  B.IOT t#1, m#1, mask=0001
  B.IOT u#1, mask=0001, last, ->m<128B>
  BSTART.TSTORE FP32
  B.DATR M162ND, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a4,zero],[]
  B.IOT m#1, mask=0001, last

  C.BSTART COND, .Lfail
  addtpc mx_out0, ->a0
  lwi [a0, 0], ->t
  hl.lui 1065353216, ->u
  setc.ne t#1, u#1
  C.BSTOP
  C.BSTART COND, .Lfail
  addtpc mx_out1, ->a0
  lwi [a0, 0], ->t
  hl.lui 1065353216, ->u
  setc.ne t#1, u#1
  C.BSTOP
  C.BSTART
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
.Lfail:
  C.BSTART
  hl.lui 13107, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 12
mx_a_fp16: .short 0x3c00
.p2align 12
mx_b_e4m3: .byte 0x38, 0
.p2align 12
mx_scale_b: .byte 0x7f, 0
.p2align 12
mx_a_e2m1: .byte 0x02, 0
.p2align 12
mx_scale_a: .byte 0x7f, 0
.p2align 12
mx_b_bf16: .short 0x3f80
.p2align 12
mx_out0: .space 4
.p2align 12
mx_out1: .space 4
