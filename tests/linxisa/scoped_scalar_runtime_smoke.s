        # SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .p2align 2
scoped_pcr_data:
        .4byte 0x12345678

        .globl _start
_start:
        C.BSTART
        hl.liu 131072, ->a0
        hl.liu 21930, ->a2
        sdi zero, [a0, 0]
        .8byte 0x000000013199041f # LDI.private [a0, 0], ->a1
        .8byte 0x000004222059051f # SWI.intra a2, [a0, 8]
        .8byte 0x000000212299061f # LWI.inter [a0, 8], ->a3
        .8byte 0x0000fffaa339041f # LW.PCR.private scoped_pcr_data, ->a4
        C.BSTOP

        C.BSTART COND, .Lfail
        setc.ne a1, zero
        C.BSTOP
        C.BSTART COND, .Lfail
        setc.ne a3, a2
        C.BSTOP
        C.BSTART
        hl.liu 305419896, ->a5
        C.BSTOP
        C.BSTART COND, .Lfail
        setc.ne a4, a5
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
