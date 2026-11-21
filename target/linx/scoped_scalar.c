/*
 * PTO 0.59.1 scoped Scalar carrier decoding.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "scoped_scalar.h"

typedef struct LinxScopedScalarForm {
    uint64_t mask;
    uint64_t match;
    uint64_t reserved_mask; /* The all-ones SrcRType value, if present. */
    unsigned inner_bytes;
    bool runtime_eligible;
    const char *base_form_id;
} LinxScopedScalarForm;

static const LinxScopedScalarForm linx_scoped_scalar_forms[] = {
#include "scoped_scalar_forms.inc"
};

bool linx_scoped_scalar_runtime_requested(void)
{
    const char *flag = getenv("LINX_QEMU_SCOPED_SCALAR_RUNTIME");

    return flag && flag[0] && strcmp(flag, "0") != 0;
}

bool linx_scoped_scalar_decode(uint64_t word, LinxScopedScalar *decoded)
{
    unsigned scope = (word >> 8) & 3;
    unsigned length = (word >> 10) & 3;
    unsigned inner_bytes = 2 * (length + 1);
    uint64_t inner_word = word >> 16;

    if ((word & 0xf0ff) != 0x001f || scope == 3 || length == 3 ||
        inner_word >> (8 * inner_bytes)) {
        return false;
    }

    for (size_t i = 0; i < ARRAY_SIZE(linx_scoped_scalar_forms); i++) {
        const LinxScopedScalarForm *form = &linx_scoped_scalar_forms[i];

        if (form->inner_bytes == inner_bytes &&
            (inner_word & form->mask) == form->match &&
            (!form->reserved_mask ||
             (inner_word & form->reserved_mask) != form->reserved_mask)) {
            *decoded = (LinxScopedScalar) {
                .inner_word = inner_word,
                .base_form_id = form->base_form_id,
                .scope = scope,
                .inner_bytes = inner_bytes,
                .runtime_eligible = form->runtime_eligible,
            };
            return true;
        }
    }
    return false;
}
