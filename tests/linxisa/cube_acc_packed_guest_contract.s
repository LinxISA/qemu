.text
.globl _start
_start:
  C.BSTART
  addtpc signed_c, ->a0
  addtpc signed_a, ->a1
  addtpc signed_b, ->a2
  addtpc signed_out, ->a4
  c.movi 16, ->a3
  C.BSTOP

  BSTART.TLOAD S32
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 4, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->m<256B>
  .4byte 0x98011181  # BSTART.TLOAD S8
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a1,a3],[]
  B.IOT mask=0001, last, ->t<128B>
  .4byte 0x98011181  # BSTART.TLOAD S8
  B.DATR ND2N8, DTYPE_NONE, Null
  C.B.DIMI 4, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a2,a3],[]
  B.IOT mask=0001, last, ->u<128B>
  .4byte 0x98231181  # BSTART.TMATMUL.ACC S8
  B.FPATR 0, 0, 0, 0, 0, 0, 0, 0, 0
  C.B.DIMI 2, ->lb0
  C.B.DIMI 4, ->lb1
  C.B.DIMI 1, ->lb2
  B.IOT m#1, t#1, mask=0001
  B.IOT u#1, mask=0001, last, ->m<256B>
  BSTART.TSTORE S32
  B.DATR M162ND, DTYPE_NONE, Null
  C.B.DIMI 4, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a4,a3],[]
  B.IOT m#1, mask=0001, last

  C.BSTART
  addtpc unsigned_c, ->a0
  addtpc unsigned_a, ->a1
  addtpc unsigned_b, ->a2
  addtpc unsigned_out, ->a4
  C.BSTOP
  BSTART.TLOAD U32
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 4, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->m<256B>
  .4byte 0xd8011181  # BSTART.TLOAD U8
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 1, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a1,a3],[]
  B.IOT mask=0001, last, ->t<128B>
  .4byte 0xd8011181  # BSTART.TLOAD U8
  B.DATR ND2N8, DTYPE_NONE, Null
  C.B.DIMI 4, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a2,a3],[]
  B.IOT mask=0001, last, ->u<128B>
  .4byte 0xd8231181  # BSTART.TMATMUL.ACC U8
  B.FPATR 0, 0, 0, 0, 0, 0, 0, 0, 0
  C.B.DIMI 2, ->lb0
  C.B.DIMI 4, ->lb1
  C.B.DIMI 1, ->lb2
  B.IOT m#1, t#1, mask=0001
  B.IOT u#1, mask=0001, last, ->m<256B>
  BSTART.TSTORE U32
  B.DATR M162ND, DTYPE_NONE, Null
  C.B.DIMI 4, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a4,a3],[]
  B.IOT m#1, mask=0001, last

  C.BSTART
  addtpc packed_a, ->a0
  addtpc packed_b, ->a1
  addtpc packed_out, ->a2
  c.movi 1, ->a3
  C.BSTOP
  BSTART.TLOAD S4X2
  B.DATR ND2M16, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a0,a3],[]
  B.IOT mask=0001, last, ->t<128B>
  BSTART.TLOAD S4X2
  B.DATR ND2N8, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 2, ->lb1
  B.IOR [a1,a3],[]
  B.IOT mask=0001, last, ->u<128B>
  BSTART.TMATMUL S4X2
  B.FPATR 0, 0, 0, 0, 0, 0, 0, 0, 0
  C.B.DIMI 1, ->lb0
  C.B.DIMI 2, ->lb1
  C.B.DIMI 2, ->lb2
  B.IOT t#1, u#1, mask=0001, last, ->m<128B>
  BSTART.TSTORE S32
  B.DATR M162ND, DTYPE_NONE, Null
  C.B.DIMI 2, ->lb0
  C.B.DIMI 1, ->lb1
  B.IOR [a2,a3],[]
  B.IOT m#1, mask=0001, last

  C.BSTART
  addtpc signed_out, ->a0
  addtpc signed_expected, ->a1
  addtpc unsigned_out, ->a2
  addtpc unsigned_expected, ->a4
  addtpc packed_out, ->a5
  addtpc packed_expected, ->a6
  C.BSTOP
  .set offset, 0
  .rept 8
    C.BSTART COND, .Lfail
    lwi [a0, offset], ->t
    lwi [a1, offset], ->u
    setc.ne t#1, u#1
    C.BSTOP
    C.BSTART COND, .Lfail
    lwi [a2, offset], ->t
    lwi [a4, offset], ->u
    setc.ne t#1, u#1
    C.BSTOP
    .set offset, offset + 4
  .endr
  C.BSTART COND, .Lfail
  lwi [a5, 0], ->t
  lwi [a6, 0], ->u
  setc.ne t#1, u#1
  C.BSTOP
  C.BSTART COND, .Lfail
  lwi [a5, 4], ->t
  lwi [a6, 4], ->u
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

.text
.balign 64, 0
signed_c: .long -8, -7, -6, -5, -4, -3, -2, -1
signed_a: .byte 1
  .space 15
  .byte 1
signed_b: .byte 1, 1, 1, 1
signed_expected: .long -7, -6, -5, -4, -3, -2, -1, 0
unsigned_c: .long 8, 7, 6, 5, 4, 3, 2, 1
unsigned_a: .byte 1
  .space 15
  .byte 1
unsigned_b: .byte 1, 1, 1, 1
unsigned_expected: .long 9, 8, 7, 6, 5, 4, 3, 2
packed_a: .byte 0x11
packed_b: .byte 0x21, 0x43
packed_expected: .long 4, 6
.balign 64, 0
signed_out: .space 32
.balign 64, 0
unsigned_out: .space 32
.balign 64, 0
packed_out: .space 8
