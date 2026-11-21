/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "../../target/linx/cpu.h"
#include "../../target/linx/hscb_geometry.h"

static void test_hscb_geometry_lifecycle(void)
{
    CPULinxState env = { 0 };

    g_assert_cmpuint(linx_hscb_geometry_blocknum(&env), ==, 0);
    g_assert_cmpuint(linx_hscb_geometry_blockid(&env), ==, 0);
    g_assert_false(linx_hscb_geometry_bind(&env, 1, 7, 8, 3));
    g_assert_false(linx_hscb_geometry_bind(&env, 0, 0, 8, 3));
    g_assert_false(linx_hscb_geometry_bind(&env, 0, 7, 0, 0));
    g_assert_false(linx_hscb_geometry_bind(&env, 0, 7, 8, 8));
    g_assert_false(linx_hscb_geometry_bind(&env, 0, 7,
                                           UINT64_C(1) << 32, 3));

    g_assert_true(linx_hscb_geometry_bind(&env, 0, 7, 8, 3));
    g_assert_true(linx_hscb_geometry_matches(&env, 7));
    g_assert_cmpuint(linx_hscb_geometry_blocknum(&env), ==, 8);
    g_assert_cmpuint(linx_hscb_geometry_blockid(&env), ==, 3);
    g_assert_false(linx_hscb_geometry_bind(&env, 0, 8, 4, 1));
    g_assert_false(linx_hscb_geometry_clear(&env, 8));

    env.acr = 1;
    g_assert_cmpuint(linx_hscb_geometry_blocknum(&env), ==, 0);
    g_assert_cmpuint(linx_hscb_geometry_blockid(&env), ==, 0);
    env.acr = 0;
    g_assert_cmpuint(linx_hscb_geometry_blocknum(&env), ==, 8);
    g_assert_cmpuint(linx_hscb_geometry_blockid(&env), ==, 3);

    g_assert_true(linx_hscb_geometry_clear(&env, 7));
    g_assert_false(linx_hscb_geometry_matches(&env, 7));
    g_assert_cmpuint(linx_hscb_geometry_blocknum(&env), ==, 0);
    g_assert_cmpuint(linx_hscb_geometry_blockid(&env), ==, 0);
    g_assert_true(linx_hscb_geometry_bind(&env, 0, 9,
                                          UINT32_MAX, UINT32_MAX - 1));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/hscb/geometry-lifecycle",
                    test_hscb_geometry_lifecycle);
    return g_test_run();
}
