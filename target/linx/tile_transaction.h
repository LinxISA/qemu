/*
 * Linx tile descriptor transaction gate.
 *
 * Keep this header independent of QEMU internals so the exact gate used by
 * helper.c can also be exercised by a native unit test.  No apply callback is
 * entered until every preflight class has succeeded.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LINX_TILE_TRANSACTION_H
#define LINX_TILE_TRANSACTION_H

#include <stdbool.h>

typedef enum LinxTileTxnFault {
    LINX_TILE_TXN_OK = 0,
    LINX_TILE_TXN_ILLEGAL,
    LINX_TILE_TXN_ALLOCATION,
} LinxTileTxnFault;

typedef struct LinxTileTxnGate {
    bool datr_legal;
    bool operands_legal;
    bool allocation_available;
} LinxTileTxnGate;

typedef bool (*LinxTileTxnApplyFn)(void *opaque);

static inline LinxTileTxnFault
linx_tile_txn_preflight(const LinxTileTxnGate *gate)
{
    if (!gate->datr_legal) {
        return LINX_TILE_TXN_ILLEGAL;
    }
    if (!gate->allocation_available) {
        return LINX_TILE_TXN_ALLOCATION;
    }
    if (!gate->operands_legal) {
        return LINX_TILE_TXN_ILLEGAL;
    }
    return LINX_TILE_TXN_OK;
}

static inline LinxTileTxnFault
linx_tile_txn_guarded_apply(const LinxTileTxnGate *gate,
                            LinxTileTxnApplyFn apply, void *opaque)
{
    LinxTileTxnFault fault = linx_tile_txn_preflight(gate);

    if (fault != LINX_TILE_TXN_OK) {
        return fault;
    }
    return apply(opaque) ? LINX_TILE_TXN_OK : LINX_TILE_TXN_ILLEGAL;
}

#endif /* LINX_TILE_TRANSACTION_H */
