        # SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
        C.BSTART
        ssrget 80, ->a0
        ssrget 81, ->a1
        C.BSTOP

        C.BSTART COND, .Lfail
        setc.ne a0, zero
        C.BSTOP
        C.BSTART COND, .Lfail
        setc.ne a1, zero
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
