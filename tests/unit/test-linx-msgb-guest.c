/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "../../target/linx/msgb_guest.h"

#define MSGB_D1 0x831u
#define MSGB_RXCLAIM 0x83bu
#define MSGB_RXRELEASE 0x841u
#define MSGB_ACK 0x842u
#define MSGB_CFG 0x84au
#define MSGB_DOMAIN 0x84bu

static LinxMsgbAccessContext context(unsigned pe, unsigned acr, uint64_t tpc,
                                     uint64_t domain)
{
    return (LinxMsgbAccessContext) {
        .pe = pe,
        .acr = acr,
        .tpc = tpc,
        .execution_domain = domain,
    };
}

static void configure(LinxMsgbGuestState *state, unsigned pe)
{
    LinxMsgbAccessContext ctx = context(pe, 0, 0x1000 + pe * 0x100, 1);
    uint64_t config = UINT64_C(1) | (UINT64_C(1) << 16) |
                      (UINT64_C(1) << 32);

    g_assert_false(linx_msgb_guest_write(state, &ctx, MSGB_DOMAIN, 1).fault);
    g_assert_false(linx_msgb_guest_write(state, &ctx, MSGB_CFG, config).fault);
}

static void test_guest_send_receive_replay(void)
{
    LinxMsgbGuestState state;
    LinxMsgbAccessContext source = context(0, 0, 0x1000, 7);
    LinxMsgbAccessContext target = context(1, 0, 0x1100, 8);
    uint64_t token;
    uint64_t value;

    linx_msgb_guest_reset(&state);
    g_assert_false(linx_msgb_guest_state_live(&state));
    configure(&state, 0);
    g_assert_true(linx_msgb_guest_state_live(&state));
    configure(&state, 1);
    g_assert_false(linx_msgb_guest_write(&state, &source, MSGB_D1,
                                         UINT64_C(0x123456789abcdef0)).fault);
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &source,
                                        (UINT64_C(1) << 32) | 16), ==,
                     LINX_MSGB_OK);
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &source,
                                        (UINT64_C(1) << 32) | 16), ==,
                     LINX_MSGB_DUPLICATE);
    g_assert_false(linx_msgb_guest_write(
        &state, &source, MSGB_CFG,
        UINT64_C(1) | (UINT64_C(2) << 16) | (UINT64_C(2) << 32)).fault);
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &source,
                                        (UINT64_C(1) << 32) | 16), ==,
                     LINX_MSGB_DUPLICATE);
    g_assert_false(linx_msgb_guest_write(&state, &target, MSGB_RXCLAIM, 1).fault);
    token = linx_msgb_receive_token(&state.bank[16].receive, 1);
    g_assert_true(linx_msgb_receive_read(&state.bank[16].receive, 1, 0,
                                         &value));
    g_assert_cmphex(value, ==, UINT64_C(0x123456789abcdef0));
    g_assert_false(linx_msgb_guest_write(&state, &target, MSGB_ACK, token).fault);
    g_assert_false(linx_msgb_guest_write(&state, &target, MSGB_RXRELEASE,
                                         token).fault);

    source.tpc += 4;
    source.execution_domain++;
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &source,
                                        (UINT64_C(1) << 32) | 16), ==,
                     LINX_MSGB_OK);
}

static void test_slot_and_invocation_ownership(void)
{
    LinxMsgbGuestState state;
    LinxMsgbAccessContext slot0 = context(0, 0, 0x1800, 1);
    LinxMsgbAccessContext slot1 = context(0, 0, 0x1804, 2);

    linx_msgb_guest_reset(&state);
    configure(&state, 0);
    g_assert_true(linx_msgb_guest_select_thread(&state, 0, 1));
    configure(&state, 0);
    g_assert_true(state.bank[0].enabled);
    g_assert_true(state.bank[64].enabled);
    linx_msgb_guest_write(&state, &slot1, MSGB_D1, 0xa5);
    g_assert_cmpuint(linx_msgb_guest_bse(
                         &state, &slot1, (UINT64_C(1) << 32) | 0), ==,
                     LINX_MSGB_OK);
    g_assert_true(state.bank[0].receive.queued.valid);
    g_assert_false(state.bank[64].receive.queued.valid);
    g_assert_false(linx_msgb_guest_clear_invocation(&state, 2));
    g_assert_false(linx_msgb_guest_clear_invocation(&state, 1));
    g_assert_true(linx_msgb_guest_begin_drain(&state, 1));
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &slot0, 0), ==,
                     LINX_MSGB_BAD_COMMAND);
    g_assert_true(linx_msgb_guest_abort_invocation(&state, 1));
    g_assert_false(state.bank[0].enabled);
    g_assert_false(state.bank[64].enabled);
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &slot0, 0), ==,
                     LINX_MSGB_NONE);

    linx_msgb_guest_reset(&state);
    configure(&state, 0);
    g_assert_true(linx_msgb_guest_clear_invocation(&state, 1));
    g_assert_false(state.invocation_active);
}

