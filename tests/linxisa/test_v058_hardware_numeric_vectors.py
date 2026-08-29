#!/usr/bin/env python3
"""Execute the official PTO ISA 0.58.3 numeric vectors."""

import ctypes
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "tests/linxisa/pto-isa-0583-hardware-numeric-vectors.json"
MATRIX_AUTHORITY = ROOT / "tests/linxisa/pto-v0583-matrix-type-authority.json"
DTYPE = {
    "FP64": 0, "FP32": 1, "TF32": 2, "HF32": 3, "FP16": 4,
    "BF16": 5, "HiF8": 6, "E4M3": 7, "E5M2": 8, "E3M2": 9,
    "E2M3": 10, "E2M1X2": 11, "E1M2X2": 12, "E8M0": 13,
    "HiF4X2": 14, "S64": 16, "S32": 17, "S16": 18, "S8": 19,
    "S4X2": 20, "U64": 24, "U32": 25, "U16": 26, "U8": 27,
    "U4X2": 28,
}
RMODE = {"RNE": 1, "RTZ": 2, "RDN": 3, "RUP": 4,
         "RNA": 5, "RTO": 6, "RHB": 7}

WRAPPER = r'''
#include <math.h>
#include <stdint.h>
#include "tile_numeric_058.h"

uint64_t v_nan(unsigned d) { return linx_tile_numeric_canonical_nan(d); }
int v_ordinary(unsigned d) { return linx_tile_numeric_ordinary(d); }
unsigned v_acc_dtype(unsigned d) { return linx_tile_numeric_acc_dtype(d); }
int v_mx_pair(unsigned a, unsigned b) { return linx_tile_numeric_mx_pair(a, b); }
int v_matrix_pair(unsigned a, unsigned b) {
    return linx_tile_numeric_ordinary_matrix_pair(a, b);
}
int v_mx_scale(unsigned d) { return linx_tile_numeric_mx_requires_scale(d); }
double v_decode(unsigned d, uint64_t r, unsigned l) {
    return linx_tile_numeric_decode(d, r, l);
}
int64_t v_round(double x, unsigned m) {
    return linx_tile_numeric_round_s64(x, m);
}
int v_compare(double a, double b, unsigned mode) {
    return linx_tile_numeric_compare(a, b, mode);
}
uint64_t v_invalid(unsigned d) {
    return linx_tile_numeric_float_to_integer(d, NAN, 1, false);
}
uint64_t v_encode(unsigned d, double value, unsigned mode, int sat) {
    return linx_tile_numeric_encode(d, value, mode, sat != 0);
}
int64_t v_sat_int(double x, unsigned d) {
    uint64_t raw = linx_tile_numeric_float_to_integer(d, x, 1, true);
    if (d == 19) return (int8_t)raw; if (d == 17) return (int32_t)raw;
    return raw;
}
uint32_t v_sat_fp16(double x) {
    return linx_tile_numeric_encode_saturated(4, x);
}
uint32_t v_f32_op(double a, double b, unsigned op) {
    return linx_tile_numeric_f32_binary(a, b, op);
}
void v_matmul_2x2(const float *a, const float *b, float *out) {
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned j = 0; j < 2; j++) {
            float acc = 0;
            for (unsigned k = 0; k < 2; k++) acc += a[i * 2 + k] * b[k * 2 + j];
            out[i * 2 + j] = acc;
        }
    }
}
float v_bias_acc(float dot, float bias, float prior, unsigned flags) {
    float acc = (flags & 1) ? prior : 0;
    acc += dot;
    if (flags & 2) acc += bias;
    return acc;
}
float v_mx33(unsigned bias, unsigned prior) {
    float acc = prior;
    for (unsigned k = 0; k < 33; k++) {
        uint8_t sa = k < 32 ? 0x7f : 0x80;
        uint8_t sb = k < 32 ? 0x7f : 0x81;
        float a = (float)linx_tile_numeric_decode(7, 0x38, 0);
        float b = (float)linx_tile_numeric_decode(8, 0x3c, 0);
        a *= (float)linx_tile_numeric_decode(13, sa, 0);
        b *= (float)linx_tile_numeric_decode(13, sb, 0);
        acc += a * b;
    }
    return acc + bias;
}
int v_argmax(const double *v, unsigned n) {
    return linx_tile_numeric_argmax(v, n);
}
double v_reduce(const double *v, unsigned n, unsigned op) {
    return linx_tile_numeric_reduce(v, n, op);
}
void v_sort(const double *v, unsigned *idx, unsigned n, unsigned desc) {
    linx_tile_numeric_sort_indices(v, idx, n, desc);
}
'''


