/*
 * PTO ISA 0.58 hardware numeric profile shared helpers.
 *
 * This file deliberately contains no CPULinxState dependencies: the CUBE
 * executor and the executable conformance-vector test use the same decode,
 * type-table, packed-lane, rounding, and conversion implementation.
 */
#ifndef LINX_TILE_NUMERIC_058_H
#define LINX_TILE_NUMERIC_058_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    LINX_TILE_ACC_FP64 = 0,
    LINX_TILE_ACC_FP32 = 1,
    LINX_TILE_ACC_S64 = 16,
    LINX_TILE_ACC_U64 = 24,
};

static inline bool linx_tile_numeric_is_packed(uint32_t dtype)
{
    dtype &= 31u;
    return dtype == 11u || dtype == 12u || dtype == 14u || dtype == 20u ||
           dtype == 28u;
}

static inline bool linx_tile_numeric_ordinary(uint32_t dtype)
{
    dtype &= 31u;
    return dtype <= 12u || dtype == 14u || (dtype >= 16u && dtype <= 20u) ||
           (dtype >= 24u && dtype <= 28u);
}

static inline uint8_t linx_tile_numeric_acc_dtype(uint32_t dtype)
{
    dtype &= 31u;
    if (dtype == 0u) {
        return LINX_TILE_ACC_FP64;
    }
    if (dtype >= 1u && dtype <= 14u && dtype != 13u) {
        return LINX_TILE_ACC_FP32;
    }
    if (dtype >= 16u && dtype <= 20u) {
        return LINX_TILE_ACC_S64;
    }
    if (dtype >= 24u && dtype <= 28u) {
        return LINX_TILE_ACC_U64;
    }
    return UINT8_MAX;
}

static inline bool linx_tile_numeric_mx_pair(uint32_t left, uint32_t right)
{
    left &= 31u;
    right &= 31u;
    if ((left == 7u || left == 8u) && (right == 7u || right == 8u)) {
        return true;
    }
    return (left == 11u || left == 14u) && (right == 11u || right == 14u);
}

static inline uint64_t linx_tile_numeric_canonical_nan(uint32_t dtype)
{
    switch (dtype & 31u) {
    case 0u:
        return UINT64_C(0x7ff8000000000000);
    case 1u:
    case 2u:
    case 3u:
        return 0x7fc00000u;
    case 4u:
        return 0x7e00u;
    case 5u:
        return 0x7fc0u;
    case 6u:
        return 0x80u;
    case 7u:
        return 0x7fu;
    case 8u:
        return 0x7eu;
    case 13u:
        return 0xffu;
    default:
        return 0u;
    }
}

