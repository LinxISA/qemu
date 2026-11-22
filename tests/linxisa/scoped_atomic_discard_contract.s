.text
.globl _start
.globl cross_model_result
.globl cross_model_result_size
.set cross_model_result, 0x20000
.set cross_model_result_size, 48

_start:
  C.BSTART
  hl.liu 0x20100, ->a0
  hl.liu 0x20000, ->a4
  hl.liu 7, ->a1
  swi a1, [a0, 0]

  # Establish queue sentinels, then target every discard destination 24..29.
  hl.liu 0x4444, ->t
  hl.liu 0x3333, ->t
  hl.liu 0x2222, ->t
  hl.liu 0x1111, ->t
  hl.liu 0x6666, ->u
  hl.liu 0x5555, ->u
  .8byte 0x000010010c0b041f
  .8byte 0x000010010c8b041f
  .8byte 0x000010010d0b041f
  .8byte 0x000010010d8b041f
  .8byte 0x000010010e0b041f
  .8byte 0x000010010e8b041f

  sdi t#1, [a4, 0]
  sdi t#2, [a4, 8]
  sdi t#3, [a4, 16]
  sdi t#4, [a4, 24]
  sdi u#1, [a4, 32]
  sdi u#2, [a4, 40]
  C.BSTOP

  C.BSTART
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
