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

  # Exit code = 0 (virt exit MMIO at 0x10000004).
  addi zero, 0, ->a0
  hl.lui 268435460, ->t
  swi a0, [t#1, 0]
  C.BSTOP

.p2align 2
__linx_vector_empty_body:
  C.BSTOP
