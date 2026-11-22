/*
 * PTO 0.59.1 Message Buffer guest-visible owner.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "msgb_guest.h"

#define MSGB_CR 0x0830u
#define MSGB_D1 0x0831u
#define MSGB_D10 0x083au
#define MSGB_RXCLAIM 0x083bu
#define MSGB_RXSTATE 0x083cu
#define MSGB_RXTOKEN 0x083du
#define MSGB_RXMSGID_HI 0x083eu
#define MSGB_RXMSGID_LO 0x083fu
#define MSGB_RXCONTROL 0x0840u
#define MSGB_RXRELEASE 0x0841u
#define MSGB_ACK 0x0842u
#define MSGB_ACKSTAT 0x0843u
#define MSGB_ACKID 0x0844u
#define MSGB_TXID_HI 0x0845u
#define MSGB_TXID_LO 0x0846u
#define MSGB_TXSTAT 0x0847u
#define MSGB_ENDPOINT 0x0848u
#define MSGB_CFGSEL 0x0849u
#define MSGB_CFG 0x084au
#define MSGB_DOMAIN 0x084bu

static LinxMsgbAccessResult result(bool fault, uint64_t value)
{
    return (LinxMsgbAccessResult) {
        .handled = true,
        .fault = fault,
        .value = value,
    };
}

static unsigned current_index(const LinxMsgbGuestState *state,
                              const LinxMsgbAccessContext *context)
{
    unsigned thread = state->current_thread[context->pe];
    unsigned worker = thread * LINX_MSGB_PE_COUNT + context->pe;

    return worker * LINX_MSGB_ACR_COUNT + context->acr;
}

static unsigned selected_index(const LinxMsgbGuestState *state,
                               const LinxMsgbAccessContext *context)
{
    unsigned thread = state->current_thread[context->pe];
    unsigned worker = thread * LINX_MSGB_PE_COUNT + context->pe;

    return worker * LINX_MSGB_ACR_COUNT +
           state->config_selector[context->pe][thread];
}

static bool config_address(uint32_t address)
{
    return address == MSGB_CFGSEL || address == MSGB_CFG ||
           address == MSGB_DOMAIN;
}

static bool data_address(uint32_t address)
{
    return address >= MSGB_D1 && address <= MSGB_D10;
}

static bool quiescent(const LinxMsgbGuestBank *bank)
{
    return !bank->receive.queued.valid && !bank->receive.snapshot.valid;
}

static bool current_owner(const LinxMsgbGuestState *state,
                          const LinxMsgbAccessContext *context,
                          const LinxMsgbGuestBank *bank)
{
    return state->invocation_active && state->invocation != 0 &&
           bank->enabled && bank->bound_invocation == state->invocation;
}

static bool effect_permitted(const LinxMsgbGuestState *state,
                             const LinxMsgbAccessContext *context,
                             const LinxMsgbGuestBank *bank,
                             bool require_enabled)
{
    if (!state->invocation_active || state->invocation == 0 ||
        (require_enabled && !current_owner(state, context, bank))) {
        return false;
    }
    return !context->atomic && !context->far;
}

static void clear_invocation_state(LinxMsgbGuestBank *bank)
{
    memset(&bank->receive.queued, 0, sizeof(bank->receive.queued));
    memset(&bank->receive.snapshot, 0, sizeof(bank->receive.snapshot));
    memset(bank->staging, 0, sizeof(bank->staging));
    memset(bank->receipt, 0, sizeof(bank->receipt));
    bank->receive.rx_result = LINX_MSGB_NONE;
    bank->receive.ack_result = LINX_MSGB_NONE;
    bank->receive.ack_result_valid = false;
    bank->receive.ack_id = 0;
    bank->receive.last_success_valid = false;
    bank->receive.last_success_token = 0;
    bank->last_accepted_control = 0;
    bank->tx_message_id_hi = 0;
    bank->tx_message_id_lo = 0;
    bank->tx_result = LINX_MSGB_NONE;
    bank->tx_result_valid = false;
    bank->hscb_ready = false;
    bank->hscb_waiting = false;
    bank->hscb_running = false;
    bank->hscb_dispatch_id = 0;
    bank->hscb_launch_message_id_hi = 0;
    bank->hscb_launch_message_id_lo = 0;
    bank->hscb_blocknum = 0;
    bank->hscb_blockid = 0;
    bank->hscb_return_value = 0;
    bank->hscb_issued_count = 0;
    memset(bank->hscb_issued_id, 0, sizeof(bank->hscb_issued_id));
}

void linx_msgb_guest_reset(LinxMsgbGuestState *state)
{
    memset(state, 0, sizeof(*state));
    state->invocation_active = true;
    state->invocation = 1;
    state->next_execution_domain = 1;
    linx_msgb_receive_init(&state->bank[LINX_MSGB_HSCB_BANK].receive, 1);
    state->bank[LINX_MSGB_HSCB_BANK].enabled = true;
    state->bank[LINX_MSGB_HSCB_BANK].domain_bound = true;
    state->bank[LINX_MSGB_HSCB_BANK].domain = 1;
    state->bank[LINX_MSGB_HSCB_BANK].bound_invocation = 1;
    state->bank[LINX_MSGB_HSCB_BANK].source_epoch = 1;
}

bool linx_msgb_guest_state_live(const LinxMsgbGuestState *state)
{
    if (state->control_epoch || state->draining || state->last_done_valid ||
        state->invocation != 1 || !state->invocation_active) {
        return true;
    }
    for (unsigned i = 0; i < LINX_MSGB_ENDPOINTS; ++i) {
        const LinxMsgbGuestBank *bank = &state->bank[i];
        if (bank->enabled || bank->domain_bound || bank->receive.queued.valid ||
            bank->receive.snapshot.valid || bank->tx_result_valid ||
            bank->hscb_ready || bank->hscb_running || bank->hscb_waiting) {
            return true;
        }
    }
    const LinxMsgbGuestBank *host = &state->bank[LINX_MSGB_HSCB_BANK];
    return !host->enabled || host->domain != 1 ||
           host->receive.queued.valid || host->receive.snapshot.valid ||
           host->transmit_sequence != 0;
}

bool linx_msgb_guest_select_thread(LinxMsgbGuestState *state,
                                  unsigned pe, unsigned thread)
{
    if (pe >= LINX_MSGB_PE_COUNT || thread >= LINX_MSGB_THREADS_PER_PE) {
        return false;
    }
    state->current_thread[pe] = thread;
    return true;
}

bool linx_msgb_guest_bind_invocation(LinxMsgbGuestState *state,
                                    uint64_t invocation)
{
    if (invocation == 0 || (state->invocation_active &&
                            state->invocation != invocation)) {
        return false;
    }
    state->invocation_active = true;
    state->invocation = invocation;
    return true;
}

bool linx_msgb_guest_clear_invocation(LinxMsgbGuestState *state,
                                     uint64_t invocation)
{
    if (!state->invocation_active || invocation == 0 ||
        state->invocation != invocation) {
        return false;
    }
    for (unsigned i = 0; i < LINX_MSGB_ENDPOINTS; ++i) {
        LinxMsgbGuestBank *bank = &state->bank[i];
        if (bank->bound_invocation == invocation &&
            (!quiescent(bank) || bank->hscb_running)) {
            return false;
        }
    }
    for (unsigned i = 0; i < LINX_MSGB_ENDPOINTS; ++i) {
        if (state->bank[i].bound_invocation == invocation) {
            clear_invocation_state(&state->bank[i]);
            state->bank[i].enabled = false;
            state->bank[i].bound_invocation = 0;
        }
    }
    state->invocation_active = false;
    state->invocation = 0;
    state->draining = false;
    return true;
}

bool linx_msgb_guest_begin_drain(LinxMsgbGuestState *state,
                                uint64_t invocation)
{
    if (!state->invocation_active || invocation == 0 ||
        state->invocation != invocation) {
        return false;
    }
    state->draining = true;
    return true;
}

bool linx_msgb_guest_abort_invocation(LinxMsgbGuestState *state,
                                     uint64_t invocation)
{
    if (!state->invocation_active || invocation == 0 ||
        state->invocation != invocation) {
        return false;
    }
    for (unsigned i = 0; i < LINX_MSGB_ENDPOINTS; ++i) {
        if (state->bank[i].bound_invocation == invocation) {
            clear_invocation_state(&state->bank[i]);
            state->bank[i].enabled = false;
            state->bank[i].bound_invocation = 0;
        }
    }
    state->invocation_active = false;
    state->invocation = 0;
    state->draining = false;
    return true;
}

static LinxMsgbAccessResult read_operational(LinxMsgbGuestState *state,
                                            const LinxMsgbAccessContext *context,
                                            uint32_t address,
                                            LinxMsgbGuestBank *bank,
                                            unsigned index)
{
    uint64_t invocation = state->invocation;
    uint64_t value;

    if (!effect_permitted(state, context, bank, true)) {
        return result(true, 0);
    }
    if (address == MSGB_ENDPOINT) {
        return result(false, index);
    }
    if (address == MSGB_CR) {
        return result(false, bank->last_accepted_control);
    }
    if (data_address(address)) {
        if (!linx_msgb_receive_read(&bank->receive, invocation,
                                    address - MSGB_D1, &value)) {
            return result(true, 0);
        }
        return result(false, value);
    }
    switch (address) {
    case MSGB_RXSTATE:
        return result(false, linx_msgb_receive_rxstate(&bank->receive,
                                                       invocation));
    case MSGB_RXTOKEN:
        return result(false, linx_msgb_receive_token(&bank->receive,
                                                     invocation));
    case MSGB_RXMSGID_HI:
    case MSGB_RXMSGID_LO:
    case MSGB_RXCONTROL:
        if (!bank->receive.snapshot.valid ||
            bank->receive.snapshot.owner_invocation != invocation) {
            return result(true, 0);
        }
        if (address == MSGB_RXMSGID_HI) {
            value = bank->receive.snapshot.packet.message_id_hi;
        } else if (address == MSGB_RXMSGID_LO) {
            value = bank->receive.snapshot.packet.message_id_lo;
        } else {
            value = bank->receive.snapshot.packet.control;
        }
        return result(false, value);
    case MSGB_ACKSTAT:
        return result(false, linx_msgb_receive_ackstat(&bank->receive));
    case MSGB_ACKID:
        return result(false, linx_msgb_receive_ackid(&bank->receive));
    case MSGB_TXID_HI:
        return result(false, bank->tx_message_id_hi);
    case MSGB_TXID_LO:
        return result(false, bank->tx_message_id_lo);
    case MSGB_TXSTAT:
        value = bank->tx_result | ((uint64_t)bank->tx_result_valid << 8);
        return result(false, value);
    default:
        return result(true, 0);
    }
}

LinxMsgbAccessResult linx_msgb_guest_read(LinxMsgbGuestState *state,
                                         const LinxMsgbAccessContext *context,
                                         uint32_t address)
{
    unsigned index;
    LinxMsgbGuestBank *bank;

    if (address < MSGB_CR || address > MSGB_DOMAIN ||
        context->pe >= LINX_MSGB_PE_COUNT ||
        context->acr >= LINX_MSGB_ACR_COUNT) {
        return (LinxMsgbAccessResult) { 0 };
    }
    if (config_address(address)) {
        if (context->acr != 0) {
            return result(true, 0);
        }
        if (address == MSGB_CFGSEL) {
            unsigned thread = state->current_thread[context->pe];
            return result(false,
                          state->config_selector[context->pe][thread]);
        }
        index = selected_index(state, context);
        bank = &state->bank[index];
        if (address == MSGB_DOMAIN) {
            return result(false, bank->domain);
        }
        return result(false, (uint64_t)bank->enabled |
                             ((uint64_t)bank->receive.epoch << 16) |
                             ((uint64_t)bank->source_epoch << 32));
    }
    index = current_index(state, context);
    return read_operational(state, context, address, &state->bank[index],
                            index);
}

static bool configure_bank(LinxMsgbGuestState *state,
                           LinxMsgbGuestBank *bank, uint64_t value)
{
    uint16_t rx = value >> 16;
    uint32_t source = value >> 32;
    bool enable = value & 1;

    if (value & UINT64_C(0x000000000000fffe)) {
        return false;
    }
    if (!enable) {
        if (value != 0 || !quiescent(bank) || bank->hscb_running) {
            return false;
        }
        bank->enabled = false;
        bank->hscb_waiting = false;
        return true;
    }
    if (!state->invocation_active || state->invocation == 0 || rx == 0 ||
        source == 0 || !quiescent(bank) || !bank->domain_bound) {
        return false;
    }
    bool first = bank->receive.epoch == 0 && bank->source_epoch == 0;
    bool same = rx == bank->receive.epoch && source == bank->source_epoch;
    bool advance = bank->receive.epoch != UINT16_MAX &&
                   bank->source_epoch != UINT32_MAX &&
                   rx == bank->receive.epoch + 1 &&
                   source == bank->source_epoch + 1;
    if (!first && !same && !advance) {
        return false;
    }
    if (first || advance) {
        linx_msgb_receive_init(&bank->receive, rx);
        bank->source_epoch = source;
        bank->transmit_sequence = 0;
    }
    if (bank->bound_invocation != state->invocation) {
        clear_invocation_state(bank);
    }
    bank->bound_invocation = state->invocation;
    bank->enabled = true;
    return true;
}

LinxMsgbAccessResult linx_msgb_guest_write(LinxMsgbGuestState *state,
                                          const LinxMsgbAccessContext *context,
                                          uint32_t address, uint64_t value)
{
    unsigned index;
    LinxMsgbGuestBank *bank;

    if (address < MSGB_CR || address > MSGB_DOMAIN ||
        context->pe >= LINX_MSGB_PE_COUNT ||
        context->acr >= LINX_MSGB_ACR_COUNT) {
        return (LinxMsgbAccessResult) { 0 };
    }
    if (config_address(address)) {
        if (context->acr != 0 || context->atomic || context->far ||
            !state->invocation_active) {
            return result(true, 0);
        }
        if (address == MSGB_CFGSEL) {
            unsigned thread = state->current_thread[context->pe];
            if (value >= LINX_MSGB_ACR_COUNT) {
                return result(true, 0);
            }
            state->config_selector[context->pe][thread] = value;
            return result(false, 0);
        }
        index = selected_index(state, context);
        bank = &state->bank[index];
        if (address == MSGB_DOMAIN) {
            if (bank->enabled || !quiescent(bank) ||
                (bank->domain_bound && bank->domain != value)) {
                return result(true, 0);
            }
            bank->domain = value;
            bank->domain_bound = true;
            return result(false, 0);
        }
        return result(!configure_bank(state, bank, value), 0);
    }
    index = current_index(state, context);
    bank = &state->bank[index];
    if (!effect_permitted(state, context, bank, true)) {
        return result(true, 0);
    }
    if (data_address(address)) {
        bank->staging[address - MSGB_D1] = value;
        return result(false, 0);
    }
    switch (address) {
    case MSGB_RXCLAIM:
        linx_msgb_receive_claim(&bank->receive, state->invocation, value);
        return result(false, 0);
    case MSGB_RXRELEASE:
        linx_msgb_receive_release(&bank->receive, state->invocation, value);
        return result(false, 0);
    case MSGB_ACK:
        linx_msgb_receive_ack(&bank->receive, state->invocation, value);
        return result(false, 0);
    default:
        return result(true, 0);
    }
}

LinxMsgbAccessResult linx_msgb_guest_swap(LinxMsgbGuestState *state,
                                         const LinxMsgbAccessContext *context,
                                         uint32_t address, uint64_t value)
{
    (void)state;
    (void)context;
    (void)value;
    if (address >= MSGB_CR && address <= MSGB_DOMAIN) {
        return result(true, 0);
    }
    return (LinxMsgbAccessResult) { 0 };
}

static int find_receipt(const LinxMsgbGuestBank *bank,
                        const LinxMsgbGuestState *state,
                        const LinxMsgbAccessContext *context)
{
    for (unsigned i = 0; i < LINX_MSGB_RECEIPTS; ++i) {
        const LinxMsgbReplayReceipt *receipt = &bank->receipt[i];
        if (receipt->valid && receipt->invocation == state->invocation &&
            receipt->acr == context->acr &&
            receipt->execution_domain == context->execution_domain &&
            receipt->tpc == context->tpc) {
            return i;
        }
    }
    return -1;
}

static int free_receipt(const LinxMsgbGuestBank *bank, bool reserve_one)
{
    bool skip = reserve_one;
    for (unsigned i = 0; i < LINX_MSGB_RECEIPTS; ++i) {
        if (!bank->receipt[i].valid) {
            if (!skip) {
                return i;
            }
            skip = false;
        }
    }
    return -1;
}

static bool matching_done(const LinxMsgbGuestBank *source, uint64_t control)
{
    return source->hscb_running &&
           control == ((UINT64_C(3) << 32) |
                       LINX_MSGB_HSCB_ENDPOINT) &&
           source->staging[0] == 3 &&
           source->staging[1] == source->hscb_dispatch_id;
}

static LinxMsgbResult admit(LinxMsgbGuestState *state, unsigned source_index,
                            uint64_t control)
{
    uint64_t destination = (uint32_t)control;
    unsigned count = (control >> 32) & 0xf;
    unsigned target_index;
    LinxMsgbGuestBank *source = &state->bank[source_index];
    LinxMsgbGuestBank *target;
    LinxMsgbPacket packet = { 0 };

    if (control >> 36 || count > LINX_MSGB_WORDS ||
        (destination >= LINX_MSGB_ENDPOINTS &&
         destination != LINX_MSGB_HSCB_ENDPOINT)) {
        return LINX_MSGB_BAD_COMMAND;
    }
    target_index = destination == LINX_MSGB_HSCB_ENDPOINT ?
                   LINX_MSGB_HSCB_BANK : destination;
    target = &state->bank[target_index];
    if (!source->enabled || !target->enabled ||
        source->domain != target->domain) {
        return LINX_MSGB_BAD_COMMAND;
    }
    if (target_index != LINX_MSGB_HSCB_BANK &&
        (!state->invocation_active ||
         target->bound_invocation != state->invocation)) {
        return LINX_MSGB_BAD_COMMAND;
    }
    bool done = matching_done(source, control);
    if (target_index == LINX_MSGB_HSCB_BANK) {
        uint64_t remaining = LINX_MSGB_CLAIM_SEQUENCE_MAX -
                             target->receive.claim_sequence;
        unsigned outstanding = 0;
        for (unsigned i = 0; i < LINX_MSGB_ENDPOINTS;
             i += LINX_MSGB_ACR_COUNT) {
            outstanding += state->bank[i].hscb_running;
        }
        if (remaining == 0 || (!done && remaining <= outstanding)) {
            return LINX_MSGB_EXHAUSTED;
        }
    }
    if (source->transmit_sequence == UINT64_MAX ||
        (!done && source->hscb_running &&
         source->transmit_sequence == UINT64_MAX - 1)) {
        return LINX_MSGB_EXHAUSTED;
    }
    packet.source_invocation = source->bound_invocation;
    packet.destination_invocation = target->bound_invocation;
    packet.message_id_hi = ((uint64_t)source_index << 32) |
                           source->source_epoch;
    packet.message_id_lo = source->transmit_sequence + 1;
    packet.control = control;
    packet.word_count = count;
    memcpy(packet.words, source->staging, count * sizeof(uint64_t));
    LinxMsgbResult receive = linx_msgb_receive_enqueue(&target->receive,
                                                       &packet);
    if (receive != LINX_MSGB_OK) {
        return receive;
    }
    source->transmit_sequence = packet.message_id_lo;
    source->last_accepted_control = control;
    source->tx_message_id_hi = packet.message_id_hi;
    source->tx_message_id_lo = packet.message_id_lo;
    return LINX_MSGB_OK;
}

LinxMsgbResult linx_msgb_guest_bse(LinxMsgbGuestState *state,
                                  const LinxMsgbAccessContext *context,
                                  uint64_t control)
{
    unsigned source_index = current_index(state, context);
    LinxMsgbGuestBank *source = &state->bank[source_index];
    int receipt_index;
    bool done;

    if (!current_owner(state, context, source)) {
        state->control_request = LINX_MSGB_CONTROL_SEND_EVENT;
        state->control_operand = control;
        state->control_epoch++;
        return LINX_MSGB_NONE;
    }
    if (!effect_permitted(state, context, source, true)) {
        return LINX_MSGB_BAD_COMMAND;
    }
    done = matching_done(source, control);
    receipt_index = find_receipt(source, state, context);
    if (receipt_index >= 0) {
        LinxMsgbReplayReceipt *receipt = &source->receipt[receipt_index];
        source->tx_result = receipt->control == control ?
                            LINX_MSGB_DUPLICATE : LINX_MSGB_BAD_COMMAND;
        source->tx_result_valid = true;
        if (receipt->control == control) {
            source->last_accepted_control = control;
            source->tx_message_id_hi = receipt->message_id_hi;
            source->tx_message_id_lo = receipt->message_id_lo;
        }
        return source->tx_result;
    }
    receipt_index = free_receipt(source, source->hscb_running && !done);
    LinxMsgbResult send;
    if (receipt_index < 0) {
        send = LINX_MSGB_BUSY;
    } else if (state->draining && !done) {
        send = LINX_MSGB_BAD_COMMAND;
    } else {
        send = admit(state, source_index, control);
    }
    if (send == LINX_MSGB_OK) {
        LinxMsgbReplayReceipt *receipt = &source->receipt[receipt_index];
        receipt->valid = true;
        receipt->invocation = state->invocation;
        receipt->acr = context->acr;
        receipt->execution_domain = context->execution_domain;
        receipt->tpc = context->tpc;
        receipt->control = control;
        receipt->message_id_hi = source->tx_message_id_hi;
        receipt->message_id_lo = source->tx_message_id_lo;
    }
    source->tx_result = send;
    source->tx_result_valid = true;
    return send;
}

void linx_msgb_guest_bwe(LinxMsgbGuestState *state,
                         const LinxMsgbAccessContext *context,
                         uint64_t operand)
{
    state->control_request = LINX_MSGB_CONTROL_WAIT_EVENT;
    state->control_operand = operand;
    state->control_epoch++;
    unsigned index = current_index(state, context);
    LinxMsgbGuestBank *bank = &state->bank[index];
    if (current_owner(state, context, bank)) {
        bank->hscb_waiting = true;
    }
}

LinxMsgbResult linx_msgb_hscb_issue_launch(LinxMsgbGuestState *state,
                                           unsigned worker_endpoint,
                                           uint64_t dispatch_id,
                                           uint64_t kernel_pc,
                                           uint64_t argument,
                                           uint32_t blocknum,
                                           uint32_t blockid)
{
    LinxMsgbGuestBank *host = &state->bank[LINX_MSGB_HSCB_BANK];
    LinxMsgbGuestBank *worker;
    uint64_t control;

    if (worker_endpoint >= LINX_MSGB_ENDPOINTS ||
        worker_endpoint % LINX_MSGB_ACR_COUNT != 0 || blocknum == 0 ||
        blockid >= blocknum || kernel_pc == 0 || (kernel_pc & 1)) {
        return LINX_MSGB_BAD_COMMAND;
    }
    worker = &state->bank[worker_endpoint];
    if (!worker->enabled || !worker->hscb_ready ||
        worker->bound_invocation != state->invocation) {
        return LINX_MSGB_BAD_COMMAND;
    }
    if (worker->hscb_running ||
        !quiescent(host) || free_receipt(worker, false) < 0) {
        return LINX_MSGB_BUSY;
    }
    if (worker->hscb_issued_count == LINX_MSGB_RECEIPTS ||
        worker->transmit_sequence == UINT64_MAX ||
        worker->receive.claim_sequence == LINX_MSGB_CLAIM_SEQUENCE_MAX) {
        return LINX_MSGB_EXHAUSTED;
    }
    for (unsigned i = 0; i < worker->hscb_issued_count; ++i) {
        if (worker->hscb_issued_id[i] == dispatch_id) {
            return LINX_MSGB_BAD_COMMAND;
        }
    }
    host->staging[0] = 2;
    host->staging[1] = dispatch_id;
    host->staging[2] = kernel_pc;
    host->staging[3] = argument;
    control = ((uint64_t)4 << 32) | worker_endpoint;
    LinxMsgbResult status = admit(state, LINX_MSGB_HSCB_BANK, control);
    if (status == LINX_MSGB_OK) {
        worker->hscb_issued_id[worker->hscb_issued_count++] = dispatch_id;
        worker->hscb_running = true;
        worker->hscb_ready = false;
        worker->hscb_dispatch_id = dispatch_id;
        worker->hscb_launch_message_id_hi = host->tx_message_id_hi;
        worker->hscb_launch_message_id_lo = host->tx_message_id_lo;
        worker->hscb_blocknum = blocknum;
        worker->hscb_blockid = blockid;
    }
    return status;
}

LinxMsgbResult linx_msgb_hscb_receive_worker(LinxMsgbGuestState *state)
{
    LinxMsgbGuestBank *host = &state->bank[LINX_MSGB_HSCB_BANK];
    uint64_t token;
    uint64_t payload[LINX_MSGB_WORDS] = { 0 };
    LinxMsgbPacket packet;

    linx_msgb_receive_claim(&host->receive, 1, 1);
    if (host->receive.rx_result != LINX_MSGB_OK) {
        return host->receive.rx_result;
    }
    token = linx_msgb_receive_token(&host->receive, 1);
    unsigned count = host->receive.snapshot.packet.word_count;
    packet = host->receive.snapshot.packet;
    for (unsigned i = 0; i < count; ++i) {
        if (!linx_msgb_receive_read(&host->receive, 1, i, &payload[i])) {
            return LINX_MSGB_IO_ERROR;
        }
    }
    unsigned source = packet.message_id_hi >> 32;
    linx_msgb_receive_ack(&host->receive, 1, token);
    if (host->receive.ack_result != LINX_MSGB_OK) {
        return host->receive.ack_result;
    }
    linx_msgb_receive_release(&host->receive, 1, token);
    if (host->receive.rx_result != LINX_MSGB_OK) {
        return host->receive.rx_result;
    }
    if (source >= LINX_MSGB_ENDPOINTS || source % LINX_MSGB_ACR_COUNT != 0) {
        return LINX_MSGB_BAD_COMMAND;
    }
    LinxMsgbGuestBank *worker = &state->bank[source];
    if (!worker->enabled || worker->bound_invocation !=
                            packet.source_invocation) {
        return LINX_MSGB_NOT_OWNER;
    }
    if (count == 1 && payload[0] == 1 && !worker->hscb_running) {
        worker->hscb_ready = true;
    } else if (count == 3 && payload[0] == 3 && worker->hscb_running &&
               worker->hscb_dispatch_id == payload[1]) {
        worker->hscb_running = false;
        worker->hscb_return_value = payload[2];
        worker->hscb_launch_message_id_hi = 0;
        worker->hscb_launch_message_id_lo = 0;
        worker->hscb_blocknum = 0;
        worker->hscb_blockid = 0;
        state->last_done_valid = true;
        state->last_done_worker = source;
        state->last_done_invocation = packet.source_invocation;
    } else {
        return LINX_MSGB_BAD_COMMAND;
    }
    return LINX_MSGB_OK;
}

bool linx_msgb_hscb_accept_launch(LinxMsgbGuestState *state,
                                 unsigned worker_endpoint,
                                 uint64_t release_token,
                                 uint64_t *kernel_pc, uint64_t *argument,
                                 uint32_t *blocknum, uint32_t *blockid)
{
    if (worker_endpoint >= LINX_MSGB_ENDPOINTS) {
        return false;
    }
    LinxMsgbGuestBank *worker = &state->bank[worker_endpoint];
    LinxMsgbSnapshot *snapshot = &worker->receive.snapshot;
    if (!worker->hscb_running || !snapshot->valid || !snapshot->acked ||
        snapshot->token != release_token ||
        snapshot->packet.message_id_hi != worker->hscb_launch_message_id_hi ||
        snapshot->packet.message_id_lo != worker->hscb_launch_message_id_lo ||
        snapshot->packet.word_count != 4 || snapshot->packet.words[0] != 2 ||
        snapshot->packet.words[1] != worker->hscb_dispatch_id) {
        return false;
    }
    *kernel_pc = snapshot->packet.words[2];
    *argument = snapshot->packet.words[3];
    *blocknum = worker->hscb_blocknum;
    *blockid = worker->hscb_blockid;
    return true;
}
