# SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
        C.BSTART
        hl.liu 131104, ->a2
        hl.liu 21850, ->a0
        sdi a0, [a2, 0]
        swi a0, [a2, -4]
        .8byte 0xffe232992ffe091f # HL.LDI.PO.intra  [a2, -16], ->a3,a3
        addi a3, 0, ->a6
        hl.liu 1, ->a4
        sub zero, a4, ->a4
        .8byte 0x12622289382e0a1f # HL.LW.PR.inter  [a2,a4.sw<<2], ->a3,a5
        hl.liu 1, ->a4
        hl.liu 26214, ->a0
        .8byte 0x14622049203e081f # HL.SW.PO.private a0,[a2,a4.uw], ->a2
        C.BSTOP

        C.BSTART COND, .Lfail
        hl.liu 131088, ->a1
        setc.ne a6, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 21850, ->a1
        setc.ne a3, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 131100, ->a1
        setc.ne a5, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 131108, ->a1
        setc.ne a2, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 131104, ->a1
        lwi [a1, 0], ->a3
        setc.ne a3, a0
        C.BSTOP

        C.BSTART
        hl.liu 131120, ->a0
        hl.liu 8, ->a4
        hl.liu 17185, ->a2
        .8byte 0x20617049282e081f # HL.SD.UPR.private a2,[a0,a4], ->a3
        C.BSTOP
        C.BSTART COND, .Lfail
        hl.liu 131128, ->a1
        setc.ne a3, a1
        C.BSTOP
        C.BSTART COND, .Lfail
        ldi [a3, 0], ->a6
        setc.ne a6, a2
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
