/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "../../target/linx/msgb_receive.h"

static void assert_core_state_equal(const LinxMsgbReceiveBank *left,
                                    const LinxMsgbReceiveBank *right)
{
    g_assert_cmpuint(left->epoch, ==, right->epoch);
    g_assert_cmpuint(left->claim_sequence, ==, right->claim_sequence);
    g_assert_cmpmem(&left->queued, sizeof(left->queued),
                    &right->queued, sizeof(right->queued));
    g_assert_cmpmem(&left->snapshot, sizeof(left->snapshot),
                    &right->snapshot, sizeof(right->snapshot));
    g_assert_cmpuint(left->last_success_valid, ==,
                     right->last_success_valid);
    g_assert_cmpuint(left->last_success_token, ==,
                     right->last_success_token);
}

static LinxMsgbPacket packet(uint64_t message_id_lo)
{
    LinxMsgbPacket value = {
        .source_invocation = 40,
        .destination_invocation = 41,
        .message_id_hi = UINT64_C(0x0000001000000001),
        .message_id_lo = message_id_lo,
        .control = UINT64_C(0x0000000200000010),
        .word_count = 2,
        .words = { UINT64_C(0x1234), UINT64_C(0xfedcba9876543210) },
    };
    return value;
}

static void test_receive_lifecycle(void)
{
    LinxMsgbReceiveBank bank;
    LinxMsgbReceiveBank peer;
    LinxMsgbReceiveBank before;
    LinxMsgbPacket first = packet(1);
    LinxMsgbPacket second = packet(2);
    LinxMsgbPacket invalid = first;
    uint64_t value = 0;
    const uint64_t token1 = UINT64_C(0x0001000000000001);
    const uint64_t token2 = UINT64_C(0x0001000000000002);
    const uint64_t token3 = UINT64_C(0x0001000000000003);

    g_assert_true(linx_msgb_receive_init(&bank, 1));
    g_assert_true(linx_msgb_receive_init(&peer, 2));
    g_assert_cmpuint(linx_msgb_receive_ackstat(&bank), ==, 0);
    g_assert_cmpuint(linx_msgb_receive_ackid(&bank), ==, 0);
    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_EMPTY);
    invalid.word_count = LINX_MSGB_WORDS + 1;
    before = bank;
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &invalid), ==,
                     LINX_MSGB_BAD_COMMAND);
    g_assert_cmpmem(&bank, sizeof(bank), &before, sizeof(before));
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &first), ==,
                     LINX_MSGB_OK);

    before = bank;
    linx_msgb_receive_claim(&bank, 41, 0);
    g_assert_cmpmem(&bank, sizeof(bank), &before, sizeof(before));
    linx_msgb_receive_claim(&bank, 41, 2);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_BAD_COMMAND);
    assert_core_state_equal(&bank, &before);

    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_OK);
    g_assert_cmpuint(linx_msgb_receive_token(&bank, 41), ==, token1);
    g_assert_cmpuint(linx_msgb_receive_token(&bank, 40), ==, 0);
    g_assert_cmpuint(linx_msgb_receive_rxstate(&bank, 41), ==,
                     LINX_MSGB_OK | (1u << 8) | (2u << 12));
    before = bank;
    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_BUSY);
    assert_core_state_equal(&bank, &before);

    g_assert_cmpuint(linx_msgb_receive_enqueue(&peer, &first), ==,
                     LINX_MSGB_OK);
    linx_msgb_receive_claim(&peer, 61, 1);
    g_assert_cmpuint(linx_msgb_receive_token(&peer, 61), ==,
                     UINT64_C(0x0002000000000001));
    g_assert_cmpuint(linx_msgb_receive_token(&bank, 41), ==, token1);

    before = bank;
    linx_msgb_receive_ack(&bank, 40, token1);
    g_assert_cmpuint(linx_msgb_receive_ackstat(&bank), ==,
                     (1u << 8) | LINX_MSGB_NOT_OWNER);
    g_assert_cmpuint(linx_msgb_receive_ackid(&bank), ==, token1);
    assert_core_state_equal(&bank, &before);
    linx_msgb_receive_ack(&bank, 40, token1 + 1);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_NOT_OWNER);
    g_assert_cmpuint(bank.ack_id, ==, token1 + 1);
    assert_core_state_equal(&bank, &before);
    uint64_t stale_token = UINT64_C(0x0002000000000001);
    linx_msgb_receive_ack(&bank, 41, stale_token);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_STALE_EPOCH);
    g_assert_cmpuint(bank.ack_id, ==, stale_token);
    assert_core_state_equal(&bank, &before);
    value = UINT64_C(0xaaaaaaaaaaaaaaaa);
    g_assert_false(linx_msgb_receive_read(&bank, 40, 0, &value));
    g_assert_cmpuint(value, ==, UINT64_C(0xaaaaaaaaaaaaaaaa));
    g_assert_false(linx_msgb_receive_read(&bank, 41, 2, &value));
    g_assert_cmpuint(value, ==, UINT64_C(0xaaaaaaaaaaaaaaaa));
    g_assert_false(linx_msgb_receive_read(&bank, 41, 0, NULL));
    assert_core_state_equal(&bank, &before);

    linx_msgb_receive_release(&bank, 41, 0);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_BAD_TOKEN);
    assert_core_state_equal(&bank, &before);
    linx_msgb_receive_release(&bank, 41,
                              UINT64_C(0x0002000000000001));
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_STALE_EPOCH);
    assert_core_state_equal(&bank, &before);
    linx_msgb_receive_release(&bank, 40, token1 + 1);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_NOT_OWNER);
    assert_core_state_equal(&bank, &before);
    linx_msgb_receive_release(&bank, 41, token1 + 1);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_TOKEN_MISMATCH);
    assert_core_state_equal(&bank, &before);

    linx_msgb_receive_ack(&bank, 41, token1);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_NOT_READ);
    g_assert_cmpuint(bank.ack_id, ==, token1);
    assert_core_state_equal(&bank, &before);
    g_assert_true(linx_msgb_receive_read(&bank, 41, 0, &value));
    g_assert_cmpuint(value, ==, UINT64_C(0x1234));
    linx_msgb_receive_ack(&bank, 41, token1);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_NOT_READ);
    g_assert_true(bank.queued.valid);

    linx_msgb_receive_release(&bank, 41, token1);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_OK);
    g_assert_false(bank.snapshot.valid);
    g_assert_true(bank.queued.valid);
    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_cmpuint(bank.snapshot.token, ==, token2);
    g_assert_cmpuint(bank.snapshot.read_mask, ==, 0);
    before = bank;
    linx_msgb_receive_ack(&bank, 41, token1);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_TOKEN_MISMATCH);
    assert_core_state_equal(&bank, &before);

    g_assert_true(linx_msgb_receive_read(&bank, 41, 1, &value));
    g_assert_cmpuint(value, ==, UINT64_C(0xfedcba9876543210));
    linx_msgb_receive_ack(&bank, 41, token2);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_NOT_READ);
    g_assert_true(linx_msgb_receive_read(&bank, 41, 0, &value));
    linx_msgb_receive_ack(&bank, 41, token2);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_OK);
    g_assert_false(bank.queued.valid);
    LinxMsgbQueuedMessage empty_queue = { 0 };
    g_assert_cmpmem(&bank.queued, sizeof(bank.queued),
                    &empty_queue, sizeof(empty_queue));
    g_assert_true(bank.snapshot.valid);
    g_assert_true(bank.snapshot.acked);
    g_assert_cmpuint(linx_msgb_receive_rxstate(&bank, 41), ==,
                     LINX_MSGB_OK | (1u << 8) | (1u << 9) | (2u << 12));
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &second), ==,
                     LINX_MSGB_OK);
    g_assert_cmpuint(bank.queued.packet.message_id_lo, ==, 2);
    g_assert_cmpuint(bank.snapshot.packet.message_id_lo, ==, 1);
    g_assert_true(linx_msgb_receive_read(&bank, 41, 1, &value));
    g_assert_cmpuint(value, ==, UINT64_C(0xfedcba9876543210));
    before = bank;
    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_BUSY);
    assert_core_state_equal(&bank, &before);

    before = bank;
    linx_msgb_receive_ack(&bank, 41, 0);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_BAD_TOKEN);
    g_assert_cmpuint(bank.ack_id, ==, 0);
    assert_core_state_equal(&bank, &before);
    linx_msgb_receive_ack(&bank, 41, token2);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_DUPLICATE);
    g_assert_cmpuint(bank.ack_id, ==, token2);
    assert_core_state_equal(&bank, &before);
    linx_msgb_receive_release(&bank, 41, token2);
    g_assert_false(bank.snapshot.valid);
    g_assert_true(bank.queued.valid);
    g_assert_cmpuint(bank.queued.packet.message_id_lo, ==, 2);
    before = bank;
    linx_msgb_receive_ack(&bank, 41, token2);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_DUPLICATE);
    assert_core_state_equal(&bank, &before);
    linx_msgb_receive_release(&bank, 41, token2);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_NO_CLAIM);
    assert_core_state_equal(&bank, &before);

    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_cmpuint(bank.snapshot.token, ==, token3);
    before = bank;
    linx_msgb_receive_ack(&bank, 41, token2);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_DUPLICATE);
    assert_core_state_equal(&bank, &before);
    g_assert_cmpuint(peer.ack_result, ==, LINX_MSGB_NONE);
    g_assert_cmpuint(peer.ack_id, ==, 0);
}

