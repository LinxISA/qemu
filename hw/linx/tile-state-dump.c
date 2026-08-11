/*
 * Linx tile-state snapshot encoding
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/linx/tile-state-dump.h"

#define LINX_TILE_STATE_HEADER_SIZE 48u
#define LINX_TILE_STATE_RECORD_HEADER_SIZE 32u

static const uint8_t linx_tile_state_isa_sha256[32] = {
    0xcf, 0x50, 0x4d, 0xb8, 0x6e, 0xb4, 0x62, 0x15,
    0x67, 0x72, 0x32, 0xb9, 0xc8, 0x4f, 0xc6, 0x1c,
    0xef, 0x55, 0xa8, 0x09, 0x13, 0xf3, 0xd3, 0xd6,
    0x09, 0xb5, 0x9e, 0x6a, 0x38, 0x26, 0x72, 0xbd,
};

static void linx_put_le16(GByteArray *out, uint16_t value)
{
    const uint8_t bytes[] = { value & 0xffu, value >> 8 };

    g_byte_array_append(out, bytes, sizeof(bytes));
}

static void linx_put_le32(GByteArray *out, uint32_t value)
{
    const uint8_t bytes[] = {
        value & 0xffu, (value >> 8) & 0xffu,
        (value >> 16) & 0xffu, (value >> 24) & 0xffu,
    };

    g_byte_array_append(out, bytes, sizeof(bytes));
}

static bool linx_tile_dtype_is_packed(uint8_t dtype)
{
    return dtype == 12 || dtype == 14 || dtype == 20 || dtype == 28;
}

static bool linx_tile_state_invalid(GError **errp, const char *message)
{
    g_set_error_literal(errp, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        message);
    return false;
}

static bool linx_tile_state_validate_record(const LinxTileStateRecord *record,
                                            uint64_t *encoded_size,
                                            GError **errp)
{
    bool packed = linx_tile_dtype_is_packed(record->dtype);
    uint64_t row_bytes;
    uint64_t storage_bytes;

    if (record->layout != 0) {
        return linx_tile_state_invalid(errp,
                                       "tile-state layout must be zero");
    }
    if (record->elem_bytes == 0 || (packed && record->elem_bytes != 1)) {
        return linx_tile_state_invalid(errp,
                                       "tile-state element size is invalid");
    }
    if (record->valid_rows > record->rows ||
        record->valid_cols > record->cols) {
        return linx_tile_state_invalid(errp,
                                       "tile-state valid shape exceeds storage shape");
    }
    row_bytes = packed ? ((uint64_t)record->cols + 1) / 2
                       : (uint64_t)record->cols * record->elem_bytes;
    storage_bytes = (uint64_t)record->rows * row_bytes;
    if (storage_bytes > record->bytes) {
        return linx_tile_state_invalid(errp,
                                       "tile-state storage is shorter than its shape");
    }
    if (record->capacity < record->bytes) {
        return linx_tile_state_invalid(errp,
                                       "tile-state capacity is shorter than storage");
    }
    if (record->bytes != 0 && record->payload == NULL) {
        return linx_tile_state_invalid(errp,
                                       "tile-state payload is missing");
    }
    *encoded_size += LINX_TILE_STATE_RECORD_HEADER_SIZE +
                     2 * (uint64_t)record->bytes;
    if (*encoded_size > G_MAXUINT) {
        return linx_tile_state_invalid(errp,
                                       "tile-state output is too large");
    }
    return true;
}

bool linx_tile_state_encode(GByteArray *out, unsigned pe_count,
                            const LinxTileStateRecord *records,
                            size_t record_count, GError **errp)
{
    uint64_t encoded_size = LINX_TILE_STATE_HEADER_SIZE;

    if (out == NULL) {
        return linx_tile_state_invalid(errp,
                                       "tile-state output buffer is missing");
    }
    g_byte_array_set_size(out, 0);
    if (pe_count > UINT8_MAX) {
        return linx_tile_state_invalid(errp,
                                       "tile-state PE count exceeds the file format");
    }
    if (record_count > UINT32_MAX) {
        return linx_tile_state_invalid(errp,
                                       "tile-state record count exceeds the file format");
    }
    if (record_count != 0 && records == NULL) {
        return linx_tile_state_invalid(errp,
                                       "tile-state records are missing");
    }
    for (size_t i = 0; i < record_count; i++) {
        if (!linx_tile_state_validate_record(&records[i], &encoded_size,
                                             errp)) {
            return false;
        }
    }

    g_byte_array_append(out, (const uint8_t *)"PTOAST58", 8);
    linx_put_le16(out, 1);
    g_byte_array_append(out, linx_tile_state_isa_sha256,
                        sizeof(linx_tile_state_isa_sha256));
    const uint8_t pe_count_byte = (uint8_t)pe_count;
    g_byte_array_append(out, &pe_count_byte, 1);
    g_byte_array_append(out, (const uint8_t *)"\0", 1);
    linx_put_le32(out, (uint32_t)record_count);

    for (size_t record_index = 0; record_index < record_count; record_index++) {
        const LinxTileStateRecord *record = &records[record_index];
        bool packed = linx_tile_dtype_is_packed(record->dtype);
        uint32_t row_bytes = packed ? ((uint32_t)record->cols + 1) / 2
                                    : record->cols * record->elem_bytes;
        const uint8_t header[] = {
            record->pe, record->hand, record->rank, 1,
            record->dtype, record->layout, 0, 0,
        };

        g_byte_array_append(out, header, sizeof(header));
        linx_put_le32(out, record->capacity);
        linx_put_le32(out, record->bytes);
        linx_put_le16(out, record->valid_rows);
        linx_put_le16(out, record->valid_cols);
        linx_put_le16(out, record->rows);
        linx_put_le16(out, record->cols);
        linx_put_le32(out, record->bytes);
        linx_put_le32(out, record->bytes);
        for (uint32_t i = 0; i < record->bytes; i++) {
            uint8_t defined = 0;

            if (packed) {
                uint32_t row = row_bytes == 0 ? 0 : i / row_bytes;
                uint32_t byte_col = row_bytes == 0 ? 0 : i % row_bytes;

                if (row < record->valid_rows) {
                    if (2 * byte_col < record->valid_cols) {
                        defined |= 0x0f;
                    }
                    if (2 * byte_col + 1 < record->valid_cols) {
                        defined |= 0xf0;
                    }
                }
            } else {
                uint32_t element = i / record->elem_bytes;
                uint32_t row = record->cols == 0 ? 0 : element / record->cols;
                uint32_t col = record->cols == 0 ? 0 : element % record->cols;

                if (record->cols != 0 && row < record->valid_rows &&
                    col < record->valid_cols) {
                    defined = 0xff;
                }
            }
            g_byte_array_append(out, &defined, 1);
        }
        if (record->bytes != 0) {
            g_byte_array_append(out, record->payload, record->bytes);
        }
    }
    return true;
}
