/*
 * PTO 0.59.1 scoped Scalar carrier decoding.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LINX_SCOPED_SCALAR_H
#define LINX_SCOPED_SCALAR_H

typedef enum LinxScalarScope {
    LINX_SCALAR_PRIVATE,
    LINX_SCALAR_INTRA_CORE,
    LINX_SCALAR_INTER_CORE,
} LinxScalarScope;

typedef struct LinxScopedScalar {
    uint64_t inner_word;
    const char *base_form_id;
    LinxScalarScope scope;
    unsigned inner_bytes;
    bool runtime_eligible;
    int reg_store_shift;
} LinxScopedScalar;

bool linx_scoped_scalar_decode(uint64_t word, LinxScopedScalar *decoded);
bool linx_scoped_scalar_runtime_requested(void);

#endif
