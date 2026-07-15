.text
.globl _start
_start:
  C.BSTART
  # Privileged/system smoke via SSR write path, without MMU page-walk
  # dependencies.
  addi zero, 291, ->a0
  ssrset a0, 0x0f01
  hl.lui 21845, ->a0
  hl.lui 268472320, ->t
  swi a0, [t#1, 0]
  C.BSTOP
