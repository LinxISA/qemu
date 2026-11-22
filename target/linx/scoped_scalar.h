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
    uint8_t queue_sources; /* T#1..T#4 in bits 0..3, U#1..U#4 in 4..7. */
} LinxScopedScalar;

bool linx_scoped_scalar_decode(uint64_t word, LinxScopedScalar *decoded);
bool linx_scoped_scalar_runtime_requested(void);
bool linx_scoped_scalar_topology_supported(unsigned pe_count);

#endif
