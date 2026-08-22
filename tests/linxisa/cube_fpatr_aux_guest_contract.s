.text
.globl _start
_start:
  C.BSTART
  addtpc prelu_a, ->a0
  addtpc prelu_b, ->a1
  addtpc quant_vector, ->a2
  addtpc prelu_vector, ->a4
  addtpc prelu_out, ->a5
  c.movi 8, ->a3
  C.BSTOP
  BSTART.TLOAD FP32
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->t<128B>
  BSTART.TLOAD FP32
  B.DATR ND2N8, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a1,a3],[]
  B.IOT mask=0001, last, ->u<128B>
  BSTART.TLOAD U64
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a2,a3],[]
  B.IOT mask=0001, last, ->m<128B>
  BSTART.TLOAD U64
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a4,a3],[]
  B.IOT mask=0001, last, ->t<128B>
  BSTART.TMATMUL FP32
  B.FPATR 33, 3, 0, 0, 0, 0, 0, 0, 0
  C.B.DIMI 1, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOT t#2, u#1, mask=0001
  B.IOT m#1, t#1, mask=0001, last, ->m<128B>
  BSTART.TSTORE FP16
  B.DATR M162ND, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a5,a3],[]
  B.IOT m#1, mask=0001, last

  C.BSTART
  addtpc reduce_a, ->a0
  addtpc reduce_b, ->a1
  addtpc rowmax_in, ->a2
  addtpc reduce_out, ->a4
  addtpc rowmax_out, ->a5
  C.BSTOP
  BSTART.TLOAD FP32
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->t<128B>
  BSTART.TLOAD FP32
  B.DATR ND2N8, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a1,a3],[]
  B.IOT mask=0001, last, ->u<128B>
  BSTART.TLOAD FP32
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a2,a3],[]
  B.IOT mask=0001, last, ->m<128B>
  BSTART.TMATMUL FP32
  B.FPATR 0, 0, 0, 1, 0, 1, 1, 0, 0
  C.B.DIMI 1, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOT t#1, u#1, mask=0001
  B.IOT m#1, mask=0001, ->m<128B>
  B.IOT mask=0001, last, ->t<128B>
  BSTART.TSTORE FP32
  B.DATR M162ND, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a4,a3],[]
  B.IOT m#1, mask=0001, last
  BSTART.TSTORE FP32
  B.DATR NORM, DTYPE_NONE, Zero
  C.B.DIMI 1, ->lb0
  C.B.DIMI 1, ->lb1
  C.B.DIMI 1, ->lb2
  B.IOR [a5,a3],[]
  B.IOT t#1, mask=0001, last

  C.BSTART COND, .Lfail
  addtpc prelu_out, ->a0
  lwi [a0, 0], ->t
  hl.lui 3087023104, ->u
  setc.ne t#1, u#1
  C.BSTOP
  C.BSTART COND, .Lfail
  addtpc rowmax_out, ->a0
  lwi [a0, 0], ->t
  hl.lui 1094713344, ->u
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
prelu_a: .float 1.0, -1.0
.p2align 12
prelu_b: .float 1.0, 0.0, 0.0, 1.0
.p2align 12
quant_vector: .quad 0x000000003f800000, 0x000000003f800000
.p2align 12
prelu_vector: .quad 0x000000000001f800, 0x000000000001f800
.p2align 12
reduce_a: .float 1.0, 1.0
.p2align 12
reduce_b: .float -5.0, 1.0, -5.0, 2.0
.p2align 12
rowmax_in: .float -12.0
.p2align 12
prelu_out: .space 4
.p2align 12
reduce_out: .space 8
.p2align 12
rowmax_out: .space 4
