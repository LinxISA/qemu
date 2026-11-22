# SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start, hscb_worker_entry
_start:
        C.BSTART.SYS
        hl.liu 1, ->a0
        ssrset a0, 0x84b
        .4byte 0x1117000e # raw upper-word materialization of 1 into a0
        .2byte 0
        hl.liu 65537, ->a1
        or a0, a1, ->a0
        ssrset a0, 0x84a
        hl.liu 1, ->a0
        ssrset a0, 0x831
        .4byte 0x1117000e # raw upper-word materialization of 1 into a0
        .2byte 0
        hl.liu 4294967295, ->a1
        or a0, a1, ->a0
        bse a0
        C.BSTOP
        C.BSTART.SYS
        ssrget 80, ->a2
        bwe zero
        C.BSTOP
        C.BSTART DIRECT, .Lidle
        C.BSTOP

hscb_worker_entry:
        C.BSTART.SYS
        hl.liu 305419896, ->a1
        setc.ne a0, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        C.BSTOP
        C.BSTART.SYS
        ssrget 80, ->a2
        ssrget 81, ->a3
        hl.liu 8, ->a4
        setc.ne a2, a4
        C.BSTOP
        C.BSTART COND, .Lfail
        C.BSTOP
        C.BSTART.SYS
        hl.liu 3, ->a4
        setc.ne a3, a4
        C.BSTOP
        C.BSTART COND, .Lfail
        C.BSTOP

        C.BSTART.SYS
        hl.liu 3, ->a0
        ssrset a0, 0x831
        hl.liu 77, ->a0
        ssrset a0, 0x832
        hl.liu 24576, ->a0
        ssrset a0, 0x833
        .4byte 0x3117000e # raw upper-word materialization of 3 into a0
        .2byte 0
        hl.liu 4294967295, ->a1
        or a0, a1, ->a0
        bse a0
        ssrget 80, ->a2
        ssrget 81, ->a3
        or a2, a3, ->a2
        setc.ne a2, zero
        C.BSTOP
        C.BSTART COND, .Lfail
        C.BSTOP
        C.BSTART.SYS
        ssrget 0x84a, ->a4
        setc.ne a4, zero
        C.BSTOP
        C.BSTART COND, .Lfail
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
.Lidle:
        C.BSTART DIRECT, .Lidle
        C.BSTOP
