# SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
        C.BSTART.SYS
        hl.liu 1, ->a0
        ssrset a0, 0x84b
        .4byte 0x1117000e # raw upper-word materialization of 1 into a0
        .2byte 0
        hl.liu 65537, ->a1
        or a0, a1, ->a0
        ssrset a0, 0x84a
        hl.liu 4660, ->a0
        ssrset a0, 0x831
        .4byte 0x1117000e # raw upper-word materialization of 1 into a0
        .2byte 0
        bse a0
        ssrget 0x847, ->a1
        hl.liu 257, ->a2
        setc.ne a1, a2
        C.BSTOP
        C.BSTART COND, .Lfail
        C.BSTOP

        C.BSTART.SYS
        hl.liu 1, ->a0
        ssrset a0, 0x83b
        ssrget 0x831, ->a1
        hl.liu 4660, ->a2
        setc.ne a1, a2
        C.BSTOP
        C.BSTART COND, .Lfail
        C.BSTOP

        C.BSTART.SYS
        ssrget 0x83d, ->a0
        ssrset a0, 0x842
        ssrget 0x843, ->a1
        hl.liu 257, ->a2
        setc.ne a1, a2
        C.BSTOP
        C.BSTART COND, .Lfail
        C.BSTOP

        C.BSTART.SYS
        ssrset a0, 0x841
        bwe a0
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
