.text
.globl _start
_start:
  C.BSTART

  # Minimal vector block smoke: emit MPAR then MSEQ decoupled headers.
  # Trace mapping classifies in-body MPAR/MSEQ commits as vpar/vseq.
  BSTART.MPAR 0
  B.TEXT __linx_vector_empty_body
  C.BSTART

  BSTART.MSEQ 0
  B.TEXT __linx_vector_empty_body
  C.BSTART

  # canonical finisher PASS value 0x5555 (virt exit MMIO at 0x10009000).
  hl.liu 21845, ->a0
  hl.liu 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 2
__linx_vector_empty_body:
  C.BSTOP
