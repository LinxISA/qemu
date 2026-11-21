        # SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
        C.BSTART
        hl.liu 131073, ->a0
        hl.liu 1911, ->a1
scoped_fault:
        .8byte 0x000000013199041f # LDI.private [a0, 0], ->a1
        C.BSTOP
