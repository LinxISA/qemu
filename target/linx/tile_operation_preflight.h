/*
 * Linx tile-operation pre-publish checks shared with the native atomicity test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LINX_TILE_OPERATION_PREFLIGHT_H
#define LINX_TILE_OPERATION_PREFLIGHT_H

#include "cpu.h"

static inline bool linx_tile_operation_preflight_resolve_ior(
    const CPULinxState *env, unsigned slot, unsigned *reg_out)
{
    static const unsigned shifts[] = { 10u, 5u, 15u, 0u };
    const unsigned count = env->tile_ior_count < LINX_TILE_MAX_IOR
                               ? env->tile_ior_count
                               : LINX_TILE_MAX_IOR;
    unsigned current = 0;

    if (slot >= LINX_TILE_MAX_IOR) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        for (unsigned authored = 0; authored < 4u; authored++) {
            const unsigned reg =
                (env->tile_ior_desc[i] >> shifts[authored]) & 0x1fu;

            if (reg == 0u) {
                continue;
            }
            if (current++ == slot) {
                if (reg >= LINX_GPR_COUNT) {
                    return false;
                }
                *reg_out = reg;
                return true;
            }
        }
    }
    return false;
}

static inline bool linx_tile_operation_preflight_shape_covers(
    const CPULinxState *env, unsigned tile, uint32_t valid_cols,
    uint32_t valid_rows, uint32_t cols, uint32_t rows)
{
    return tile < LINX_TILE_SLOT_COUNT &&
           env->tile_reg_valid_cols[tile] >= valid_cols &&
           env->tile_reg_valid_rows[tile] >= valid_rows &&
           env->tile_reg_cols[tile] == cols &&
           env->tile_reg_rows[tile] >= rows;
}

static inline bool linx_tile_operation_remainder_divisor_nonzero(
    uint32_t dtype, unsigned elem_bytes, uint64_t raw)
{
    switch (dtype & 0x1fu) {
    case 0u: /* FP64: +0 and -0 are both zero divisors. */
        return (raw & UINT64_C(0x7fffffffffffffff)) != 0u;
    case 1u: /* FP32 */
    case 2u: /* TF32 */
    case 3u: /* HF32 */
        return (raw & UINT32_C(0x7fffffff)) != 0u;
    default:
        if (elem_bytes == 1u) {
            return (raw & UINT8_MAX) != 0u;
        }
        if (elem_bytes == 2u) {
            return (raw & UINT16_MAX) != 0u;
        }
        if (elem_bytes == 4u) {
            return (raw & UINT32_MAX) != 0u;
        }
        return raw != 0u;
    }
}