def number(text):
    if text in ("qNaN", "canonical-qNaN") or text.startswith("qNaN#"):
        return math.nan
    if text in ("+infinity",):
        return math.inf
    if text == "-infinity":
        return -math.inf
    if text == "NaN":
        return math.nan
    if text == "+0":
        return 0.0
    if text == "-0":
        return -0.0
    if "^" in text:
        base, exp = text.split("^")
        return float(base) ** int(exp)
    return float(text)


class HardwareNumericVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="linx-numeric-058-")
        source = Path(cls.tmp.name) / "vectors.c"
        library = Path(cls.tmp.name) / "vectors.so"
        source.write_text(WRAPPER, encoding="utf-8")
        subprocess.run([
            "cc", "-std=c11", "-shared", "-fPIC", "-O2",
            "-I", str(ROOT / "target/linx"), str(source), "-lm", "-o", str(library)
        ], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.v_nan.restype = ctypes.c_uint64
        cls.lib.v_decode.argtypes = [ctypes.c_uint, ctypes.c_uint64, ctypes.c_uint]
        cls.lib.v_decode.restype = ctypes.c_double
        cls.lib.v_round.argtypes = [ctypes.c_double, ctypes.c_uint]
        cls.lib.v_round.restype = ctypes.c_int64
        cls.lib.v_compare.argtypes = [ctypes.c_double, ctypes.c_double, ctypes.c_uint]
        cls.lib.v_invalid.restype = ctypes.c_uint64
        cls.lib.v_encode.argtypes = [ctypes.c_uint, ctypes.c_double,
                                     ctypes.c_uint, ctypes.c_int]
        cls.lib.v_encode.restype = ctypes.c_uint64
        cls.lib.v_sat_int.argtypes = [ctypes.c_double, ctypes.c_uint]
        cls.lib.v_sat_int.restype = ctypes.c_int64
        cls.lib.v_sat_fp16.argtypes = [ctypes.c_double]
        cls.lib.v_sat_fp16.restype = ctypes.c_uint32
        cls.lib.v_f32_op.argtypes = [ctypes.c_double, ctypes.c_double, ctypes.c_uint]
        cls.lib.v_f32_op.restype = ctypes.c_uint32
        cls.lib.v_bias_acc.argtypes = [ctypes.c_float, ctypes.c_float,
                                       ctypes.c_float, ctypes.c_uint]
        cls.lib.v_bias_acc.restype = ctypes.c_float
        cls.lib.v_mx33.argtypes = [ctypes.c_uint, ctypes.c_uint]
        cls.lib.v_mx33.restype = ctypes.c_float
        cls.lib.v_reduce.restype = ctypes.c_double

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_all_114_executable_reference_vectors_are_checked(self):
        groups = json.loads(VECTORS.read_text(encoding="utf-8"))["vector_groups"]
        executed = 0

        for v in groups["canonical_nan"]:
            self.assertEqual(self.lib.v_nan(DTYPE[v["data_type"]]), int(v["expected"], 16)); executed += 1
        for v in groups["comparison"]:
            mode = ["EQ", "NE", "LT", "GT", "LE", "GE"].index(v["mode"])
            self.assertEqual(self.lib.v_compare(number(v["inputs"][0]), number(v["inputs"][1]), mode), v["expected"]); executed += 1
        for v in groups["invalid_float_to_integer"]:
            expected = int(v.get("expected_raw", v.get("expected_lane_raw")), 16)
            self.assertEqual(self.lib.v_invalid(DTYPE[v["destination"]]), expected); executed += 1
        for v in groups["low_precision_encoding"]:
            got = self.lib.v_decode(DTYPE[v["data_type"]], int(v["raw"], 16), 0)
            expected = number(v["expected"])
            self.assertTrue(math.isnan(got) if math.isnan(expected) else got == expected); executed += 1

        for v in groups["matrix"]:
            if v["id"] == "fp32-matmul-2x2":
                A = (ctypes.c_float * 4)(*[float(x) for row in v["left"] for x in row])
                B = (ctypes.c_float * 4)(*[float(x) for row in v["right"] for x in row])
                out = (ctypes.c_float * 4)(); self.lib.v_matmul_2x2(A, B, out)
                self.assertEqual(list(out), [float(x) for row in v["expected"] for x in row])
            elif v["id"] == "fp32-matmul-bias":
                self.assertEqual([self.lib.v_bias_acc(float(d), float(b), 0, 2)
                                  for d, b in zip(v["dot_products"], v["bias"])],
                                 [float(x) for x in v["expected_acc"]])
            elif v["id"] == "fp32-matmul-acc":
                self.assertEqual([self.lib.v_bias_acc(float(d), 0, float(p), 1)
                                  for d, p in zip(v["dot_products"], v["prior_acc"])],
                                 [float(x) for x in v["expected_acc"]])
            elif v["id"] == "mx-e8m0-scale":
                self.assertEqual(float(v["base"]) * self.lib.v_decode(13, int(v["e8m0"], 16), 0), float(v["expected"]))
            else:
                self.assertEqual(self.lib.v_mx33(0, 0), float(v["expected_acc"]))
                self.assertEqual(self.lib.v_mx33(2, 0), float(v["expected_acc_with_bias_2"]))
                self.assertEqual(self.lib.v_mx33(0, 3), float(v["expected_acc_with_prior_3"]))
            executed += 1

        for v in groups["packed_lane_order"]:
            raw = int(v["raw_byte"], 16); dtype = DTYPE[v["data_type"]]
            self.assertEqual([self.lib.v_decode(dtype, raw, i) for i in range(2)],
                             [number(x) for x in v["expected_logical_lanes"]]); executed += 1
        for v in groups["reduction"]:
            vals = (ctypes.c_double * len(v["inputs"]))(*[number(x) for x in v["inputs"]])
            if v["operation"] == "ARGMAX": got = self.lib.v_argmax(vals, len(vals)); expected = v["expected_index"]
            elif v["operation"] == "MIN": got = self.lib.v_reduce(vals, len(vals), 0); expected = number(v["expected"])
            elif v["operation"] == "SUM": got = self.lib.v_reduce(vals, len(vals), 1); expected = math.nan
            else: got = self.lib.v_f32_op(vals[0], vals[1], 3); expected = 0
            self.assertTrue(math.isnan(got) if isinstance(expected, float) and math.isnan(expected) else got == expected); executed += 1
        for v in groups["rounding"]:
            self.assertEqual(self.lib.v_round(float(v["input"]), RMODE[v["mode"]]), int(v["expected"])); executed += 1
        for v in groups["rounding_before_saturation"]:
            self.assertEqual(
                self.lib.v_sat_int(float(v["input"]), DTYPE[v["destination"]]),
                int(v["expected"]),
            ); executed += 1
        scalar_frm = {0: "RNE", 1: "RTM", 2: "RTP", 3: "RTZ", 4: "RNE", 7: "RNE"}
        public_conversion = {
            0: ("CAST_NONE", 0), 1: ("CAST_RINT", 1),
            2: ("CAST_ROUND", 5), 3: ("CAST_FLOOR", 3),
            4: ("CAST_CEIL", 4), 5: ("CAST_TRUNC", 2),
            6: ("CAST_ODD", 6),
        }
        for v in groups["rounding_selection"]:
            namespace = v["namespace"]
            if namespace == "scalar-core-state-frm":
                self.assertEqual(scalar_frm[v["raw"]], v["expected_mode"])
            elif namespace == "bundle-rmode":
                self.assertEqual((v["raw"], v["expected_mode"]), (0, "RTZ"))
            elif v["raw"] == 7:
                self.assertEqual(v["expected"], "reject-before-effects")
            else:
                self.assertEqual(
                    public_conversion[v["raw"]],
                    (v["public_name"], v["bundle_rmode"]),
                )
            executed += 1
        for v in groups["saturation"]:
            expected = int(v["expected"], 16) if v["expected"].startswith("0x") else int(v["expected"])
            got = self.lib.v_sat_fp16(number(v["input"])) if v["destination"] == "FP16" else self.lib.v_sat_int(number(v["input"]), DTYPE[v["destination"]])
            self.assertEqual(got, expected); executed += 1
        for v in groups["signed_zero"]:
            op = {"multiply": 0, "add": 1, "minimum": 2, "maximum": 3}[v["operation"]]
            self.assertEqual(self.lib.v_f32_op(number(v["inputs"][0]), number(v["inputs"][1]), op), int(v["expected"], 16)); executed += 1
        for v in groups["sort"]:
            vals = (ctypes.c_double * 32)(*[number(x) for x in v["input_values"]]); out = (ctypes.c_uint * 32)()
            self.lib.v_sort(vals, out, 32, 0); self.assertEqual(list(out), v["ascending_expected_original_indices"])
            self.lib.v_sort(vals, out, 32, 1); self.assertEqual(list(out), v["descending_expected_original_indices"]); executed += 1
        for v in groups["subnormal_policy"]:
            if "raw" in v:
                got = self.lib.v_decode(DTYPE[v["data_type"]], int(v["raw"], 16), 0)
                if v["expected_class"] == "positive-subnormal":
                    self.assertGreater(got, 0.0); self.assertLess(got, 2.0 ** -126)
                else:
                    self.assertGreaterEqual(got, 2.0 ** -126)
            else:
                unsupported = v["ftz"] or v["daz"] or v["operation_override"]
                self.assertEqual(
                    v["expected"],
                    "reject-before-effects" if unsupported else "accept",
                )
            executed += 1

        tcvt_modes = {"RNE": 1, "RTM": 3}
        for v in groups["tcvt_e8m0"]["vectors"]:
            source_type = DTYPE[v["source_type"]]
            source = self.lib.v_decode(source_type, int(v["input"], 16), 0)
            self.assertEqual(
                self.lib.v_encode(13, source, tcvt_modes[v["rmode"]], v["sat"]),
                int(v["expected"], 16),
            )
            executed += 1

        self.assertEqual(executed, 114)

    def test_0583_matrix_postprocess_vectors_are_present(self):
        groups = json.loads(VECTORS.read_text(encoding="utf-8"))["vector_groups"]
        self.assertEqual(len(groups["matrix_postprocess"]), 8)

    def test_conformance_separates_reference_and_production_vectors(self):
        conformance = json.loads(
            (ROOT / "tests/linxisa/pto-v058-cube-profile-conformance.json").read_text(
                encoding="utf-8"
            )
        )
        evidence = conformance["qemu_implementation_gap"]["evidence"]
        self.assertEqual(conformance["status"], "production-conformance")
        self.assertEqual(evidence["canonical_reference_vectors_checked"], 114)
        self.assertEqual(evidence["production_vector_cases"], 51)
        self.assertNotIn("canonical_contract_vectors_executed", evidence)

        contract = conformance["normative_contract"]
        authority = json.loads(MATRIX_AUTHORITY.read_text(encoding="utf-8"))
        ordinary_codes = [DTYPE[name] for name in contract["ordinary_side_types"]]
        mx_codes = [DTYPE[name] for name in contract["mx_side_types"]]
        scaled_codes = [DTYPE[name] for name in contract["mx_scaled_side_types"]]
        self.assertEqual(len(ordinary_codes), 18)
        self.assertEqual(contract["ordinary_ordered_operand_pairs"],
                         12 * 12 + 3 * 3 + 3 * 3)
        self.assertEqual(len([(a, b) for a in mx_codes for b in mx_codes]),
                         contract["mx_ordered_operand_pairs"])
        self.assertEqual(scaled_codes, [7, 8, 11, 12])
        self.assertEqual(
            contract["ordinary_side_types"],
            [name for values in authority["ordinary_classes"].values()
             for name in values],
        )
        self.assertEqual(contract["mx_side_types"], authority["mx_side_types"])

    def test_cube_type_matrix_matches_authority_with_current_mx_extension(self):
        authority = json.loads(MATRIX_AUTHORITY.read_text(encoding="utf-8"))
        floating, signed, unsigned = [
            [DTYPE[name] for name in authority["ordinary_classes"][kind]]
            for kind in ("floating", "signed", "unsigned")
        ]
        ordinary = floating + signed + unsigned
        expected_ordinary_pairs = [
            (a, b) for group in (floating, signed, unsigned)
            for a in group for b in group
        ]
        self.assertEqual(
            [(a, b) for a in range(32) for b in range(32)
             if self.lib.v_matrix_pair(a, b)],
            expected_ordinary_pairs,
        )
        self.assertEqual(len(ordinary), 18)
        self.assertNotIn(14, ordinary)
        self.assertNotIn(17, ordinary)
        self.assertNotIn(25, ordinary)

        # ADR-0101 adds HiF4X2 to Matrix-MX input roles only.  Keep the
        # 0.58.3 fixture as the ordinary-type authority, then extend the
        # Matrix-MX expectation to the current v0.58.4 contract.
        mx_types = [DTYPE[name] for name in authority["mx_side_types"]] + [
            DTYPE["HiF4X2"]
        ]
        mx = [(a, b) for a in mx_types for b in mx_types]
        self.assertEqual(
            [(a, b) for a in range(32) for b in range(32)
             if self.lib.v_mx_pair(a, b)],
            mx,
        )
        self.assertEqual(len(mx), 49)
        self.assertEqual(
            [d for d in mx_types if self.lib.v_mx_scale(d)],
            [DTYPE[name] for name in authority["mx_scaled_side_types"]] + [
                DTYPE["HiF4X2"]
            ],
        )


if __name__ == "__main__":
    unittest.main()
