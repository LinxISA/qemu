/*
 * PTO ISA 0.58 tile operation identity and engine tables.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_LINX_TILE_ISA_058_H
#define TARGET_LINX_TILE_ISA_058_H

enum {
    LINX_IOT_S0V = 1u << 0,
    LINX_IOT_S1V = 1u << 1,
    LINX_IOT_S0R = 1u << 2,
    LINX_IOT_S1R = 1u << 3,
};

enum {
    LINX_TILE_IOT_SRC0_SHIFT = 0,
    LINX_TILE_IOT_SRC1_SHIFT = 6,
    LINX_TILE_IOT_DST_SHIFT = 12,
    LINX_TILE_IOT_LAST_SHIFT = 15,
    LINX_TILE_IOT_FLAGS_SHIFT = 16,
    LINX_TILE_IOT_REG_SHIFT = 20,
    LINX_TILE_IOT_SIZE_SHIFT = 25,
    LINX_TILE_IOT_HAS_SIZE_SHIFT = 30,
};

typedef struct LinxTileIOTDesc {
    uint32_t src0;
    uint32_t src1;
    uint32_t dst;
    uint32_t last;
    uint32_t flags;
    uint32_t reg;
    uint32_t size;
    bool has_size;
} LinxTileIOTDesc;

static inline LinxTileIOTDesc linx_tile_decode_iot(uint64_t packed)
{
    LinxTileIOTDesc d;

    d.src0 = (packed >> LINX_TILE_IOT_SRC0_SHIFT) & 0x3fu;
    d.src1 = (packed >> LINX_TILE_IOT_SRC1_SHIFT) & 0x3fu;
    d.dst = (packed >> LINX_TILE_IOT_DST_SHIFT) & 0x7u;
    d.last = (packed >> LINX_TILE_IOT_LAST_SHIFT) & 0x1u;
    d.flags = (packed >> LINX_TILE_IOT_FLAGS_SHIFT) & 0xfu;
    d.reg = (packed >> LINX_TILE_IOT_REG_SHIFT) & 0x1fu;
    d.size = (packed >> LINX_TILE_IOT_SIZE_SHIFT) & 0x1fu;
    d.has_size = ((packed >> LINX_TILE_IOT_HAS_SIZE_SHIFT) & 0x1u) != 0;
    return d;
}

static inline bool linx_tile_shared_allocation_compatible(
    uint8_t allocation_mask, uint8_t requested_mask,
    uint32_t allocation_capacity, uint32_t requested_capacity,
    uint32_t allocation_dtype, uint32_t requested_dtype)
{
    return allocation_mask == 0u ||
           ((requested_mask & ~allocation_mask) == 0u &&
            allocation_capacity == requested_capacity &&
            allocation_dtype == requested_dtype);
}

/* Source-only operations recover the internal allocation size from the Tile. */
static inline bool linx_tile_size_code_from_bytes(uint32_t bytes,
                                                  unsigned *size_code)
{
    for (unsigned size = 3u; size <= 9u; size++) {
        if (bytes == (UINT32_C(1) << (size + 4u))) {
            *size_code = size;
            return true;
        }
    }
    return false;
}

static inline bool linx_tile_tstore_resolve_binding(
    const LinxTileIOTDesc *desc, uint8_t bound_sources,
    const uint8_t bound_tiles[2],
    const uint32_t tile_bytes[LINX_TILE_SLOT_COUNT],
    unsigned *tile, unsigned *size_code)
{
    unsigned source;

    /* TSTORE has a source operand but no destination TSize field. */
    if (desc->has_size) {
        return false;
    }
    if ((desc->flags & LINX_IOT_S0V) == 0u) {
        source = 0u;
    } else if ((desc->flags & LINX_IOT_S1V) == 0u) {
        source = 1u;
    } else {
        return false;
    }
    if ((bound_sources & (1u << source)) == 0u) {
        return false;
    }
    *tile = bound_tiles[source];
    return *tile < LINX_TILE_SLOT_COUNT &&
           linx_tile_size_code_from_bytes(tile_bytes[*tile], size_code);
}

static inline bool linx_tile_data_type_accepted(uint32_t data_type)
{
    return data_type < 32u &&
           (UINT32_C(0x1f1f7fff) & (UINT32_C(1) << data_type)) != 0;
}

static inline bool linx_tile_layout_accepted(uint32_t layout)
{
    return layout < 32u &&
           (UINT32_C(0x5816035b) & (UINT32_C(1) << layout)) != 0;
}