static inline bool linx_tile_operation_remainder_tile_divisors_legal(
    const CPULinxState *env, unsigned tile, uint32_t dtype,
    unsigned elem_bytes, uint32_t valid_cols, uint32_t valid_rows,
    uint32_t cols)
{
    const uint8_t *data;

    if (tile >= LINX_TILE_SLOT_COUNT || elem_bytes == 0u ||
        elem_bytes > sizeof(uint64_t)) {
        return false;
    }
    data = (const uint8_t *)env->tile_reg[tile];
    for (uint32_t row = 0; row < valid_rows; row++) {
        for (uint32_t col = 0; col < valid_cols; col++) {
            const uint64_t offset =
                ((uint64_t)row * cols + col) * elem_bytes;
            uint64_t raw = 0u;

            if (offset + elem_bytes > env->tile_reg_bytes[tile]) {
                return false;
            }
            memcpy(&raw, data + offset, elem_bytes);
            if (!linx_tile_operation_remainder_divisor_nonzero(
                    dtype, elem_bytes, raw)) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Checks that must happen before output materialization.  Remaining
 * operation-specific checks are covered by the snapshotted dry execution in
 * linx_tile_preflight_operation(); keeping these high-risk checks here makes them
 * independently regression-testable against a real CPULinxState.
 */
static inline bool linx_tile_operation_pre_publish_legal(
    const CPULinxState *env, uint32_t impl, const unsigned *sources,
    unsigned source_count, uint32_t dtype, unsigned elem_bytes,
    uint32_t valid_cols, uint32_t valid_rows, uint32_t cols, uint32_t rows)
{
    unsigned src0 = source_count > 0u ? sources[0] : 0u;
    unsigned src1 = source_count > 1u ? sources[1] : 0u;
    unsigned src2 = source_count > 2u ? sources[2] : 0u;
    bool has_src0 = source_count > 0u;
    bool has_src1 = source_count > 1u;
    bool has_src2 = source_count > 2u;
    const bool expand = impl == 0x01eu || impl == 0x01fu ||
                        (impl >= 0x03bu && impl <= 0x048u);
    const bool partial = impl >= 0x0c3u && impl <= 0x0c6u;
    const bool custom_shape = impl == 0x00du || impl == 0x01cu ||
                              impl == 0x087u ||
                              impl == 0x089u || impl == 0x085u ||
                              impl == 0x084u ||
                              (impl >= 0x102u && impl <= 0x10bu);

    if (impl == 0x02cu) {
        if (source_count < 3u) {
            return false;
        }
        src0 = sources[1];
        src1 = sources[2];
        has_src0 = true;
        has_src1 = true;
    } else if (impl == 0x034u) {
        if (source_count < 2u) {
            return false;
        }
        src0 = sources[1];
        has_src0 = true;
        has_src1 = false;
    }

    if ((!expand && !partial && !custom_shape &&
         ((has_src0 && !linx_tile_operation_preflight_shape_covers(
                           env, src0, valid_cols, valid_rows, cols, rows)) ||
          (has_src1 && !linx_tile_operation_preflight_shape_covers(
                           env, src1, valid_cols, valid_rows, cols, rows)) ||
          (has_src2 && !linx_tile_operation_preflight_shape_covers(
                           env, src2, valid_cols, valid_rows, cols, rows)))) ||
        (expand && impl >= 0x03bu &&
         (!has_src0 || !linx_tile_operation_preflight_shape_covers(
                           env, src0, valid_cols, valid_rows, cols, rows)))) {
        return false;
    }

    if (impl == 0x102u) { /* TQUANT */
        unsigned scale_reg;
        unsigned zero_reg;

        return has_src0 && !has_src1 &&
               linx_tile_operation_preflight_resolve_ior(env, 0u, &scale_reg) &&
               linx_tile_operation_preflight_resolve_ior(env, 1u, &zero_reg) &&
               env->gpr[scale_reg] != 0u;
    }
    if (impl == 0x10cu) { /* TFMA */
        return has_src0 && has_src1 && has_src2 &&
               env->tile_reg_dtype[src0] == dtype &&
               env->tile_reg_dtype[src1] == dtype &&
               env->tile_reg_dtype[src2] == dtype;
    }
    if (impl == 0x00du) { /* TCVT */
        return has_src0 && !has_src1 && src0 < LINX_TILE_SLOT_COUNT &&
               env->tile_reg_valid_cols[src0] >= valid_cols &&
               env->tile_reg_valid_rows[src0] >= valid_rows &&
               env->tile_reg_cols[src0] == cols;
    }
    if (impl == 0x030u) { /* TREM */
        return has_src1 && linx_tile_operation_remainder_tile_divisors_legal(
                               env, src1, dtype, elem_bytes, valid_cols,
                               valid_rows, cols);
    }
    if (impl == 0x032u) { /* TREMS */
        unsigned scalar_reg;

        return linx_tile_operation_preflight_resolve_ior(
                   env, 0u, &scalar_reg) &&
               linx_tile_operation_remainder_divisor_nonzero(
                   dtype, elem_bytes, env->gpr[scalar_reg]);
    }
    return true;
}

#endif /* LINX_TILE_OPERATION_PREFLIGHT_H */
