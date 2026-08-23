.text
.globl _start
_start:
  C.BSTART
  addtpc input_a, ->a0
  addtpc input_b, ->a1
  addtpc result_buffer, ->a2
  addtpc expected_result, ->a4
  c.movi 8, ->a3
  C.BSTOP

  BSTART.TLOAD FP32
  .4byte (22 << 7) | (31 << 20) | 0x1023
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->t<128B>

  BSTART.TLOAD FP32
  .4byte (23 << 7) | (31 << 20) | 0x1023
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOR [a1,a3],[]
  B.IOT mask=0001, last, ->u<128B>

  BSTART.TMATMUL FP32
  .4byte 0x00002023
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOT t#1, u#1, mask=0001, last, ->m<128B>

  BSTART.TSTORE FP32
  .4byte (25 << 7) | (31 << 20) | 0x1023
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOR [a2,a3],[]
  B.IOT m#1, mask=0001, last

  C.BSTART COND, .Lfail
  lwi [a2, 0], ->a5
  lwi [a4, 0], ->a6
  setc.ne a5, a6
  C.BSTOP
  C.BSTART COND, .Lfail
  lwi [a2, 4], ->a5
  lwi [a4, 4], ->a6
  setc.ne a5, a6
  C.BSTOP
  C.BSTART COND, .Lfail
  lwi [a2, 8], ->a5
  lwi [a4, 8], ->a6
  setc.ne a5, a6
  C.BSTOP
  C.BSTART COND, .Lfail
  lwi [a2, 12], ->a5
  lwi [a4, 12], ->a6
  setc.ne a5, a6
  C.BSTOP

  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.Lfail:
  C.BSTART
  hl.liu 13107, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 12
.globl input_a
input_a:
  .float 1.0, 2.0, 3.0, 4.0
.globl input_b
.p2align 12
input_b:
  .float 5.0, 6.0, 7.0, 8.0
.globl result_buffer
.p2align 12
result_buffer:
  .space 16
.p2align 12
expected_result:
  .float 19.0, 22.0, 43.0, 50.0
