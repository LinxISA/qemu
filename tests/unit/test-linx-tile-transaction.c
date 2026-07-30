/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "target/linx/cpu.h"
#include "target/linx/tile_isa_057.h"
#include "target/linx/tile_tepl_preflight.h"
#include "target/linx/tile_transaction.h"

typedef struct VisibleTileState {
    uint8_t hand_reserved[4];
    uint16_t pin_owner[32];
    uint32_t acc_bytes;
    uint32_t acc_head[16];
    uint32_t tile_bytes[32];
    uint32_t tile_head[32];
} VisibleTileState;

typedef struct VisibleCPUTileState {
    uint8_t hand_live[LINX_TILE_HAND_COUNT];
    uint8_t hand_reserved[LINX_TILE_HAND_COUNT];
    uint8_t hand_order[LINX_TILE_HAND_COUNT][LINX_TILE_HAND_DEPTH];
    uint8_t hand_count[LINX_TILE_HAND_COUNT];
    uint16_t pin_owner[LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH];
    uint8_t acc_carrier_valid;
    uint8_t acc_carrier;
    uint8_t acc_sources_valid;
    uint8_t acc_src0;
    uint8_t acc_src1;
    uint32_t acc[LINX_TILE_MAX_WORDS];
    uint32_t acc_bytes;
    uint8_t acc_dtype;
    uint8_t acc_valid;
    uint16_t acc_cols;
    uint16_t acc_rows;
    uint32_t tile[32][LINX_TILE_MAX_WORDS];
    uint32_t tile_capacity[32];
    uint32_t tile_bytes[32];
    uint8_t tile_elem_bytes[32];
    uint8_t tile_dtype[32];
    uint16_t tile_valid_cols[32];
    uint16_t tile_valid_rows[32];
    uint16_t tile_cols[32];
    uint16_t tile_rows[32];
} VisibleCPUTileState;

static void capture_cpu_visible_state(const CPULinxState *env,
                                      VisibleCPUTileState *visible)
{
    memcpy(visible->hand_live, env->tile_hand_live,
           sizeof(visible->hand_live));
    memcpy(visible->hand_reserved, env->tile_hand_reserved,
           sizeof(visible->hand_reserved));
    memcpy(visible->hand_order, env->tile_hand_order,
           sizeof(visible->hand_order));
    memcpy(visible->hand_count, env->tile_hand_count,
           sizeof(visible->hand_count));
    memcpy(visible->pin_owner, env->tile_pin_owner,
           sizeof(visible->pin_owner));
    visible->acc_carrier_valid = env->tile_acc_carrier_valid;
    visible->acc_carrier = env->tile_acc_carrier;
    visible->acc_sources_valid = env->tile_acc_sources_valid;
    visible->acc_src0 = env->tile_acc_src0;
    visible->acc_src1 = env->tile_acc_src1;
    memcpy(visible->acc, env->tile_acc, sizeof(visible->acc));
    visible->acc_bytes = env->tile_acc_bytes;
    visible->acc_dtype = env->tile_acc_dtype;
    visible->acc_valid = env->tile_acc_valid;
    visible->acc_cols = env->tile_acc_cols;
    visible->acc_rows = env->tile_acc_rows;
    memcpy(visible->tile, env->tile_reg, sizeof(visible->tile));
    memcpy(visible->tile_capacity, env->tile_reg_capacity,
           sizeof(visible->tile_capacity));
    memcpy(visible->tile_bytes, env->tile_reg_bytes,
           sizeof(visible->tile_bytes));
    memcpy(visible->tile_elem_bytes, env->tile_reg_elem_bytes,
           sizeof(visible->tile_elem_bytes));
    memcpy(visible->tile_dtype, env->tile_reg_dtype,
           sizeof(visible->tile_dtype));
    memcpy(visible->tile_valid_cols, env->tile_reg_valid_cols,
           sizeof(visible->tile_valid_cols));
    memcpy(visible->tile_valid_rows, env->tile_reg_valid_rows,
           sizeof(visible->tile_valid_rows));
    memcpy(visible->tile_cols, env->tile_reg_cols,
           sizeof(visible->tile_cols));
    memcpy(visible->tile_rows, env->tile_reg_rows,
           sizeof(visible->tile_rows));
}

