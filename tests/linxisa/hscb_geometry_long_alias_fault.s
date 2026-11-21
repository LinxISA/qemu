        # SPDX-License-Identifier: GPL-2.0-or-later
        .text
        .globl _start
_start:
        C.BSTART
geometry_long_alias:
        hl.ssrget 1048656, ->a0
        C.BSTOP