/* TEPL remains only the unchanged two-bit Mode/five-bit Function carrier. */
static inline bool linx_tile_operation_selector_accepted(uint32_t selector)
{
    static const uint32_t function_masks[4] = {
        UINT32_C(0x1cffbfdf),
        UINT32_C(0x0c003fdf),
        UINT32_C(0x3fff3fff),
        UINT32_C(0x001ffdfd),
    };
    const uint32_t mode = selector >> 5;
    const uint32_t function = selector & 0x1fu;

    return mode < ARRAY_SIZE(function_masks) &&
           (function_masks[mode] & (UINT32_C(1) << function)) != 0;
}

typedef enum LinxTileEngine {
    LINX_TILE_ENGINE_VEC,
    LINX_TILE_ENGINE_SFU,
    LINX_TILE_ENGINE_TLSU,
    LINX_TILE_ENGINE_CUBE,
} LinxTileEngine;

static inline LinxTileEngine linx_tile_operation_engine(uint32_t selector)
{
    static const uint32_t vec_function_masks[4] = {
        UINT32_C(0x1c83bfdf),
        UINT32_C(0x0c003fdf),
        UINT32_C(0x00000000),
        UINT32_C(0x00000000),
    };
    const uint32_t mode = selector >> 5;
    const uint32_t function = selector & 0x1fu;

    if (mode < ARRAY_SIZE(vec_function_masks) &&
        (vec_function_masks[mode] & (UINT32_C(1) << function)) != 0u) {
        return LINX_TILE_ENGINE_VEC;
    }
    return LINX_TILE_ENGINE_SFU;
}

/* CUBE functions outside this mask are reserved by PTO ISA 0.58. */
static inline bool linx_tile_cube_function_accepted(uint32_t function)
{
    return function < 32u &&
           (UINT32_C(0x00770077) & (UINT32_C(1) << function)) != 0;
}

enum {
    LINX_DATR_SAT = 1u << 0,
    LINX_DATR_CANONICALIZE = 1u << 1,
    LINX_DATR_DATA_TYPE = 1u << 2,
    LINX_DATR_RMODE = 1u << 3,
    LINX_DATR_LAYOUT = 1u << 4,
    LINX_DATR_PAD_OR_BYTE_ID = 1u << 5,
    LINX_DATR_CMODE = 1u << 6,
};

static inline uint32_t linx_tile_datr_nonzero_fields(uint32_t packed)
{
    return (((packed >> 28) & 1u) ? LINX_DATR_SAT : 0u) |
           (((packed >> 17) & 1u) ? LINX_DATR_CANONICALIZE : 0u) |
           (((packed >> 7) & 0x1fu) ? LINX_DATR_DATA_TYPE : 0u) |
           (((packed >> 25) & 7u) ? LINX_DATR_RMODE : 0u) |
           (((packed >> 2) & 0x1fu) ? LINX_DATR_LAYOUT : 0u) |
           (((packed >> 12) & 3u) ? LINX_DATR_PAD_OR_BYTE_ID : 0u) |
           (((packed >> 22) & 7u) ? LINX_DATR_CMODE : 0u);
}

/* Generated from pto-spec spec/catalog/tile-operations.json. */
static inline uint32_t linx_tile_operation_datr_allowed(uint32_t selector)
{
    static const uint8_t allowed[128] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x30, 0x00, 0x00,
        0x24, 0x00, 0x1f, 0x1f, 0x00, 0x00, 0x10, 0x10,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
        0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    return selector < ARRAY_SIZE(allowed) ? allowed[selector] : 0u;
}

static inline uint32_t linx_tile_datr_allowed(uint32_t blocktype,
                                              uint32_t function)
{
    switch (blocktype) {
    case 2u: /* TLSU */
        if (function == 6u) {
            return LINX_DATR_LAYOUT | LINX_DATR_PAD_OR_BYTE_ID;
        }
        return function == 3u ? 0u : LINX_DATR_LAYOUT;
    case 6u: /* CUBE */
        return function == 8u ?
            LINX_DATR_SAT | LINX_DATR_CANONICALIZE |
            LINX_DATR_DATA_TYPE | LINX_DATR_RMODE | LINX_DATR_LAYOUT : 0u;
    case 7u: /* TEPL */
        return linx_tile_operation_datr_allowed(function & 0x7fu);
    default:
        return 0u;
    }
}

static inline bool linx_tile_datr_applicable(uint32_t blocktype,
                                             uint32_t function,
                                             uint32_t packed)
{
    return (linx_tile_datr_nonzero_fields(packed) &
            ~linx_tile_datr_allowed(blocktype, function)) == 0u;
}

#endif /* TARGET_LINX_TILE_ISA_058_H */
