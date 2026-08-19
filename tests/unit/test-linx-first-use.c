/*
 * Linx VECTOR/CUBE first-use exception tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "first_use.h"

static void seed_visible_state(CPULinxState *env)
{
    env->bpc = UINT64_C(0x1110);
    env->brtype = 7;
    env->blocktype = 6;
    env->tq[0] = UINT64_C(0x2220);
    env->uq[0] = UINT64_C(0x3330);
    env->tile_func = 4;
    env->tile_hand_live[0] = 3;
}

static void assert_visible_state(const CPULinxState *env)
{
    g_assert_cmphex(env->bpc, ==, UINT64_C(0x1110));
    g_assert_cmpuint(env->brtype, ==, 7);
    g_assert_cmpuint(env->blocktype, ==, 6);
    g_assert_cmphex(env->tq[0], ==, UINT64_C(0x2220));
    g_assert_cmphex(env->uq[0], ==, UINT64_C(0x3330));
    g_assert_cmpuint(env->tile_func, ==, 4);
    g_assert_cmpuint(env->tile_hand_live[0], ==, 3);
}

static void test_reset_and_mask(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);

    linx_first_use_reset(env);
    for (unsigned acr = 0; acr < LINX_ACR_COUNT; acr++) {
        g_assert_cmphex(env->ssr_acr[acr][LINX_SSR_ECONFIG], ==,
                        LINX_ECONFIG_RESET);
    }
    g_assert_cmphex(linx_econfig_sanitize(UINT64_MAX), ==,
                    LINX_ECONFIG_ALLOWED_MASK);
    g_free(env);
}

static void test_vector_and_cube_are_independent(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);

    linx_first_use_reset(env);
    env->acr = 2;
    seed_visible_state(env);

    g_assert_true(linx_first_use_prepare(
        env, LINX_FIRST_USE_VECTOR, UINT64_C(0x400)));
    g_assert_cmpuint(env->pending_trap_cause, ==, 4);
    g_assert_cmphex(env->pending_trap_arg0, ==, 0);
    g_assert_cmphex(env->pc, ==, UINT64_C(0x400));
    assert_visible_state(env);

    env->ssr_acr[1][LINX_SSR_ECONFIG] &= ~LINX_ECONFIG_VECTOR_BIT;
    g_assert_false(linx_first_use_prepare(
        env, LINX_FIRST_USE_VECTOR, UINT64_C(0x410)));
    g_assert_true(linx_first_use_prepare(
        env, LINX_FIRST_USE_CUBE, UINT64_C(0x420)));
    g_assert_cmpuint(env->pending_trap_cause, ==, 4);
    g_assert_cmphex(env->pending_trap_arg0, ==, 1);
    g_assert_cmphex(env->pc, ==, UINT64_C(0x420));
    assert_visible_state(env);
    g_free(env);
}

static void test_wrong_acr_and_disabled_are_effect_free(void)
{
    CPULinxState *env = g_new0(CPULinxState, 1);

    linx_first_use_reset(env);
    env->acr = 1;
    seed_visible_state(env);
    g_assert_false(linx_first_use_prepare(
        env, LINX_FIRST_USE_VECTOR, UINT64_C(0x500)));
    assert_visible_state(env);
    g_assert_cmpuint(env->pending_trap_cause, ==, 0);
    g_assert_cmphex(env->pending_trap_arg0, ==, 0);

    env->acr = 2;
    env->ssr_acr[1][LINX_SSR_ECONFIG] = 0;
    g_assert_false(linx_first_use_prepare(
        env, LINX_FIRST_USE_CUBE, UINT64_C(0x510)));
    assert_visible_state(env);
    g_assert_cmpuint(env->pending_trap_cause, ==, 0);
    g_assert_cmphex(env->pending_trap_arg0, ==, 0);
    g_free(env);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/first-use/reset-mask", test_reset_and_mask);
    g_test_add_func("/linx/first-use/independent",
                    test_vector_and_cube_are_independent);
    g_test_add_func("/linx/first-use/inapplicable",
                    test_wrong_acr_and_disabled_are_effect_free);
    return g_test_run();
}
