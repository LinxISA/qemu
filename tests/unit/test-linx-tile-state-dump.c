/*
 * Linx tile-state snapshot encoder tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/linx/tile-state-dump.h"

static void test_scalar_and_raw_payload(void)
{
    static const uint8_t payload[] = { 0x12, 0x34, 0x56, 0x78 };
    static const uint8_t expected[] = {
        'P', 'T', 'O', 'A', 'S', 'T', '5', '8',
        0x01, 0x00,
        0xcf, 0x50, 0x4d, 0xb8, 0x6e, 0xb4, 0x62, 0x15,
        0x67, 0x72, 0x32, 0xb9, 0xc8, 0x4f, 0xc6, 0x1c,
        0xef, 0x55, 0xa8, 0x09, 0x13, 0xf3, 0xd3, 0xd6,
        0x09, 0xb5, 0x9e, 0x6a, 0x38, 0x26, 0x72, 0xbd,
        0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x02, 0x03, 0x01, 0x01, 0x04, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0x12, 0x34, 0x56, 0x78,
    };
    const LinxTileStateRecord record = {
        .pe = 2,
        .hand = 3,
        .rank = 1,
        .dtype = 4,
        .layout = 0,
        .elem_bytes = 2,
        .capacity = 4,
        .bytes = 4,
        .valid_rows = 1,
        .valid_cols = 2,
        .rows = 1,
        .cols = 2,
        .payload = payload,
    };
    g_autoptr(GByteArray) encoded = g_byte_array_new();
    g_autoptr(GError) err = NULL;

    g_assert_true(linx_tile_state_encode(encoded, 4, &record, 1, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(encoded->len, ==, sizeof(expected));
    g_assert_cmpmem(encoded->data, encoded->len, expected, sizeof(expected));
    g_assert_cmphex(encoded->data[42], ==, 0x04);
    g_assert_cmpmem(encoded->data + 44, 4, "\x01\x00\x00\x00", 4);
    g_assert_cmpmem(encoded->data + 84, 4, "\x12\x34\x56\x78", 4);
}

static void test_packed_defined_nibbles(void)
{
    static const uint8_t payload[] = { 0x21, 0x03 };
    const LinxTileStateRecord record = {
        .dtype = 12,
        .elem_bytes = 1,
        .capacity = 2,
        .bytes = 2,
        .valid_rows = 1,
        .valid_cols = 3,
        .rows = 1,
        .cols = 4,
        .payload = payload,
    };
    g_autoptr(GByteArray) encoded = g_byte_array_new();
    g_autoptr(GError) err = NULL;
    const size_t mask_offset = 80;

    g_assert_true(linx_tile_state_encode(encoded, 1, &record, 1, &err));
    g_assert_no_error(err);
    g_assert_cmphex(encoded->data[mask_offset + 0], ==, 0xff);
    g_assert_cmphex(encoded->data[mask_offset + 1], ==, 0x0f);
}

static LinxTileStateRecord valid_record(void)
{
    static const uint8_t payload[] = { 0 };

    return (LinxTileStateRecord) {
        .elem_bytes = 1,
        .capacity = 1,
        .bytes = 1,
        .valid_rows = 1,
        .valid_cols = 1,
        .rows = 1,
        .cols = 1,
        .payload = payload,
    };
}

static void assert_rejected(unsigned pe_count, LinxTileStateRecord *record)
{
    g_autoptr(GByteArray) encoded = g_byte_array_new();
    g_autoptr(GError) err = NULL;

    g_byte_array_append(encoded, (const uint8_t *)"dirty", 5);
    g_assert_false(linx_tile_state_encode(encoded, pe_count, record, 1, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_cmpuint(encoded->len, ==, 0);
}

static void test_reject_invalid_records(void)
{
    LinxTileStateRecord record = valid_record();

    assert_rejected(UINT8_MAX + 1u, &record);

    record = valid_record();
    record.layout = 1;
    assert_rejected(1, &record);

    record = valid_record();
    record.dtype = 14;
    record.elem_bytes = 2;
    record.bytes = record.capacity = 2;
    assert_rejected(1, &record);

    record = valid_record();
    record.valid_rows = 2;
    assert_rejected(1, &record);

    record = valid_record();
    record.rows = 2;
    assert_rejected(1, &record);

    record = valid_record();
    record.capacity = 0;
    assert_rejected(1, &record);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/tile-state/scalar-and-raw-payload",
                    test_scalar_and_raw_payload);
    g_test_add_func("/linx/tile-state/packed-defined-nibbles",
                    test_packed_defined_nibbles);
    g_test_add_func("/linx/tile-state/reject-invalid-records",
                    test_reject_invalid_records);
    return g_test_run();
}