static void test_receive_io_error_and_exhaustion(void)
{
    LinxMsgbReceiveBank bank;
    LinxMsgbReceiveBank before;
    LinxMsgbPacket first = packet(1);
    uint64_t value;
    const uint64_t final_token = UINT64_C(0x0001ffffffffffff);

    g_assert_true(linx_msgb_receive_init(&bank, 1));
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &first), ==,
                     LINX_MSGB_OK);
    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_true(linx_msgb_receive_read(&bank, 41, 0, &value));
    g_assert_true(linx_msgb_receive_read(&bank, 41, 1, &value));
    bank.queued.packet.message_id_lo = 2;
    before = bank;
    linx_msgb_receive_ack(&bank, 41, bank.snapshot.token);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_IO_ERROR);
    assert_core_state_equal(&bank, &before);

    g_assert_true(linx_msgb_receive_init(&bank, 1));
    bank.claim_sequence = LINX_MSGB_CLAIM_SEQUENCE_MAX - 1;
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &first), ==,
                     LINX_MSGB_OK);
    linx_msgb_receive_claim(&bank, 41, 1);
    g_assert_cmpuint(bank.snapshot.token, ==, final_token);
    before = bank;
    linx_msgb_receive_release(&bank, 41, final_token);
    g_assert_cmpuint(bank.rx_result, ==, LINX_MSGB_EXHAUSTED);
    assert_core_state_equal(&bank, &before);
    g_assert_true(linx_msgb_receive_read(&bank, 41, 0, &value));
    g_assert_true(linx_msgb_receive_read(&bank, 41, 1, &value));
    linx_msgb_receive_ack(&bank, 41, final_token);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_OK);
    linx_msgb_receive_release(&bank, 41, final_token);
    g_assert_false(bank.snapshot.valid);
    before = bank;
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &first), ==,
                     LINX_MSGB_EXHAUSTED);
    assert_core_state_equal(&bank, &before);
}

