/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "../../target/linx/scoped_scalar.h"

static const struct {
    uint64_t word;
    LinxScalarScope scope;
    unsigned inner_bytes;
    const char *base_form_id;
} witnesses[] = {
#include "linx-scoped-scalar-witnesses.inc"
};

static void test_scope_forms(void)
{
    LinxScopedScalar decoded;
    unsigned runtime_forms = 0;

    g_assert_cmpuint(ARRAY_SIZE(witnesses), ==, 705);
    for (size_t i = 0; i < ARRAY_SIZE(witnesses); i++) {
        g_assert_true(linx_scoped_scalar_decode(witnesses[i].word, &decoded));
        g_assert_cmpstr(decoded.base_form_id, ==, witnesses[i].base_form_id);
        g_assert_cmpuint(decoded.scope, ==, witnesses[i].scope);
        g_assert_cmpuint(decoded.inner_bytes, ==, witnesses[i].inner_bytes);
        g_assert_cmpuint(decoded.inner_word, ==, witnesses[i].word >> 16);
        runtime_forms += decoded.runtime_eligible;
    }
    g_assert_cmpuint(runtime_forms, ==, 519);
}

static void test_scope_invalid(void)
{
    static const uint64_t bad_words[] = {
        0x00000000001a101f, /* Reserved prefix bits */
        0x00000000001a031f, /* Reserved scope */
        0x00000000001a0c1f, /* Reserved length */
        0x00000001001a001f, /* Nonzero padding for a 16-bit word */
        0x000100002019041f, /* Nonzero padding for a 32-bit word */
        0x000000000000001f, /* C.BSTOP */
        0x000000000007041f, /* ADDTPC */
        0x00000000700b041f, /* DMA */
        0x06000009003e081f, /* HL.LB.PO with forbidden SrcRType */
    };
    LinxScopedScalar decoded = { .inner_word = UINT64_MAX };

    for (size_t i = 0; i < ARRAY_SIZE(bad_words); i++) {
        g_assert_false(linx_scoped_scalar_decode(bad_words[i], &decoded));
        g_assert_cmpuint(decoded.inner_word, ==, UINT64_MAX);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/linx/scoped-scalar/released-forms", test_scope_forms);
    g_test_add_func("/linx/scoped-scalar/illegal-carriers", test_scope_invalid);
    return g_test_run();
}
