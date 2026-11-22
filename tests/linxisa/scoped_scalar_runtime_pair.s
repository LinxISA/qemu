# SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
.if FAULT
        C.BSTART
        addtpc .Ltrap, ->a0
        addi a0, .Ltrap, ->a0
        ssrset a0, 0x0f01
        C.BSTOP
        C.BSTART
        hl.liu 16777208, ->a0
        hl.liu 0x1122, ->a1
        hl.liu 0x3344, ->a2
        hl.liu 0x5566, ->a3
        hl.liu 0x7788, ->a4
        addi a0, 8, ->a6
        sdi.intra a3, [a0, 0]
.if FAULT == 1
        .8byte 0x0021b059011e091f # hl.sdip.intra a1,a2,[a0,0]
.else
        .8byte 0x00013299301e091f # hl.ldip.intra [a0,0], ->a3,a4
.endif
        C.BSTOP
        C.BSTART DIRECT, .Lfail
        C.BSTOP

.Ltrap:
        C.BSTART COND, .Lfail
        ssrget 0x0f03, ->a5
        setc.ne a5, a6
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 0x5566, ->a5
        setc.ne a3, a5
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 0x7788, ->a5
        setc.ne a4, a5
        C.BSTOP
.else
        C.BSTART
        hl.liu 131072, ->a0
        hl.liu 0x1122, ->a1
        hl.liu 0x3344, ->a2
        .8byte 0x0021b059011e091f # hl.sdip.intra a1,a2,[a0,0]
        .8byte 0x00013299301e091f # hl.ldip.intra [a0,0], ->a3,a4
        C.BSTOP
        C.BSTART COND, .Lfail
        setc.ne a3, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        setc.ne a4, a2
        C.BSTOP

        C.BSTART
        hl.liu 131104, ->a0
        hl.liu 2, ->a5
        .8byte 0x18713049011e0a1f # hl.sdp.inter a1,a2,[a0,a5]
        .8byte 0x18713289301e0a1f # hl.ldp.inter [a0,a5<<3], ->a3,a4
        C.BSTOP
        C.BSTART COND, .Lfail
        setc.ne a3, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        setc.ne a4, a2
        C.BSTOP
.endif

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
