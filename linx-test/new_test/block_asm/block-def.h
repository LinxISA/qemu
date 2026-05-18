/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (c) 2023 Huawei Technologies Co., Ltd.
 */

#ifndef _LINX_BLOCK_DEF_H
#define _LINX_BLOCK_DEF_H

/*
 * Default HAVE_BLOCK_TEXT_BODY_SECTION is set, so that
 * block text body codes will be placed in the section which
 * defined by BLOCK_TEXT_BODY_SECTION. To place block text body codes
 * in default section, unset HAVE_BLOCK_TEXT_BODY_SECTION
 * before include this file, e.g. through AFLAGS or CFLAGS in Makefile.
 */
#ifndef HAVE_BLOCK_TEXT_BODY_SECTION
#define HAVE_BLOCK_TEXT_BODY_SECTION 1
#endif

/*
 * Default block text body codes will be placed in ".text.body" section.
 * To change the section, set BLOCK_TEXT_BODY_SECTION
 * before include this file, e.g. through AFLAGS or CFLAGS in Makefile.
 */
#if HAVE_BLOCK_TEXT_BODY_SECTION
#ifndef BLOCK_TEXT_BODY_SECTION
#define BLOCK_TEXT_BODY_SECTION ".text.body", "ax"
#endif
#endif

/*
 * {PUSH, POP}_BLOCK_TEXT_BODY_SECTION are used for inline assembly codes.
 */
#if HAVE_BLOCK_TEXT_BODY_SECTION
#define PUSH_BLOCK_TEXT_BODY_SECTION ".pushsection \".text.body\",\"ax\"\n"
#define POP_BLOCK_TEXT_BODY_SECTION  ".popsection\n"
#else
#define PUSH_BLOCK_TEXT_BODY_SECTION
#define POP_BLOCK_TEXT_BODY_SECTION
#endif

#endif /* _LINX_BLOCK_DEF_H */