static CPULinxState *new_atomicity_env(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);

    env->tile_hand_live[0] = 0x3u;
    env->tile_hand_reserved[0] = 0x8u;
    env->tile_hand_order[0][0] = 1u;
    env->tile_hand_order[0][1] = 0u;
    env->tile_hand_count[0] = 2u;
    env->tile_pin_owner[0] = 0x1u;
    env->tile_acc_carrier_valid = 1u;
    env->tile_acc_carrier = 1u;
    env->tile_acc_sources_valid = 1u;
    env->tile_acc_src0 = 0u;
    env->tile_acc_src1 = 1u;
    env->tile_acc_bytes = 128u;
    env->tile_acc_dtype = 16u;
    env->tile_acc_valid = 1u;
    env->tile_acc_cols = 4u;
    env->tile_acc_rows = 2u;
    env->tile_acc[0] = 0xacc0571u;
    env->tile_reg_capacity[3] = 128u;
    env->tile_reg_bytes[3] = 128u;
    env->tile_reg_elem_bytes[3] = 4u;
    env->tile_reg_dtype[3] = 17u;
    env->tile_reg_valid_cols[3] = 4u;
    env->tile_reg_valid_rows[3] = 2u;
    env->tile_reg_cols[3] = 4u;
    env->tile_reg_rows[3] = 8u;
    env->tile_reg[3][0] = 0x5710571u;
    return env;
}

static void assert_cpu_visible_state_unchanged(
    const CPULinxState *env, const VisibleCPUTileState *before)
{
    VisibleCPUTileState *after = g_new(VisibleCPUTileState, 1);

    capture_cpu_visible_state(env, after);
    g_assert_cmpmem(after, sizeof(*after), before, sizeof(*before));
    g_free(after);
}

static bool mutate_visible_state(void *opaque)
{
    VisibleTileState *state = opaque;

    state->hand_reserved[0] |= 1u;
    state->pin_owner[3] = 1u;
    state->acc_bytes = 256u;
    state->acc_head[0] = 0xacc0571u;
    state->tile_bytes[3] = 256u;
    state->tile_head[3] = 0x5710571u;
    return true;
}

static bool mutate_cpu_visible_state(void *opaque)
{
    CPULinxState *env = opaque;

    env->tile_hand_live[0] = 0xffu;
    env->tile_hand_reserved[0] = 0xffu;
    env->tile_pin_owner[0] = 0xffffu;
    env->tile_acc[0] = 0xdeadc0deu;
    env->tile_acc_bytes = 256u;
    env->tile_reg[3][0] = 0xbad0571u;
    env->tile_reg_bytes[3] = 256u;
    return true;
}

static void assert_rejected_without_state_change(const LinxTileTxnGate *gate,
                                                  LinxTileTxnFault expected)
{
    VisibleTileState state = {
        .hand_reserved = { 0x2u, 0x4u, 0x1u, 0x8u },
        .pin_owner = { [3] = 0x20u },
        .acc_bytes = 128u,
        .acc_head = { 0x11111111u, 0x22222222u },
        .tile_bytes = { [3] = 128u },
        .tile_head = { [3] = 0x33333333u },
    };
    const VisibleTileState before = state;

    g_assert_cmpint(linx_tile_txn_guarded_apply(
                        gate, mutate_visible_state, &state), ==, expected);
    g_assert_cmpmem(&state, sizeof(state), &before, sizeof(before));
}

static void test_invalid_datr_is_atomic(void)
{
    const LinxTileTxnGate gate = { false, true, true };
    assert_rejected_without_state_change(&gate, LINX_TILE_TXN_ILLEGAL);
}

static void test_invalid_cube_operand_is_atomic(void)
{
    const LinxTileTxnGate gate = { true, false, true };
    assert_rejected_without_state_change(&gate, LINX_TILE_TXN_ILLEGAL);
}

static void test_allocation_failure_is_atomic(void)
{
    const LinxTileTxnGate gate = { true, true, false };
    assert_rejected_without_state_change(&gate, LINX_TILE_TXN_ALLOCATION);
}

static void test_valid_transaction_applies_once(void)
{
    const LinxTileTxnGate gate = { true, true, true };
    VisibleTileState state = { 0 };

    g_assert_cmpint(linx_tile_txn_guarded_apply(
                        &gate, mutate_visible_state, &state), ==,
                    LINX_TILE_TXN_OK);
    g_assert_cmpuint(state.acc_bytes, ==, 256u);
    g_assert_cmphex(state.tile_head[3], ==, 0x5710571u);
}

