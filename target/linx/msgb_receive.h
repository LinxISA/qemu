/*
 * PTO 0.59.1 Message Buffer receive state machine.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_MSGB_RECEIVE_H
#define LINX_MSGB_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>

#define LINX_MSGB_WORDS 10u
#define LINX_MSGB_CLAIM_SEQUENCE_MAX UINT64_C(0x0000ffffffffffff)

typedef enum LinxMsgbResult {
    LINX_MSGB_NONE = 0,
    LINX_MSGB_OK = 1,
    LINX_MSGB_DUPLICATE = 2,
    LINX_MSGB_EMPTY = 3,
    LINX_MSGB_BUSY = 4,
    LINX_MSGB_NO_CLAIM = 5,
    LINX_MSGB_NOT_OWNER = 6,
    LINX_MSGB_TOKEN_MISMATCH = 7,
    LINX_MSGB_STALE_EPOCH = 8,
    LINX_MSGB_BAD_TOKEN = 9,
    LINX_MSGB_NOT_READ = 10,
    LINX_MSGB_EXHAUSTED = 11,
    LINX_MSGB_BAD_COMMAND = 12,
    LINX_MSGB_IO_ERROR = 13,
} LinxMsgbResult;

typedef struct LinxMsgbPacket {
    uint64_t source_invocation;
    uint64_t destination_invocation;
    uint64_t message_id_hi;
    uint64_t message_id_lo;
    uint64_t control;
    uint8_t word_count;
    uint64_t words[LINX_MSGB_WORDS];
} LinxMsgbPacket;

typedef struct LinxMsgbQueuedMessage {
    bool valid;
    LinxMsgbPacket packet;
} LinxMsgbQueuedMessage;

typedef struct LinxMsgbSnapshot {
    bool valid;
    bool acked;
    uint64_t owner_invocation;
    uint64_t token;
    uint16_t read_mask;
    LinxMsgbPacket packet;
} LinxMsgbSnapshot;

typedef struct LinxMsgbReceiveBank {
    uint16_t epoch;
    uint64_t claim_sequence;
    LinxMsgbQueuedMessage queued;
    LinxMsgbSnapshot snapshot;
    LinxMsgbResult rx_result;
    LinxMsgbResult ack_result;
    bool ack_result_valid;
    uint64_t ack_id;
    bool last_success_valid;
    uint64_t last_success_token;
} LinxMsgbReceiveBank;

bool linx_msgb_receive_init(LinxMsgbReceiveBank *bank, uint16_t epoch);
LinxMsgbResult linx_msgb_receive_enqueue(LinxMsgbReceiveBank *bank,
                                         const LinxMsgbPacket *packet);
void linx_msgb_receive_claim(LinxMsgbReceiveBank *bank,
                             uint64_t invocation, uint64_t command);
bool linx_msgb_receive_read(LinxMsgbReceiveBank *bank,
                            uint64_t invocation, unsigned word,
                            uint64_t *value);
void linx_msgb_receive_ack(LinxMsgbReceiveBank *bank,
                           uint64_t invocation, uint64_t token);
void linx_msgb_receive_release(LinxMsgbReceiveBank *bank,
                               uint64_t invocation, uint64_t token);
uint64_t linx_msgb_receive_rxstate(const LinxMsgbReceiveBank *bank,
                                   uint64_t invocation);
uint64_t linx_msgb_receive_token(const LinxMsgbReceiveBank *bank,
                                 uint64_t invocation);
uint64_t linx_msgb_receive_ackstat(const LinxMsgbReceiveBank *bank);
uint64_t linx_msgb_receive_ackid(const LinxMsgbReceiveBank *bank);

#endif