static void test_receive_configuration_and_admission(void)
{
    LinxMsgbReceiveBank bank;
    LinxMsgbReceiveBank before = { 0 };
    LinxMsgbPacket first = packet(1);

    g_assert_false(linx_msgb_receive_init(&bank, 0));
    g_assert_cmpmem(&bank, sizeof(bank), &before, sizeof(before));
    g_assert_true(linx_msgb_receive_init(&bank, 1));
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, NULL), ==,
                     LINX_MSGB_BAD_COMMAND);
    bank.claim_sequence = LINX_MSGB_CLAIM_SEQUENCE_MAX;
    before = bank;
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &first), ==,
                     LINX_MSGB_EXHAUSTED);
    g_assert_cmpmem(&bank, sizeof(bank), &before, sizeof(before));
    g_assert_true(linx_msgb_receive_init(&bank, 1));
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &first), ==,
                     LINX_MSGB_OK);
    before = bank;
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &first), ==,
                     LINX_MSGB_BUSY);
    g_assert_cmpmem(&bank, sizeof(bank), &before, sizeof(before));
}

static void test_receive_ten_word_read_mask(void)
{
    LinxMsgbReceiveBank bank;
    LinxMsgbPacket value = packet(7);
    uint64_t word;

    value.word_count = LINX_MSGB_WORDS;
    for (unsigned i = 0; i < LINX_MSGB_WORDS; i++) {
        value.words[i] = i + 1;
    }
    g_assert_true(linx_msgb_receive_init(&bank, 3));
    g_assert_cmpuint(linx_msgb_receive_enqueue(&bank, &value), ==,
                     LINX_MSGB_OK);
    linx_msgb_receive_claim(&bank, 71, 1);
    for (unsigned i = 0; i < LINX_MSGB_WORDS; i++) {
        g_assert_true(linx_msgb_receive_read(&bank, 71, i, &word));
        g_assert_cmpuint(word, ==, i + 1);
    }
    g_assert_cmpuint(bank.snapshot.read_mask, ==, 0x3ff);
    linx_msgb_receive_ack(&bank, 71, bank.snapshot.token);
    g_assert_cmpuint(bank.ack_result, ==, LINX_MSGB_OK);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/msgb/receive-lifecycle", test_receive_lifecycle);
    g_test_add_func("/linx/msgb/io-error-and-exhaustion",
                    test_receive_io_error_and_exhaustion);
    g_test_add_func("/linx/msgb/configuration-and-admission",
                    test_receive_configuration_and_admission);
    g_test_add_func("/linx/msgb/ten-word-read-mask",
                    test_receive_ten_word_read_mask);
    return g_test_run();
}
