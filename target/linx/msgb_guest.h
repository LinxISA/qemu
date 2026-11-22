/*
 * PTO 0.59.1 Message Buffer guest-visible owner.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_MSGB_GUEST_H
#define LINX_MSGB_GUEST_H

#include "msgb_receive.h"

#define LINX_MSGB_ACR_COUNT 16u
#define LINX_MSGB_PE_COUNT 4u
#define LINX_MSGB_THREADS_PER_PE 2u
#define LINX_MSGB_ENDPOINTS \
    (LINX_MSGB_PE_COUNT * LINX_MSGB_THREADS_PER_PE * LINX_MSGB_ACR_COUNT)
#define LINX_MSGB_HSCB_BANK LINX_MSGB_ENDPOINTS
#define LINX_MSGB_BANKS (LINX_MSGB_ENDPOINTS + 1u)
#define LINX_MSGB_RECEIPTS 16u
#define LINX_MSGB_HSCB_ENDPOINT UINT32_MAX

typedef struct LinxMsgbReplayReceipt {
    bool valid;
    uint64_t invocation;
    uint8_t acr;
    uint64_t execution_domain;
    uint64_t tpc;
    uint64_t control;
    uint64_t message_id_hi;
    uint64_t message_id_lo;
} LinxMsgbReplayReceipt;

typedef struct LinxMsgbGuestBank {
    LinxMsgbReceiveBank receive;
    bool enabled;
    bool domain_bound;
    uint64_t domain;
    uint64_t bound_invocation;
    uint32_t source_epoch;
    uint64_t transmit_sequence;
    uint64_t staging[LINX_MSGB_WORDS];
    uint64_t last_accepted_control;
    uint64_t tx_message_id_hi;
    uint64_t tx_message_id_lo;
    LinxMsgbResult tx_result;
    bool tx_result_valid;
    LinxMsgbReplayReceipt receipt[LINX_MSGB_RECEIPTS];
    bool hscb_ready;
    bool hscb_waiting;
    bool hscb_running;
    uint64_t hscb_dispatch_id;
    uint64_t hscb_launch_message_id_hi;
    uint64_t hscb_launch_message_id_lo;
    uint64_t hscb_blocknum;
    uint64_t hscb_blockid;
    uint64_t hscb_return_value;
    unsigned hscb_issued_count;
    uint64_t hscb_issued_id[LINX_MSGB_RECEIPTS];
} LinxMsgbGuestBank;

typedef enum LinxMsgbControlRequest {
    LINX_MSGB_CONTROL_NONE,
    LINX_MSGB_CONTROL_SEND_EVENT,
    LINX_MSGB_CONTROL_WAIT_EVENT,
} LinxMsgbControlRequest;

typedef struct LinxMsgbGuestState {
    LinxMsgbGuestBank bank[LINX_MSGB_BANKS];
    uint8_t current_thread[LINX_MSGB_PE_COUNT];
    uint8_t config_selector[LINX_MSGB_PE_COUNT][LINX_MSGB_THREADS_PER_PE];
    bool invocation_active;
    uint64_t invocation;
    bool draining;
    LinxMsgbControlRequest control_request;
    uint64_t control_operand;
    uint64_t control_epoch;
    uint64_t next_execution_domain;
    bool last_done_valid;
    unsigned last_done_worker;
    uint64_t last_done_invocation;
    bool auto_launch_valid;
    bool auto_launch_managed;
    unsigned auto_launch_worker;
    uint64_t auto_launch_dispatch_id;
    uint64_t auto_launch_pc;
    uint64_t auto_launch_argument;
    uint32_t auto_launch_blocknum;
    uint32_t auto_launch_blockid;
} LinxMsgbGuestState;

typedef struct LinxMsgbAccessContext {
    unsigned pe;
    unsigned acr;
    uint64_t tpc;
    uint64_t execution_domain;
    bool atomic;
    bool far;
} LinxMsgbAccessContext;

typedef struct LinxMsgbAccessResult {
    bool handled;
    bool fault;
    bool bundle_fault;
    uint64_t value;
} LinxMsgbAccessResult;

void linx_msgb_guest_reset(LinxMsgbGuestState *state);
bool linx_msgb_guest_state_live(const LinxMsgbGuestState *state);
bool linx_msgb_guest_select_thread(LinxMsgbGuestState *state,
                                  unsigned pe, unsigned thread);
bool linx_msgb_guest_enabled_for_context(
    const LinxMsgbGuestState *state, const LinxMsgbAccessContext *context);
bool linx_msgb_guest_bind_invocation(LinxMsgbGuestState *state,
                                    uint64_t invocation);
bool linx_msgb_guest_clear_invocation(LinxMsgbGuestState *state,
                                     uint64_t invocation);
bool linx_msgb_guest_begin_drain(LinxMsgbGuestState *state,
                                uint64_t invocation);
bool linx_msgb_guest_abort_invocation(LinxMsgbGuestState *state,
                                     uint64_t invocation);
LinxMsgbAccessResult linx_msgb_guest_read(LinxMsgbGuestState *state,
                                         const LinxMsgbAccessContext *context,
                                         uint32_t address);
LinxMsgbAccessResult linx_msgb_guest_write(LinxMsgbGuestState *state,
                                          const LinxMsgbAccessContext *context,
                                          uint32_t address, uint64_t value);
LinxMsgbAccessResult linx_msgb_guest_swap(LinxMsgbGuestState *state,
                                         const LinxMsgbAccessContext *context,
                                         uint32_t address, uint64_t value);
LinxMsgbResult linx_msgb_guest_bse(LinxMsgbGuestState *state,
                                  const LinxMsgbAccessContext *context,
                                  uint64_t control);
void linx_msgb_guest_bwe(LinxMsgbGuestState *state,
                         const LinxMsgbAccessContext *context,
                         uint64_t operand);
LinxMsgbResult linx_msgb_hscb_issue_launch(LinxMsgbGuestState *state,
                                           unsigned worker_endpoint,
                                           uint64_t dispatch_id,
                                           uint64_t kernel_pc,
                                           uint64_t argument,
                                           uint32_t blocknum,
                                           uint32_t blockid);
LinxMsgbResult linx_msgb_hscb_receive_worker(LinxMsgbGuestState *state);
bool linx_msgb_hscb_accept_launch(LinxMsgbGuestState *state,
                                 unsigned worker_endpoint,
                                 uint64_t release_token,
                                 uint64_t *kernel_pc, uint64_t *argument,
                                 uint32_t *blocknum, uint32_t *blockid);
bool linx_msgb_hscb_schedule_launch(LinxMsgbGuestState *state,
                                   unsigned worker_endpoint,
                                   uint64_t dispatch_id,
                                   uint64_t kernel_pc,
                                   uint64_t argument,
                                   uint32_t blocknum,
                                   uint32_t blockid);
bool linx_msgb_hscb_take_launch(LinxMsgbGuestState *state,
                               unsigned worker_endpoint,
                               uint64_t *kernel_pc, uint64_t *argument,
                               uint32_t *blocknum, uint32_t *blockid);
bool linx_msgb_hscb_launch_pending(const LinxMsgbGuestState *state,
                                  unsigned worker_endpoint,
                                  uint64_t invocation,
                                  uint64_t *kernel_pc);
bool linx_msgb_hscb_geometry(const LinxMsgbGuestState *state,
                             const LinxMsgbAccessContext *context,
                             uint64_t *blocknum, uint64_t *blockid);

#endif
