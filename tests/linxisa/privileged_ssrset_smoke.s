.text
.globl _start
_start:
  C.BSTART
  # Privileged/system smoke via SSR write path, without MMU page-walk
  # dependencies.
  addi zero, 291, ->a0
  ssrset a0, 0x0f01
  addi zero, 0, ->a0
  hl.lui 268435460, ->t
  swi a0, [t#1, 0]
  C.BSTOP
