# SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
        .globl cross_model_result
        .globl cross_model_result_size
        .set cross_model_result, 0x20000
        .set cross_model_result_size, 32

_start:
        C.BSTART
        hl.liu 131072, ->a0
        hl.liu 1, ->a1
        ssrget 0x0802, ->a7
        C.BSTOP
        C.BSTART COND, .Lpe0
        setc.eq a7, zero
        C.BSTOP
        C.BSTART COND, .Lpe1
        setc.eq a7, a1
        C.BSTOP
        C.BSTART COND, .Lpe2
        hl.liu 2, ->a6
        setc.eq a7, a6
        C.BSTOP
        C.BSTART DIRECT, .Lpe3
        C.BSTOP

.Lpe0:
        C.BSTART
        hl.liu 0x2345, ->a2
        swi.intra a2, [a0, 0]
        swi.intra a1, [a0, 4]
        C.BSTOP
.Lwait_pe2:
        C.BSTART COND, .Lwait_pe2
        lwi.intra [a0, 20], ->a2
        setc.eq a2, zero
        C.BSTOP
.Lwait_pe3:
        C.BSTART COND, .Lwait_pe3
        lwi.intra [a0, 28], ->a2
        setc.eq a2, zero
        C.BSTOP
        C.BSTART
        hl.liu 0x5555, ->a2
        hl.liu 268472320, ->t
        swi a2, [t#1, 0]
        C.BSTOP

.Lpe1:
        C.BSTART COND, .Lpe1
        lwi.intra [a0, 4], ->a2
        setc.eq a2, zero
        C.BSTOP
        C.BSTART COND, .Lfail
        lwi.intra [a0, 0], ->a2
        hl.liu 0x2345, ->a3
        setc.ne a2, a3
        C.BSTOP
        C.BSTART
        hl.liu 0x6789, ->a2
        swi.inter a2, [a0, 8]
        swi.inter a1, [a0, 12]
        C.BSTOP
        C.BSTART DIRECT, .Lpark
        C.BSTOP

.Lpe2:
        C.BSTART COND, .Lpe2
        lwi.inter [a0, 12], ->a2
        setc.eq a2, zero
        C.BSTOP
        C.BSTART
        lwi.inter [a0, 8], ->a2
        swi.intra a2, [a0, 16]
        swi.intra a1, [a0, 20]
        C.BSTOP
        C.BSTART DIRECT, .Lpark
        C.BSTOP

.Lpe3:
        C.BSTART
        hl.liu 0x4321, ->a2
        swi.private a2, [a0, 24]
        lwi.private [a0, 24], ->a3
        swi.intra a3, [a0, 28]
        C.BSTOP
.Lpark:
        C.BSTART DIRECT, .Lpark
        C.BSTOP

.Lfail:
        C.BSTART
        hl.liu 0x3333, ->a2
        hl.liu 268472320, ->t
        swi a2, [t#1, 0]
        C.BSTOP
