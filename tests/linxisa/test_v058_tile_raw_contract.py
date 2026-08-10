#!/usr/bin/env python3
"""Exact raw-encoding guards for the PTO ISA 0.58 tile contract."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target" / "linx"


class V058TileRawContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.decode = (TARGET / "insn32.decode").read_text(encoding="utf-8")
        cls.meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        cls.table = (TARGET / "tile_isa_058.h").read_text(encoding="utf-8")

    def test_vec_sfu_use_unchanged_tepl_carrier(self) -> None:
        self.assertIn("%Mode 25:2", self.decode)
        self.assertIn("%TileFunc 20:5", self.decode)
        self.assertNotIn("TileOp10", self.decode)
        # The 0.58 consumer exposes each accepted VEC/SFU selector as a direct
        # decode form.  DType remains the only wildcard in those forms.
        direct_operations = [line for line in self.decode.splitlines()
                       if line.lstrip().startswith("bstart_t") and
                       "dtype=%TileDataType" in line and
                       not line.lstrip().startswith((
                           "bstart_tepl", "bstart_tload", "bstart_tstore",
                           "bstart_tmov", "bstart_tprefetch",
                           "bstart_tmatmul", "bstart_tgemv"))]
        self.assertEqual(87, len(direct_operations))
        self.assertTrue(any(line.lstrip().startswith("bstart_tfma")
                            for line in self.decode.splitlines()))
        for retired in ("tprelu", "taxpy", "tgatherb", "tdeinterleave",
                        "tinterleave", "treshape", "tpush", "tpop",
                        "talloc", "tfree", "tpartargmax", "tpartargmin"):
            self.assertNotIn(f"bstart_{retired}", self.decode)
        self.assertIn(".mask=UINT64_C(0x7ffffff)", self.meta)
        self.assertIn(".match=UINT64_C(0x19181)", self.meta)

    def test_vec_sfu_carrier_acceptance_is_87_of_128(self) -> None:
        function_table = self.table.split("function_masks[4] = {", 1)[1]
        function_table = function_table.split("};", 1)[0]
        masks = [int(value, 16) for value in re.findall(
            r"UINT32_C\(0x([0-9a-f]{8})\)", function_table
        )]
        self.assertEqual(len(masks), 4)
        self.assertEqual(sum(mask.bit_count() for mask in masks), 87)
        self.assertEqual(128 - sum(mask.bit_count() for mask in masks), 41)

    def test_b_iot_uses_final_v058_forms(self) -> None:
        patterns = [line for line in self.decode.splitlines()
                    if line.lstrip().startswith("b_iot ")]
        self.assertEqual(len(patterns), 3)
        for field in (
            "%IOTV5Src1 26:6",
            "%IOTV5Src0 20:6",
            "%IOTV5Last 19:1",
            "%IOTV5PeMask 15:4",
            "%IOTV5TSize 9:3",
            "%IOTV5Dst 7:2",
        ):
            self.assertIn(field, self.decode)
        for mask, match in (
            ("0x707f", "0x4013"),
            ("0xfc00707f", "0x5013"),
            ("0xfff0707f", "0x6013"),
        ):
            self.assertIn(f".mask=UINT64_C({mask})", self.meta)
            self.assertIn(f".match=UINT64_C({match})", self.meta)

    def test_attribute_raw_contracts(self) -> None:
        for mask, match in (
            ("0xfbf07fff", "0x23"),
            ("0xc707f", "0x1023"),
        ):
            self.assertIn(f".mask=UINT64_C({mask})", self.meta)
            self.assertIn(f".match=UINT64_C({match})", self.meta)
        self.assertIn("%BA_Canonicalize 25:1", self.decode)
        self.assertIn("%BA_PadValue 27:2", self.decode)
        self.assertIn("UINT32_C(0x1f1f7fff)", self.table)
        self.assertIn("UINT32_C(0x5816035b)", self.table)

    def test_retired_tlsu_compatibility_is_deleted(self) -> None:
        self.assertNotIn("TCVT_COMPAT", self.helper)

    def test_cube_is_fail_closed_to_13_functions(self) -> None:
        self.assertIn("UINT32_C(0x00770177)", self.table)
        cube_names = (
            "tmatmul", "tmatmul_bias", "tmatmul_acc", "tmatmulmx",
            "tmatmulmx_bias", "tmatmulmx_acc", "tgemv", "tgemv_bias",
            "tgemv_acc", "tgemvmx", "tgemvmx_bias", "tgemvmx_acc",
            "acccvt",
        )
        for name in cube_names:
            self.assertRegex(self.decode, rf"(?m)^\s*bstart_{name}\s")
            self.assertIn(f"trans_bstart_{name}", self.translate)


if __name__ == "__main__":
    unittest.main()
