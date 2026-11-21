        # SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
        C.BSTART
geometry_alias:
        ssrget 82, ->a0
        C.BSTOP
