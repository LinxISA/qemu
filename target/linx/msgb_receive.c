/*
 * PTO 0.59.1 Message Buffer receive state machine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "msgb_receive.h"

static bool linx_msgb_token_valid(uint64_t token)
{
    return (token >> 48) != 0 && (token & LINX_MSGB_CLAIM_SEQUENCE_MAX) != 0;
}

static bool linx_msgb_snapshot_owner_matches(const LinxMsgbReceiveBank *bank,
                                             uint64_t invocation)
{
    return bank->snapshot.valid &&
           bank->snapshot.owner_invocation == invocation;
}

static bool linx_msgb_read_complete(const LinxMsgbSnapshot *snapshot)
{
    uint16_t required = snapshot->packet.word_count == LINX_MSGB_WORDS ?
                        (UINT16_C(1) << LINX_MSGB_WORDS) - 1 :
                        (UINT16_C(1) << snapshot->packet.word_count) - 1;

    return (snapshot->read_mask & required) == required;
}

static void linx_msgb_clear_queued(LinxMsgbReceiveBank *bank)
{
    memset(&bank->queued, 0, sizeof(bank->queued));
}

static void linx_msgb_clear_snapshot(LinxMsgbReceiveBank *bank)
{
    memset(&bank->snapshot, 0, sizeof(bank->snapshot));
}

bool linx_msgb_receive_init(LinxMsgbReceiveBank *bank, uint16_t epoch)
{
    memset(bank, 0, sizeof(*bank));
    if (epoch == 0) {
        return false;
    }
    bank->epoch = epoch;
    return true;
}

LinxMsgbResult linx_msgb_receive_enqueue(LinxMsgbReceiveBank *bank,
                                         const LinxMsgbPacket *packet)
{
    if (!packet || packet->word_count > LINX_MSGB_WORDS) {
        return LINX_MSGB_BAD_COMMAND;
    }
    if (bank->queued.valid) {
        return LINX_MSGB_BUSY;
    }
    if (bank->claim_sequence == LINX_MSGB_CLAIM_SEQUENCE_MAX) {
        return LINX_MSGB_EXHAUSTED;
    }
    bank->queued.valid = true;
    bank->queued.packet = *packet;
    return LINX_MSGB_OK;
}

void linx_msgb_receive_claim(LinxMsgbReceiveBank *bank,
                             uint64_t invocation, uint64_t command)
{
    if (command == 0) {
        return;
    }
    if (command != 1) {
        bank->rx_result = LINX_MSGB_BAD_COMMAND;
        return;
    }
    if (bank->snapshot.valid) {
        bank->rx_result = LINX_MSGB_BUSY;
        return;
    }
    if (!bank->queued.valid) {
        bank->rx_result = LINX_MSGB_EMPTY;
        return;
    }
    if (bank->claim_sequence == LINX_MSGB_CLAIM_SEQUENCE_MAX) {
        bank->rx_result = LINX_MSGB_EXHAUSTED;
        return;
    }

    bank->claim_sequence++;
    bank->snapshot.valid = true;
    bank->snapshot.acked = false;
    bank->snapshot.owner_invocation = invocation;
    bank->snapshot.token = ((uint64_t)bank->epoch << 48) |
                           bank->claim_sequence;
    bank->snapshot.read_mask = 0;
    bank->snapshot.packet = bank->queued.packet;
    bank->rx_result = LINX_MSGB_OK;
}

bool linx_msgb_receive_read(LinxMsgbReceiveBank *bank,
                            uint64_t invocation, unsigned word,
                            uint64_t *value)
{
    if (!value || !linx_msgb_snapshot_owner_matches(bank, invocation) ||
        word >= bank->snapshot.packet.word_count) {
        return false;
    }

    bank->snapshot.read_mask |= UINT16_C(1) << word;
    *value = bank->snapshot.packet.words[word];
    return true;
}

static LinxMsgbResult linx_msgb_ack_result(const LinxMsgbReceiveBank *bank,
                                           uint64_t invocation,
                                           uint64_t token)
{
    if (!linx_msgb_token_valid(token)) {
        return LINX_MSGB_BAD_TOKEN;
    }
    if ((token >> 48) != bank->epoch) {
        return LINX_MSGB_STALE_EPOCH;
    }
    if (bank->last_success_valid && token == bank->last_success_token) {
        return LINX_MSGB_DUPLICATE;
    }
    if (!bank->snapshot.valid) {
        return LINX_MSGB_NO_CLAIM;
    }
    if (bank->snapshot.owner_invocation != invocation) {
        return LINX_MSGB_NOT_OWNER;
    }
    if (bank->snapshot.token != token) {
        return LINX_MSGB_TOKEN_MISMATCH;
    }
    if (!linx_msgb_read_complete(&bank->snapshot)) {
        return LINX_MSGB_NOT_READ;
    }
    if (!bank->queued.valid ||
        bank->queued.packet.message_id_hi !=
            bank->snapshot.packet.message_id_hi ||
        bank->queued.packet.message_id_lo !=
            bank->snapshot.packet.message_id_lo) {
        return LINX_MSGB_IO_ERROR;
    }
    return LINX_MSGB_OK;
}

void linx_msgb_receive_ack(LinxMsgbReceiveBank *bank,
                           uint64_t invocation, uint64_t token)
{
    LinxMsgbResult result = linx_msgb_ack_result(bank, invocation, token);

    bank->ack_result = result;
    bank->ack_result_valid = true;
    bank->ack_id = token;
    if (result == LINX_MSGB_OK) {
        linx_msgb_clear_queued(bank);
        bank->snapshot.acked = true;
        bank->last_success_valid = true;
        bank->last_success_token = token;
    }
}

void linx_msgb_receive_release(LinxMsgbReceiveBank *bank,
                               uint64_t invocation, uint64_t token)
{
    if (!linx_msgb_token_valid(token)) {
        bank->rx_result = LINX_MSGB_BAD_TOKEN;
    } else if ((token >> 48) != bank->epoch) {
        bank->rx_result = LINX_MSGB_STALE_EPOCH;
    } else if (!bank->snapshot.valid) {
        bank->rx_result = LINX_MSGB_NO_CLAIM;
    } else if (bank->snapshot.owner_invocation != invocation) {
        bank->rx_result = LINX_MSGB_NOT_OWNER;
    } else if (bank->snapshot.token != token) {
        bank->rx_result = LINX_MSGB_TOKEN_MISMATCH;
    } else if (!bank->snapshot.acked &&
               bank->claim_sequence == LINX_MSGB_CLAIM_SEQUENCE_MAX) {
        bank->rx_result = LINX_MSGB_EXHAUSTED;
    } else {
        linx_msgb_clear_snapshot(bank);
        bank->rx_result = LINX_MSGB_OK;
    }
}

uint64_t linx_msgb_receive_rxstate(const LinxMsgbReceiveBank *bank,
                                   uint64_t invocation)
{
    uint64_t state = bank->rx_result;

    if (linx_msgb_snapshot_owner_matches(bank, invocation)) {
        state |= UINT64_C(1) << 8;
        state |= (uint64_t)bank->snapshot.acked << 9;
        state |= (uint64_t)bank->snapshot.packet.word_count << 12;
    }
    return state;
}

uint64_t linx_msgb_receive_token(const LinxMsgbReceiveBank *bank,
                                 uint64_t invocation)
{
    return linx_msgb_snapshot_owner_matches(bank, invocation) ?
           bank->snapshot.token : 0;
}

uint64_t linx_msgb_receive_ackstat(const LinxMsgbReceiveBank *bank)
{
    return bank->ack_result |
           ((uint64_t)bank->ack_result_valid << 8);
}

uint64_t linx_msgb_receive_ackid(const LinxMsgbReceiveBank *bank)
{
    return bank->ack_id;
}
