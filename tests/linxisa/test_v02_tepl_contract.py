#!/usr/bin/env python3
"""DavinciOO PTO ISA v0.2 TEPL encoding contract checks."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "target/linx/helper.c"
DECODE = ROOT / "target/linx/insn32.decode"
TRANSLATE = ROOT / "target/linx/translate.c"
META = ROOT / "target/linx/linx_opcode_meta_gen.h"


class V02TeplContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.helper = HELPER.read_text(encoding="utf-8")
        cls.decode = DECODE.read_text(encoding="utf-8")

    def test_collision_prone_selectors_use_latest_identity(self) -> None:
        expected = {
            "LINX_TEPL_TMAX": "0x0b",
            "LINX_TEPL_TMIN": "0x0c",
            "LINX_TEPL_TCMP": "0x0d",
            "LINX_TEPL_TCVT": "0x1b",
            "LINX_TEPL_TROWSUM": "0x40",
            "LINX_TEPL_TROWMAX": "0x41",
            "LINX_TEPL_TROWMIN": "0x42",
            "LINX_TEPL_TROWPROD": "0x43",
        }
        for name, value in expected.items():
            self.assertRegex(self.helper, rf"\b{name}\s*=\s*{value}\b")

    def test_bstart_tepl_decodes_mode_and_function(self) -> None:
        self.assertIn(
            "bstart_tepl       .... .... .... 0001 1001 0001 1000 0001 "
            "dtype=%TileDataType mode=%Mode func=%TileFunc",
            self.decode,
        )

    def test_latest_fp16_and_bf16_dtype_values(self) -> None:
        self.assertRegex(
            self.helper, r"\bLINX_TILE_DTYPE_FP16\s*=\s*4\b"
        )
        self.assertRegex(
            self.helper, r"\bLINX_TILE_DTYPE_BF16\s*=\s*5\b"
        )
        self.assertNotRegex(self.helper, r"case 2u:\s*/\* FP16 \*/")
        self.assertNotRegex(self.helper, r"case 6u:\s*/\* BF16 \*/")

    def test_tcvt_result_dtype_is_selected_only_by_tcvt_identity(self) -> None:
        self.assertRegex(
            self.helper,
            re.compile(
                r"result_dtype\s*=\s*op == LINX_TEPL_TCVT"
                r"\s*\? env->tile_attr_dtype\s*:\s*env->tile_dtype",
                re.S,
            ),
        )

    def test_obsolete_tepl_selectors_are_not_executable(self) -> None:
        start = self.helper.index("static bool linx_tile_tepl_selector_executable")
        end = self.helper.index("#define LINX_TILE_DTYPE_MASK", start)
        whitelist = self.helper[start:end]
        for obsolete in ("0x01cu", "0x08au", "0x08bu", "0x0c7u", "0x0c8u"):
            self.assertNotIn(obsolete, whitelist)

    def test_b_datr_uses_latest_physical_fields(self) -> None:
        for field in (
            "%BA_CMode 29:3",
            "%BA_PadValue 27:2",
            "%BA_Sat 26:1",
            "%BA_C 25:1",
            "%BA_DataType 20:5",
            "%BA_RMode 15:3",
            "%BA_DataLayout 7:5",
        ):
            self.assertIn(field, self.decode)

    def test_removed_function_eight_identities_fail_closed(self) -> None:
        translate = TRANSLATE.read_text(encoding="utf-8")
        meta = META.read_text(encoding="utf-8")
        self.assertNotIn('mnemonic="bstart_mgather_cas"', meta)
        self.assertIn("if (a->func == 8u)", translate)


if __name__ == "__main__":
    unittest.main()
