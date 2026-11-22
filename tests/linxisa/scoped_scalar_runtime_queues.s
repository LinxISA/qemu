# SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .macro transfer scope, offset, result
        hl.liu 0x80000005, ->t
        c.swi.\scope t#1, [u#1, \offset]
        c.lwi.\scope [u#1, \offset], ->t
        c.sdi.\scope t#1, [u#1, \offset+8]
        c.ldi.\scope [u#1, \offset+8], ->t
        sdi.intra t#1, [a0, \result]
        .endm

        .globl _start
_start:
.if FAULT
        C.BSTART
        addtpc .Ltrap, ->a0
        addi a0, .Ltrap, ->a0
        ssrset a0, 0x0f01
        addtpc .Lcarrier, ->a6
        addi a6, .Lcarrier, ->a6
        C.BSTOP
        C.BSTART
        hl.liu 131072, ->a0
        hl.liu 0x1122, ->a1
        sdi.intra a1, [a0, 0]
.if FAULT == 1
.Lcarrier:
        c.swi.intra t#1, [a0, 0]
.else
        hl.liu 0x3344, ->t
.Lcarrier:
        c.swi.intra t#1, [u#1, 0]
.endif
        C.BSTOP
        C.BSTART DIRECT, .Lfail
        C.BSTOP
.Ltrap:
        C.BSTART COND, .Lfail
        ssrget 0x0f43, ->a5
        setc.ne a5, a6
        C.BSTOP
.else
        C.BSTART
        hl.liu 131072, ->a0
        hl.liu 131072, ->u
        transfer private, 0, 64
        transfer intra, 16, 72
        transfer inter, 32, 80
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
