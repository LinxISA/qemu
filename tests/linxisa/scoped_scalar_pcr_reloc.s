# SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .p2align 2
.Lbefore:
        .4byte 0, 0x2345
        .globl _start
_start:
        C.BSTART
        lw.pcr.intra [.Lbefore + 4], ->a2
        sw.pcr.inter a2, [.Lstores + 4]
        hl.ld.pcr.private [.Lafter], ->a3
        hl.sd.pcr.intra a3, [.Lstores + 8]
        lw.pcr.inter [.Lstores + 4], ->a4
        hl.ld.pcr.intra [.Lstores + 8], ->a5
        C.BSTOP

        C.BSTART COND, .Lfail
        hl.liu 0x2345, ->a6
        setc.ne a4, a6
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 0x6789, ->a6
        setc.ne a5, a6
        C.BSTOP

        C.BSTART
        hl.liu 0x5555, ->a0
        hl.liu 268472320, ->t
        swi a0, [t#1, 0]
        C.BSTOP
.Lpark:
        C.BSTART DIRECT, .Lpark
        C.BSTOP
.Lfail:
        C.BSTART
        hl.liu 0x3333, ->a0
        hl.liu 268472320, ->t
        swi a0, [t#1, 0]
        C.BSTOP
        C.BSTART DIRECT, .Lpark
        C.BSTOP

        .data
        .p2align 3
.Lafter:
        .8byte 0x6789
.Lstores:
        .8byte 0, 0
