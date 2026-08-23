/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "target/linx/insn_numeric_058.h"

static void test_hl_lui(void)
{
    g_assert_cmphex(linx_hl_lui_value(0), ==, UINT64_C(0));
    g_assert_cmphex(linx_hl_lui_value(1), ==, UINT64_C(0x0000000100000000));
    g_assert_cmphex(linx_hl_lui_value(65536), ==,
                    UINT64_C(0x0001000000000000));
    g_assert_cmphex(linx_hl_lui_value(UINT32_MAX), ==,
                    UINT64_C(0xffffffff00000000));
}

static void test_hl_li_signedness(void)
{
    g_assert_cmphex(linx_hl_liu_value(UINT32_MAX), ==,
                    UINT64_C(0x00000000ffffffff));
    g_assert_cmphex(linx_hl_lis_value(-1), ==, UINT64_MAX);
}

static void test_csel_src_r_type(void)
{
    g_assert_false(linx_csel_negates_src_r(0));
    g_assert_false(linx_csel_negates_src_r(1));
    g_assert_true(linx_csel_negates_src_r(2));
    g_assert_false(linx_csel_negates_src_r(3));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/insn/hl-lui", test_hl_lui);
    g_test_add_func("/linx/insn/hl-li-signedness", test_hl_li_signedness);
    g_test_add_func("/linx/insn/csel-src-r-type", test_csel_src_r_type);
    return g_test_run();
}