static void test_control_requests_and_preflight(void)
{
    LinxMsgbGuestState state;
    LinxMsgbAccessContext ctx = context(0, 0, 0x2000, 9);

    linx_msgb_guest_reset(&state);
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &ctx, 0x55), ==,
                     LINX_MSGB_NONE);
    g_assert_cmpuint(state.control_request, ==, LINX_MSGB_CONTROL_SEND_EVENT);
    g_assert_cmphex(state.control_operand, ==, 0x55);
    g_assert_cmpuint(state.control_epoch, ==, 1);
    linx_msgb_guest_bwe(&state, &ctx, 0xaa);
    g_assert_cmpuint(state.control_request, ==, LINX_MSGB_CONTROL_WAIT_EVENT);
    g_assert_cmphex(state.control_operand, ==, 0xaa);
    g_assert_cmpuint(state.control_epoch, ==, 2);

    configure(&state, 0);
    ctx.atomic = true;
    g_assert_true(linx_msgb_guest_write(&state, &ctx, MSGB_D1, 1).fault);
    g_assert_cmpuint(linx_msgb_guest_bse(&state, &ctx, 0), ==,
                     LINX_MSGB_BAD_COMMAND);
    ctx.atomic = false;
    ctx.acr = 1;
    g_assert_true(linx_msgb_guest_write(&state, &ctx, MSGB_DOMAIN, 1).fault);
    g_assert_true(linx_msgb_guest_swap(&state, &ctx, MSGB_D1, 1).fault);
}

static void read_ack_release(LinxMsgbGuestState *state,
                             LinxMsgbAccessContext *ctx, unsigned bank)
{
    uint64_t ignored;
    linx_msgb_guest_write(state, ctx, MSGB_RXCLAIM, 1);
    uint64_t token = linx_msgb_receive_token(&state->bank[bank].receive, 1);
    unsigned count = state->bank[bank].receive.snapshot.packet.word_count;
    for (unsigned i = 0; i < count; ++i) {
        g_assert_true(linx_msgb_receive_read(&state->bank[bank].receive, 1,
                                             i, &ignored));
    }
    linx_msgb_receive_ack(&state->bank[bank].receive, 1, token);
}

static void test_hscb_ready_launch_done(void)
{
    LinxMsgbGuestState state;
    LinxMsgbAccessContext worker = context(0, 0, 0x3000, 10);
    uint64_t kernel_pc, argument;
    uint32_t blocknum, blockid;

    linx_msgb_guest_reset(&state);
    configure(&state, 0);
    linx_msgb_guest_write(&state, &worker, MSGB_D1, 1);
    g_assert_cmpuint(linx_msgb_guest_bse(
                         &state, &worker,
                         (UINT64_C(1) << 32) | LINX_MSGB_HSCB_ENDPOINT), ==,
                     LINX_MSGB_OK);
    g_assert_cmpuint(linx_msgb_hscb_receive_worker(&state), ==, LINX_MSGB_OK);
    g_assert_true(state.bank[0].hscb_ready);

    g_assert_cmpuint(linx_msgb_hscb_issue_launch(&state, 0, 76, 0x4000,
                                                 0x5000, 3, 3), ==,
                     LINX_MSGB_BAD_COMMAND);
    g_assert_cmpuint(linx_msgb_hscb_issue_launch(&state, 0, 77, 0x4000,
                                                 0x5000, 8, 3), ==,
                     LINX_MSGB_OK);
    g_assert_cmpuint(linx_msgb_hscb_issue_launch(&state, 0, 78, 0x4000,
                                                 0x5000, 8, 3), ==,
                     LINX_MSGB_BAD_COMMAND);
    read_ack_release(&state, &worker, 0);
    g_assert_false(linx_msgb_hscb_accept_launch(
        &state, 0, state.bank[0].receive.snapshot.token + 1,
        &kernel_pc, &argument, &blocknum, &blockid));
    g_assert_true(linx_msgb_hscb_accept_launch(&state, 0,
                                               state.bank[0].receive.snapshot.token,
                                               &kernel_pc,
                                               &argument, &blocknum,
                                               &blockid));
    g_assert_cmphex(kernel_pc, ==, 0x4000);
    g_assert_cmphex(argument, ==, 0x5000);
    g_assert_cmpuint(blocknum, ==, 8);
    g_assert_cmpuint(blockid, ==, 3);
    uint64_t launch_token = linx_msgb_receive_token(&state.bank[0].receive, 1);
    linx_msgb_receive_release(&state.bank[0].receive, 1, launch_token);

    worker.tpc += 4;
    worker.execution_domain++;
    state.bank[0].staging[0] = 3;
    state.bank[0].staging[1] = 77;
    state.bank[0].staging[2] = 0x6000;
    for (unsigned i = 0; i < LINX_MSGB_RECEIPTS - 1; ++i) {
        state.bank[0].receipt[i].valid = true;
    }
    g_assert_true(linx_msgb_guest_begin_drain(&state, 1));
    g_assert_cmpuint(linx_msgb_guest_bse(
                         &state, &worker,
                         (UINT64_C(3) << 32) | LINX_MSGB_HSCB_ENDPOINT), ==,
                     LINX_MSGB_OK);
    g_assert_cmpuint(linx_msgb_hscb_receive_worker(&state), ==, LINX_MSGB_OK);
    g_assert_false(state.bank[0].hscb_running);
    g_assert_cmphex(state.bank[0].hscb_return_value, ==, 0x6000);
    g_assert_true(state.last_done_valid);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/msgb-guest/send-receive-replay",
                    test_guest_send_receive_replay);
    g_test_add_func("/linx/msgb-guest/control-preflight",
                    test_control_requests_and_preflight);
    g_test_add_func("/linx/msgb-guest/slot-invocation",
                    test_slot_and_invocation_ownership);
    g_test_add_func("/linx/msgb-guest/hscb-ready-launch-done",
                    test_hscb_ready_launch_done);
    return g_test_run();
}
