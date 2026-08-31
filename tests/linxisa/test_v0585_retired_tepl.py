#!/usr/bin/env python3
"""Source guards for TEPL selectors retired by PTO ISA 0.58.5."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target/linx"
RETIRED = {
    "tfillpad": "0x065u",
    "ttrans": "0x06eu",
    "tpartadd": "0x071u",
    "tpartmul": "0x072u",
    "tpartmax": "0x073u",
    "tpartmin": "0x074u",
}


class RetiredTeplSelectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.decode = (TARGET / "insn32.decode").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")
        cls.meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
        cls.docs = (ROOT / "docs/linxisa/pto-tile-support.md").read_text(
            encoding="utf-8"
        )

    def test_retired_names_have_no_decode_or_translation_entry(self) -> None:
        for name in RETIRED:
            self.assertNotIn(f"bstart_{name}", self.decode)
            self.assertNotIn(f"bstart_{name}", self.meta)
            self.assertNotIn(f"LINX_TRANS_TILE_OPERATION_DIRECT({name}", self.translate)

    def test_retired_selectors_cannot_reach_private_implementations(self) -> None:
        mapping = self.helper[
            self.helper.index("static uint32_t linx_tile_operation_impl_selector") :
            self.helper.index("static bool linx_tile_value_reduction_output_shape")
        ]
        inventory = self.helper[
            self.helper.index("static bool linx_tile_operation_selector_executable") :
            self.helper.index("#define LINX_TILE_DTYPE_MASK")
        ]
        for name, selector in RETIRED.items():
            label = f"case {selector}: /* {name.upper()} */"
            self.assertNotIn(label, mapping)
            self.assertNotIn(label, inventory)

    def test_documentation_marks_the_selectors_retired(self) -> None:
        for name in RETIRED:
            self.assertIn(f"`{name.upper()}`", self.docs)
        self.assertIn("former selector values are reserved", self.docs)


if __name__ == "__main__":
    unittest.main()
