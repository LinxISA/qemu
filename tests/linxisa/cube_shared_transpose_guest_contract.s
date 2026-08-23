.text
.globl _start
_start:
  C.BSTART
  addtpc shared_a, ->a0
  addtpc shared_b, ->a1
  addtpc result_buffer, ->a2
  addi zero, 32, ->a3
  C.BSTOP

  BSTART.TLOAD FP32
  C.B.DIMI 8, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 8, ->lb2
  B.IOR [a0,a3],[]
  B.IOT mask=1111, last, ->t<128B>
  BSTART.TMOV.L2S.PUBLISH FP32
  B.IOT t#1, mask=1111, last
  B.IOS mask=1111, ->S1<128B>

  BSTART.TLOAD FP32
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOR [a1,a3],[]
  B.IOT mask=1111, last, ->u<128B>
  BSTART.TMOV.L2S.PUBLISH FP32
  B.IOT u#1, mask=1111, last
  B.IOS mask=1111, ->S2<128B>

  BSTART.TMATMUL FP32
  .4byte 0x000021a3
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOS S1, mask=1111
  B.IOS S2, mask=1111
  B.IOT mask=1111, last, ->m<128B>

  C.BSTART
  addi zero, 8, ->a3
  C.BSTOP
  BSTART.TSTORE FP32
  .4byte (25 << 7) | (31 << 20) | 0x1023
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOR [a2,a3],[]
  B.IOT m#1, mask=1111, last

  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 12
shared_a:
  .float 1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0
  .float 2.0, 4.0, 2.0, 4.0, 2.0, 4.0, 2.0, 4.0
.p2align 12
shared_b:
  .float 5.0, 7.0, 6.0, 8.0
.p2align 12
result_buffer:
  .space 16