static void test_tstore_size_comes_from_source_footprint(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);
    LinxTileIOTDesc desc = linx_tile_decode_iot(UINT64_C(0x00028000));
    unsigned tile = 0u;
    unsigned size_code = 0u;

    env->tile_iot_src_valid[0] = 1u;
    env->tile_iot_src_phys[0][0] = 3u;
    for (unsigned expected = 3u; expected <= 9u; expected++) {
        const uint32_t bytes = UINT32_C(1) << (expected + 4u);

        env->tile_reg_bytes[3] = bytes;
        g_assert_true(linx_tile_tstore_resolve_binding(
            &desc, env->tile_iot_src_valid[0], env->tile_iot_src_phys[0],
            env->tile_reg_bytes, &tile, &size_code));
        g_assert_cmpuint(tile, ==, 3u);
        g_assert_cmpuint(size_code, ==, expected);
    }
    const uint32_t invalid_bytes[] = { 0u, 64u, 96u, 16384u };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid_bytes); i++) {
        env->tile_reg_bytes[3] = invalid_bytes[i];
        g_assert_false(linx_tile_tstore_resolve_binding(
            &desc, env->tile_iot_src_valid[0], env->tile_iot_src_phys[0],
            env->tile_reg_bytes, &tile, &size_code));
    }
    desc.has_size = true;
    env->tile_reg_bytes[3] = 4096u;
    g_assert_false(linx_tile_tstore_resolve_binding(
        &desc, env->tile_iot_src_valid[0], env->tile_iot_src_phys[0],
        env->tile_reg_bytes, &tile, &size_code));
    g_free(env);
}

static void test_tepl_invalid_shape_is_atomic(void)
{
    CPULinxState *env = new_atomicity_env();
    VisibleCPUTileState *before = g_new(VisibleCPUTileState, 1);
    const unsigned sources[2] = { 0u, 1u };

    env->tile_reg_valid_cols[0] = 4u;
    env->tile_reg_valid_rows[0] = 1u; /* Output requires two rows. */
    env->tile_reg_cols[0] = 4u;
    env->tile_reg_rows[0] = 1u;
    env->tile_reg_valid_cols[1] = 4u;
    env->tile_reg_valid_rows[1] = 2u;
    env->tile_reg_cols[1] = 4u;
    env->tile_reg_rows[1] = 2u;
    capture_cpu_visible_state(env, before);

    g_assert_false(linx_tile_tepl_pre_publish_legal(
        env, 0x000u, sources, 2u, 17u, 4u, 4u, 2u, 4u, 2u));
    assert_cpu_visible_state_unchanged(env, before);
    g_free(before);
    g_free(env);
}

static void test_tepl_missing_tquant_scale_is_atomic(void)
{
    CPULinxState *env = new_atomicity_env();
    VisibleCPUTileState *before = g_new(VisibleCPUTileState, 1);
    const unsigned sources[1] = { 0u };

    capture_cpu_visible_state(env, before);
    g_assert_false(linx_tile_tepl_pre_publish_legal(
        env, 0x102u, sources, 1u, 17u, 4u, 4u, 2u, 4u, 2u));
    assert_cpu_visible_state_unchanged(env, before);
    g_free(before);
    g_free(env);
}

static void test_tepl_zero_tquant_scale_is_atomic(void)
{
    CPULinxState *env = new_atomicity_env();
    VisibleCPUTileState *before = g_new(VisibleCPUTileState, 1);
    const unsigned sources[1] = { 0u };

    /* Authored RI order resolves slot 0 to r2 and slot 1 to r3. */
    env->tile_ior_count = 1u;
    env->tile_ior_desc[0] = (2u << 10) | (3u << 5);
    env->gpr[2] = 0u;
    env->gpr[3] = 7u;
    capture_cpu_visible_state(env, before);

    g_assert_false(linx_tile_tepl_pre_publish_legal(
        env, 0x102u, sources, 1u, 17u, 4u, 4u, 2u, 4u, 2u));
    assert_cpu_visible_state_unchanged(env, before);
    g_free(before);
    g_free(env);
}