static inline float linx_tile_numeric_f32(uint32_t raw)
{
    float f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

static inline double linx_tile_numeric_f64(uint64_t raw)
{
    double f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

static inline uint32_t linx_tile_numeric_f32_raw(float f)
{
    uint32_t raw;
    memcpy(&raw, &f, sizeof(raw));
    return raw;
}

static inline uint64_t linx_tile_numeric_f64_raw(double f)
{
    uint64_t raw;
    memcpy(&raw, &f, sizeof(raw));
    return raw;
}

static inline float linx_tile_numeric_fp16(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1fu;
    uint32_t frac = h & 0x3ffu;
    uint32_t raw;

    if (exp == 0u) {
        if (frac == 0u) {
            raw = sign;
        } else {
            int shift = 0;
            while ((frac & 0x400u) == 0u) {
                frac <<= 1;
                shift++;
            }
            frac &= 0x3ffu;
            raw = sign | ((uint32_t)(113 - shift) << 23) | (frac << 13);
        }
    } else if (exp == 31u) {
        raw = sign | 0x7f800000u | (frac ? 0x00400000u : 0u);
    } else {
        raw = sign | ((exp + 112u) << 23) | (frac << 13);
    }
    return linx_tile_numeric_f32(raw);
}

static inline double linx_tile_numeric_hif8(uint8_t raw)
{
    const bool neg = (raw & 0x80u) != 0;
    const uint8_t p = raw & 0x7fu;
    unsigned d, ebits, mbits, em, m;
    int exponent;
    double value;

    if (raw == 0x00u) {
        return 0.0;
    }
    if (raw == 0x80u) {
        return NAN;
    }
    if (raw == 0x6fu) {
        return INFINITY;
    }
    if (raw == 0xefu) {
        return -INFINITY;
    }
    if ((p & 0x78u) == 0u) {
        value = ldexp(1.0, (int)(p & 7u) - 23);
        return neg ? -value : value;
    }
    if ((p & 0x78u) == 0x08u) {
        d = 0;
        ebits = 0;
        mbits = 3;
    } else if ((p & 0x70u) == 0x10u) {
        d = 1;
        ebits = 1;
        mbits = 3;
    } else if ((p & 0x60u) == 0x20u) {
        d = 2;
        ebits = 2;
        mbits = 3;
    } else if ((p & 0x60u) == 0x40u) {
        d = 3;
        ebits = 3;
        mbits = 2;
    } else {
        d = 4;
        ebits = 4;
        mbits = 1;
    }
    (void)d;
    m = p & ((1u << mbits) - 1u);
    em = (p >> mbits) & ((1u << ebits) - 1u);
    if (ebits == 0u) {
        exponent = 0;
    } else {
        unsigned magnitude =
            (1u << (ebits - 1u)) | (em & ((1u << (ebits - 1u)) - 1u));
        exponent =
            (em & (1u << (ebits - 1u))) ? -(int)magnitude : (int)magnitude;
    }
    value = ldexp(1.0 + (double)m / (double)(1u << mbits), exponent);
    return neg ? -value : value;
}

static inline double linx_tile_numeric_minifloat(uint8_t raw, unsigned ebits,
                                                 unsigned mbits, int bias,
                                                 bool e4m3, bool infinity)
{
    const unsigned signbit = 1u << (ebits + mbits);
    const unsigned emask = (1u << ebits) - 1u;
    const unsigned mmask = (1u << mbits) - 1u;
    const unsigned exp = (raw >> mbits) & emask;
    const unsigned mant = raw & mmask;
    const bool neg = (raw & signbit) != 0;
    double v;

    if (exp == 0u) {
        v = mant == 0u ? 0.0 : ldexp((double)mant / (1u << mbits), 1 - bias);
    } else if (exp == emask &&
               ((e4m3 && mant == mmask) || (infinity && mant != 0u))) {
        return NAN;
    } else if (exp == emask && infinity && mant == 0u) {
        return neg ? -INFINITY : INFINITY;
    } else {
        v = ldexp(1.0 + (double)mant / (1u << mbits), (int)exp - bias);
    }
    return neg ? -v : v;
}

static inline uint8_t linx_tile_numeric_nibble(uint8_t storage,
                                               uint32_t logical_lane)
{
    return (storage >> ((logical_lane & 1u) * 4u)) & 0x0fu;
}

static inline double linx_tile_numeric_decode(uint32_t dtype, uint64_t raw,
                                              uint32_t logical_lane)
{
    static const double e2m1_values[8] = {0, .5, 1, 1.5, 2, 3, 4, 6};
    uint8_t lane;
    dtype &= 31u;
    switch (dtype) {
    case 0u:
        return linx_tile_numeric_f64(raw);
    case 1u:
    case 2u:
    case 3u:
        return linx_tile_numeric_f32((uint32_t)raw);
    case 4u:
        return linx_tile_numeric_fp16((uint16_t)raw);
    case 5u:
        return linx_tile_numeric_f32((uint32_t)(uint16_t)raw << 16);
    case 6u:
        return linx_tile_numeric_hif8((uint8_t)raw);
    case 7u:
        return linx_tile_numeric_minifloat(raw, 4, 3, 7, true, false);
    case 8u:
        return linx_tile_numeric_minifloat(raw, 5, 2, 15, false, true);
    case 9u:
        return linx_tile_numeric_minifloat(raw & 0x3fu, 3, 2, 3, false, false);
    case 10u:
        return linx_tile_numeric_minifloat(raw & 0x3fu, 2, 3, 1, false, false);
    case 11u:
        lane = linx_tile_numeric_nibble(raw, logical_lane);
        return (lane & 8u ? -1.0 : 1.0) * e2m1_values[lane & 7u];
    case 12u:
    case 14u:
        lane = linx_tile_numeric_nibble(raw, logical_lane);
        return (lane & 8u ? -1.0 : 1.0) * (double)(lane & 7u) / 4.0;
    case 13u:
        return (uint8_t)raw == 0xffu ? NAN
                                     : ldexp(1.0, (int)(uint8_t)raw - 127);
    case 16u:
        return (double)(int64_t)raw;
    case 17u:
        return (double)(int32_t)raw;
    case 18u:
        return (double)(int16_t)raw;
    case 19u:
        return (double)(int8_t)raw;
    case 20u:
        lane = linx_tile_numeric_nibble(raw, logical_lane);
        return (double)(int8_t)((lane ^ 8u) - 8u);
    case 24u:
        return (double)raw;
    case 25u:
        return (double)(uint32_t)raw;
    case 26u:
        return (double)(uint16_t)raw;
    case 27u:
        return (double)(uint8_t)raw;
    case 28u:
        return linx_tile_numeric_nibble(raw, logical_lane);
    default:
        return NAN;
    }
}

static inline int64_t linx_tile_numeric_round_s64(double value, unsigned mode)
{
    double floorv = floor(value), ceilv = ceil(value), frac;
    if (!isfinite(value)) {
        return INT64_MIN;
    }
    switch (mode & 7u) {
    case 2:
        return (int64_t)trunc(value);
    case 3:
        return (int64_t)floorv;
    case 4:
        return (int64_t)ceilv;
    case 5:
        return (int64_t)(value < 0 ? ceil(value - .5) : floor(value + .5));
    case 6:
        if (value == trunc(value)) {
            return (int64_t)value;
        }
        return ((int64_t)floorv & 1) != 0 ? (int64_t)floorv : (int64_t)ceilv;
    case 7:
        frac = value - floorv;
        return (int64_t)(frac < .5 ? floorv : ceilv);
    default:
        frac = value - floorv;
        if (frac < .5) {
            return (int64_t)floorv;
        }
        if (frac > .5) {
            return (int64_t)ceilv;
        }
        return ((int64_t)floorv & 1) == 0 ? (int64_t)floorv : (int64_t)ceilv;
    }
}

static inline bool linx_tile_numeric_round_up(uint64_t lower, double fraction,
                                              bool negative, unsigned mode)
{
    if (fraction == 0.0) {
        return false;
    }
    switch (mode & 7u) {
    case 2: /* RTZ */
        return false;
    case 3: /* RDN */
        return negative;
    case 4: /* RUP */
        return !negative;
    case 5: /* RNA */
        return fraction >= .5;
    case 6: /* RTO */
        return (lower & 1u) == 0u;
    case 7: /* RHB */
        return fraction > .5 || (fraction == .5 && !negative);
    default: /* NONE and RNE */
        return fraction > .5 || (fraction == .5 && (lower & 1u));
    }
}

static inline uint64_t linx_tile_numeric_binary_max(unsigned ebits,
                                                    unsigned precision_frac,
                                                    unsigned carrier_frac,
                                                    bool negative)
{
    uint64_t sign = negative ? UINT64_C(1) << (ebits + carrier_frac) : 0u;
    uint64_t exp = (UINT64_C(1) << ebits) - 2u;
    return sign | (exp << carrier_frac) |
           (((UINT64_C(1) << precision_frac) - 1u)
            << (carrier_frac - precision_frac));
}

static inline bool linx_tile_numeric_overflow_to_infinity(bool negative,
                                                          unsigned mode)
{
    return negative
               ? (mode & 7u) != 2u && (mode & 7u) != 4u && (mode & 7u) != 6u
               : (mode & 7u) != 2u && (mode & 7u) != 3u && (mode & 7u) != 6u;
}

static inline uint64_t
linx_tile_numeric_encode_binary(double value, unsigned ebits,
                                unsigned precision_frac, unsigned carrier_frac,
                                int bias, unsigned mode, bool sat)
{
    const bool negative = signbit(value);
    const uint64_t sign = negative ? UINT64_C(1) << (ebits + carrier_frac) : 0u;
    const uint64_t exp_all = (UINT64_C(1) << ebits) - 1u;
    const int emin = 1 - bias;
    const int emax = (int)exp_all - 1 - bias;
    double magnitude = fabs(value), scaled, fraction;
    uint64_t lower, significand, frac_field;
    int exponent;

    if (isnan(value)) {
        return (exp_all << carrier_frac) |
               (UINT64_C(1) << (carrier_frac - 1u));
    }
    if (magnitude == 0.0) {
        return sign;
    }
    if (isinf(value)) {
        return sat ? linx_tile_numeric_binary_max(ebits, precision_frac,
                                                  carrier_frac, negative)
                   : sign | (exp_all << carrier_frac);
    }

    frexp(magnitude, &exponent);
    exponent--;
    if (exponent > emax) {
        bool to_infinity =
            linx_tile_numeric_overflow_to_infinity(negative, mode);
        return sat || !to_infinity
                   ? linx_tile_numeric_binary_max(ebits, precision_frac,
                                                  carrier_frac, negative)
                   : sign | (exp_all << carrier_frac);
    }

    if (exponent < emin) {
        scaled = ldexp(magnitude, (int)precision_frac - emin);
        lower = floor(scaled);
        fraction = scaled - lower;
        significand =
            lower + linx_tile_numeric_round_up(lower, fraction, negative, mode);
        if (significand == 0u) {
            return sign;
        }
        if (significand >= (UINT64_C(1) << precision_frac)) {
            return sign | (UINT64_C(1) << carrier_frac);
        }
        return sign | (significand << (carrier_frac - precision_frac));
    }

    scaled = ldexp(magnitude, (int)precision_frac - exponent);
    lower = floor(scaled);
    fraction = scaled - lower;
    significand =
        lower + linx_tile_numeric_round_up(lower, fraction, negative, mode);
    if (significand == (UINT64_C(1) << (precision_frac + 1u))) {
        significand >>= 1;
        exponent++;
    }
    if (exponent > emax) {
        return sat || !linx_tile_numeric_overflow_to_infinity(negative, mode)
                   ? linx_tile_numeric_binary_max(ebits, precision_frac,
                                                  carrier_frac, negative)
                   : sign | (exp_all << carrier_frac);
    }
    frac_field = significand - (UINT64_C(1) << precision_frac);
    return sign | ((uint64_t)(exponent + bias) << carrier_frac) |
           (frac_field << (carrier_frac - precision_frac));
}

static inline uint8_t linx_tile_numeric_encode_e8m0(double value,
                                                     unsigned mode, bool sat)
{
    double exponent_value;
    long long exponent;

    if (!isfinite(value) || value <= 0.0) {
        if (isinf(value) && value > 0.0) {
            return sat ? 0xfeu : 0xffu;
        }
        return 0xffu;
    }
    exponent_value = log2(value);
    switch (mode & 7u) {
    case 2u: /* RTZ */
    case 3u: /* RTM */
        exponent = (long long)floor(exponent_value);
        break;
    case 4u: /* RTP */
        exponent = (long long)ceil(exponent_value);
        break;
    case 5u: /* RNA */
        exponent = (long long)floor(exponent_value + 0.5);
        break;
    default: { /* RNE and the profile's deterministic fallback modes. */
        const double lower = floor(exponent_value);
        const double fraction = exponent_value - lower;
        exponent = (long long)lower;
        if (fraction > 0.5 ||
            (fraction == 0.5 && (exponent & 1ll) != 0ll)) {
            exponent++;
        }
        break;
    }
    }
    if (exponent < -127) {
        return sat ? 0x00u : 0xffu;
    }
    if (exponent > 127) {
        return sat ? 0xfeu : 0xffu;
    }
    return (uint8_t)(exponent + 127);
}

static inline uint8_t
linx_tile_numeric_encode_enumerated(uint32_t dtype, double value, unsigned mode,
                                    bool sat, unsigned limit)
{
    const bool negative = signbit(value);
    uint8_t exact = UINT8_MAX, lower_raw = UINT8_MAX, upper_raw = UINT8_MAX;
    uint8_t min_raw = UINT8_MAX, max_raw = UINT8_MAX;
    double lower = -INFINITY, upper = INFINITY;
    double min_value = INFINITY, max_value = -INFINITY;

    if (isnan(value)) {
        return linx_tile_numeric_canonical_nan(dtype);
    }
    if (value == 0.0) {
        if (dtype == 6u) {
            return 0u;
        }
        if (!negative) {
            return 0u;
        }
        if (dtype == 9u || dtype == 10u) {
            return 0x20u;
        }
        return dtype == 11u || dtype == 12u || dtype == 14u ? 0x8u : 0x80u;
    }
    for (unsigned raw = 0; raw < limit; raw++) {
        double candidate = linx_tile_numeric_decode(dtype, raw, 0);
        if (!isfinite(candidate)) {
            continue;
        }
        if (candidate < min_value) {
            min_value = candidate;
            min_raw = raw;
        }
        if (candidate > max_value) {
            max_value = candidate;
            max_raw = raw;
        }
        if (candidate == value) {
            exact = raw;
        }
        if (candidate <= value && candidate > lower) {
            lower = candidate;
            lower_raw = raw;
        }
        if (candidate >= value && candidate < upper) {
            upper = candidate;
            upper_raw = raw;
        }
    }
    if (exact != UINT8_MAX) {
        return exact;
    }
    if (value < min_value || value > max_value || isinf(value)) {
        if (sat || dtype == 7u || dtype == 9u || dtype == 10u) {
            return negative ? min_raw : max_raw;
        }
        if (dtype == 6u) {
            if ((negative && (mode == 2u || mode == 4u || mode == 6u)) ||
                (!negative && (mode == 2u || mode == 3u || mode == 6u))) {
                return negative ? min_raw : max_raw;
            }
            if (isfinite(value) && fabs(value) < 40960.0) {
                return negative ? min_raw : max_raw;
            }
            return negative ? 0xefu : 0x6fu;
        }
        if (dtype == 8u) {
            if ((negative && (mode == 2u || mode == 4u || mode == 6u)) ||
                (!negative && (mode == 2u || mode == 3u || mode == 6u))) {
                return negative ? min_raw : max_raw;
            }
            if (isfinite(value) && fabs(value) < 61440.0) {
                return negative ? min_raw : max_raw;
            }
            return negative ? 0xfcu : 0x7cu;
        }
    }
    if ((mode & 7u) == 2u) {
        return negative ? upper_raw : lower_raw;
    }
    if ((mode & 7u) == 3u) {
        return lower_raw;
    }
    if ((mode & 7u) == 4u) {
        return upper_raw;
    }
    if ((mode & 7u) == 6u) {
        return (lower_raw & 1u) ? lower_raw : upper_raw;
    }
    double dl = value - lower, du = upper - value;
    if (dl < du) {
        return lower_raw;
    }
    if (du < dl) {
        return upper_raw;
    }
    if ((mode & 7u) == 5u) {
        return negative ? lower_raw : upper_raw;
    }
    if ((mode & 7u) == 7u) {
        return upper_raw;
    }
    return (lower_raw & 1u) == 0u ? lower_raw : upper_raw;
}

static inline uint64_t linx_tile_numeric_encode(uint32_t dtype, double value,
                                                unsigned mode, bool sat)
{
    dtype &= 31u;
    switch (dtype) {
    case 0u:
        if (isnan(value)) {
            return UINT64_C(0x7ff8000000000000);
        }
        return sat && isinf(value)
                   ? linx_tile_numeric_binary_max(11, 52, 52, signbit(value))
                   : linx_tile_numeric_f64_raw(value);
    case 1u:
        return linx_tile_numeric_encode_binary(value, 8, 23, 23, 127, mode,
                                               sat);
    case 2u:
        return linx_tile_numeric_encode_binary(value, 8, 10, 23, 127, mode,
                                               sat);
    case 3u:
        return linx_tile_numeric_encode_binary(value, 8, 11, 23, 127, mode,
                                               sat);
    case 4u:
        return linx_tile_numeric_encode_binary(value, 5, 10, 10, 15, mode, sat);
    case 5u:
        return linx_tile_numeric_encode_binary(value, 8, 7, 7, 127, mode, sat);
    case 6u:
    case 7u:
    case 8u:
        return linx_tile_numeric_encode_enumerated(dtype, value, mode, sat,
                                                   256u);
    case 9u:
    case 10u:
        return linx_tile_numeric_encode_enumerated(dtype, value, mode, sat,
                                                   64u);
    case 13u:
        return linx_tile_numeric_encode_e8m0(value, mode, sat);
    default:
        return 0u;
    }
}

static inline uint64_t linx_tile_numeric_encode_rne(uint32_t dtype,
                                                    double value)
{
    return linx_tile_numeric_encode(dtype, value, 1u, false);
}

static inline uint64_t linx_tile_numeric_float_to_integer(uint32_t dtype,
                                                          double value,
                                                          unsigned mode,
                                                          bool sat);

static inline uint8_t linx_tile_numeric_encode_nibble(uint32_t dtype,
                                                      double value,
                                                      unsigned mode, bool sat)
{
    if (dtype == 20u || dtype == 28u) {
        return linx_tile_numeric_float_to_integer(dtype, value, mode, sat) &
               0xfu;
    }
    return linx_tile_numeric_encode_enumerated(dtype, value, mode, sat, 16u);
}

static inline uint8_t linx_tile_numeric_encode_nibble_rne(uint32_t dtype,
                                                          double value)
{
    return linx_tile_numeric_encode_nibble(dtype, value, 1u, false);
}

static inline uint64_t linx_tile_numeric_float_to_integer(uint32_t dtype,
                                                          double value,
                                                          unsigned mode,
                                                          bool sat)
{
    unsigned bits;
    bool is_signed;
    long double minimum, maximum, rounded;

    dtype &= 31u;
    is_signed = dtype >= 16u && dtype <= 20u;
    if (!is_signed && !(dtype >= 24u && dtype <= 28u)) {
        return 0u;
    }
    bits = dtype == 16u || dtype == 24u   ? 64u
           : dtype == 17u || dtype == 25u ? 32u
           : dtype == 18u || dtype == 26u ? 16u
           : dtype == 19u || dtype == 27u ? 8u
                                          : 4u;
    minimum = is_signed ? -ldexpl(1.0L, bits - 1u) : 0.0L;
    maximum =
        is_signed ? ldexpl(1.0L, bits - 1u) - 1.0L : ldexpl(1.0L, bits) - 1.0L;
    if (isnan(value)) {
        if (sat) {
            return 0u;
        }
        if (is_signed) {
            return bits == 64u ? UINT64_C(0x8000000000000000)
                               : UINT64_C(1) << (bits - 1u);
        }
        return bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
    }
    if (isinf(value)) {
        if (!sat) {
            if (is_signed) {
                return bits == 64u ? UINT64_C(0x8000000000000000)
                                   : UINT64_C(1) << (bits - 1u);
            }
            return bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
        }
        rounded = value < 0 ? minimum : maximum;
    } else {
        rounded = (long double)value > (long double)INT64_MAX ||
                          (long double)value < (long double)INT64_MIN
                      ? truncl((long double)value)
                      : linx_tile_numeric_round_s64(value, mode);
        if (rounded < minimum || rounded > maximum) {
            if (!sat) {
                if (is_signed) {
                    return bits == 64u ? UINT64_C(0x8000000000000000)
                                       : UINT64_C(1) << (bits - 1u);
                }
                return bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
            }
            rounded = rounded < minimum ? minimum : maximum;
        }
    }
    if (is_signed) {
        return (uint64_t)(int64_t)rounded &
               (bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u);
    }
    return (uint64_t)rounded &
           (bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u);
}

static inline bool linx_tile_numeric_compare(double a, double b, unsigned mode)
{
    if (isnan(a) || isnan(b)) {
        return mode == 1u;
    }
    switch (mode) {
    case 0:
        return a == b;
    case 1:
        return a != b;
    case 2:
        return a < b;
    case 3:
        return a > b;
    case 4:
        return a <= b;
    case 5:
        return a >= b;
    default:
        return false;
    }
}

static inline uint32_t linx_tile_numeric_f32_binary(double a, double b,
                                                    unsigned op)
{
    float x = (float)a, y = (float)b, z;
    if (op == 0u) {
        z = x * y;
    } else if (op == 1u) {
        z = x + y;
    } else if (op == 2u) {
        z = (x == 0 && y == 0) ? -0.0f : fminf(x, y);
    } else {
        z = (x == 0 && y == 0) ? 0.0f : fmaxf(x, y);
    }
    return linx_tile_numeric_f32_raw(z);
}

static inline uint64_t linx_tile_numeric_encode_saturated(uint32_t dtype,
                                                          double value)
{
    return linx_tile_numeric_encode(dtype, value, 1u, true);
}

static inline unsigned linx_tile_numeric_argmax(const double *values,
                                                unsigned count)
{
    unsigned best = 0;
    for (unsigned i = 1; i < count; i++) {
        if (values[i] > values[best]) {
            best = i;
        }
    }
    return best;
}

static inline double linx_tile_numeric_reduce(const double *values,
                                              unsigned count, unsigned op)
{
    double acc = op == 0u ? NAN : 0.0;
    for (unsigned i = 0; i < count; i++) {
        if (op == 0u) {
            if (!isnan(values[i]) && (isnan(acc) || values[i] < acc)) {
                acc = values[i];
            }
        } else {
            acc = (float)acc + (float)values[i];
            if (isnan(acc)) {
                return NAN;
            }
        }
    }
    return acc;
}

static inline void linx_tile_numeric_sort_indices(const double *values,
                                                  unsigned *indices,
                                                  unsigned count, bool desc)
{
    for (unsigned i = 0; i < count; i++) {
        indices[i] = i;
    }
    for (unsigned i = 1; i < count; i++) {
        unsigned x = indices[i], j = i;
        while (j != 0u) {
            double a = values[indices[j - 1u]], b = values[x];
            bool move = isnan(a)   ? !isnan(b)
                        : isnan(b) ? false
                        : desc     ? a < b
                                   : a > b;
            if (!move) {
                break;
            }
            indices[j] = indices[j - 1u];
            j--;
        }
        indices[j] = x;
    }
}

#endif
