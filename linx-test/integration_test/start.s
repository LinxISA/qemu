.macro block_qemu_debug_stop
  .align 16
	.word 0xB, 0x201, 0, 0
	/* no ret */
.endm

.macro block_qemu_debug_reg
  .align 16
	.word 0xB, 0x202, 0, 0
	/* no ret */
.endm


  .file	"start.s"
  .option nopic
  .globl  _start
  .type   _start,@function
  .text
_start:
  bstart _start.bstart
  bnext.call main
  b.std
  bget sp
  bset a0, a1, a2, ra
  bstop _start.bstop
  .section .text.body
_start.bstart:
  get     sp
  lw      [t#1, 0]
  set     a0, t#1
  const   8
  add     t#4, t#1
  set     a1, t#1
  const   0
  set     a2, t#1
_start.bstop:
  .text
.Ltmp0:
  bstart .Ltmp0.bstart
  bnext.ind
  b.std
  bset ra
  bstop .Ltmp0.bstop
  .section .text.body
.Ltmp0.bstart:
  lconst   0xffffffffffffffff
  set     ra, t#1
  setc.tgt  t#2
.Ltmp0.bstop:

/*
 * void qemu_debug_str(int id, const char *str);
 * print null-terminating @str with a simple @id by qemu
 */
  .globl  qemu_debug_str
  .type   qemu_debug_str,@function
  .text
qemu_debug_str:
	.word 0xB, 0x210, 0, 0
	.word 0x00018082, 0x00010001, 0x00010001, 0x00010001
