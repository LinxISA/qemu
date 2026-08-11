/*
 * Linx tile-state snapshot encoding
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_LINX_TILE_STATE_DUMP_H
#define HW_LINX_TILE_STATE_DUMP_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct LinxTileStateRecord {
    uint8_t pe;
    uint8_t hand;
    uint8_t rank;
    uint8_t dtype;
    uint8_t layout;
    uint8_t elem_bytes;
    uint32_t capacity;
    uint32_t bytes;
    uint16_t valid_rows;
    uint16_t valid_cols;
    uint16_t rows;
    uint16_t cols;
    const uint8_t *payload;
} LinxTileStateRecord;

bool linx_tile_state_encode(GByteArray *out, unsigned pe_count,
                            const LinxTileStateRecord *records,
                            size_t record_count, GError **errp);

#endif
