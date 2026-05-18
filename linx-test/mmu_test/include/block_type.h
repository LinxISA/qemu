/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Copyright (c) 2023 Huawei Technologies Co., Ltd.
 */

#ifndef BLOCK_TYPE_H
#define BLOCK_TYPE_H
#include "block-def.h"

/*
 * standard block head definition
 */
.macro block_std_head label
	bstart __&label&__.bstart
	b.std
.endm

/*
 * super standard block head definition
 */
.macro block_stdh_head label
	bstart __&label&__.bstart
	b.stdh
.endm

/*
 * aux block head definition
 */
.macro block_aux_head label
	bstart __&label&__.bstart
	b.aux
.endm

/*
 * system block head definition
 */
.macro block_sys_head label
	bstart __&label&__.bstart
	b.sys
.endm

.macro block_text_begin label
	bstop __&label&__.bstop
#if HAVE_BLOCK_TEXT_BODY_SECTION
	.pushsection BLOCK_TEXT_BODY_SECTION
#endif
	__&label&__.bstart:
.endm

.macro block_text_end label
	__&label&__.bstop:
	
#if HAVE_BLOCK_TEXT_BODY_SECTION
	.popsection
#endif
.endm

#endif /* _LINX_BLOCK_TYPE_H */