static void test_tepl_trem_zero_tile_divisor_is_atomic(void)
{
    CPULinxState *env = new_atomicity_env();
    VisibleCPUTileState *before = g_new(VisibleCPUTileState, 1);
    const unsigned sources[2] = { 0u, 1u };
    LinxTileTxnGate gate = { .datr_legal = true,
                             .allocation_available = true };

    for (unsigned tile = 0; tile < 2u; tile++) {
        env->tile_reg_capacity[tile] = 32u;
        env->tile_reg_bytes[tile] = 32u;
        env->tile_reg_elem_bytes[tile] = 4u;
        env->tile_reg_dtype[tile] = 17u;
        env->tile_reg_valid_cols[tile] = 4u;
        env->tile_reg_valid_rows[tile] = 2u;
        env->tile_reg_cols[tile] = 4u;
        env->tile_reg_rows[tile] = 2u;
        for (unsigned lane = 0; lane < 8u; lane++) {
            env->tile_reg[tile][lane] = lane + 1u;
        }
    }
    env->tile_reg[1][5] = 0u;
    capture_cpu_visible_state(env, before);
    gate.operands_legal = linx_tile_tepl_pre_publish_legal(
        env, 0x030u, sources, 2u, 17u, 4u, 4u, 2u, 4u, 2u);

    g_assert_cmpint(linx_tile_txn_guarded_apply(
                        &gate, mutate_cpu_visible_state, env), ==,
                    LINX_TILE_TXN_ILLEGAL);
    assert_cpu_visible_state_unchanged(env, before);
    g_free(before);
    g_free(env);
}

static void test_tepl_trems_zero_scalar_is_atomic(void)
{
    CPULinxState *env = new_atomicity_env();
    VisibleCPUTileState *before = g_new(VisibleCPUTileState, 1);
    const unsigned sources[1] = { 0u };
    LinxTileTxnGate gate = { .datr_legal = true,
                             .allocation_available = true };

    env->tile_reg_capacity[0] = 32u;
    env->tile_reg_bytes[0] = 32u;
    env->tile_reg_elem_bytes[0] = 4u;
    env->tile_reg_dtype[0] = 17u;
    env->tile_reg_valid_cols[0] = 4u;
    env->tile_reg_valid_rows[0] = 2u;
    env->tile_reg_cols[0] = 4u;
    env->tile_reg_rows[0] = 2u;
    env->tile_arg_format = 1u;
    env->tile_ior_count = 1u;
    env->tile_ior_desc[0] = 2u << 10;
    env->gpr[2] = 0u;
    capture_cpu_visible_state(env, before);
    gate.operands_legal = linx_tile_tepl_pre_publish_legal(
        env, 0x032u, sources, 1u, 17u, 4u, 4u, 2u, 4u, 2u);

    g_assert_cmpint(linx_tile_txn_guarded_apply(
                        &gate, mutate_cpu_visible_state, env), ==,
                    LINX_TILE_TXN_ILLEGAL);
    assert_cpu_visible_state_unchanged(env, before);
    g_free(before);
    g_free(env);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/tile-transaction/invalid-datr",
                    test_invalid_datr_is_atomic);
    g_test_add_func("/linx/tile-transaction/invalid-cube-operand",
                    test_invalid_cube_operand_is_atomic);
    g_test_add_func("/linx/tile-transaction/allocation-failure",
                    test_allocation_failure_is_atomic);
    g_test_add_func("/linx/tile-transaction/valid",
                    test_valid_transaction_applies_once);
    g_test_add_func("/linx/tile-transaction/tstore-source-footprint",
                    test_tstore_size_comes_from_source_footprint);
    g_test_add_func("/linx/tile-transaction/tepl-invalid-shape",
                    test_tepl_invalid_shape_is_atomic);
    g_test_add_func("/linx/tile-transaction/tepl-missing-tquant-scale",
                    test_tepl_missing_tquant_scale_is_atomic);
    g_test_add_func("/linx/tile-transaction/tepl-zero-tquant-scale",
                    test_tepl_zero_tquant_scale_is_atomic);
    g_test_add_func("/linx/tile-transaction/tepl-trem-zero-tile-divisor",
                    test_tepl_trem_zero_tile_divisor_is_atomic);
    g_test_add_func("/linx/tile-transaction/tepl-trems-zero-scalar",
                    test_tepl_trems_zero_scalar_is_atomic);
    return g_test_run();
}
