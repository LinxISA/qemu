        # SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
        C.BSTART
        hl.liu 4660, ->a0
geometry_write:
        ssrset a0, 80
        C.BSTOP
