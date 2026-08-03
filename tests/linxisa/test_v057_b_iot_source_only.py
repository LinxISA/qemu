#!/usr/bin/env python3
"""Regression checks for canonical PTO ISA 0.57.1 B.IOT forms."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
TRANSLATE = ROOT / "target/linx/translate.c"
HELPER = ROOT / "target/linx/helper.c"


class V057BIOTSourceOnlyTest(unittest.TestCase):
    def test_source_only_forms_have_no_magic_destination(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn(
            "const uint32_t flags = func == 4u ? 0u",
            text,
        )
        self.assertIn("func == 5u ? LINX_IOT_S1V", text)
        self.assertIn("LINX_IOT_S0V | LINX_IOT_S1V", text)
        self.assertNotIn("dst == 7u", text)

    def test_output_constraints_are_canonical(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn(
            "pe_mask == 0u || func < 4u || func > 6u || tsize > 7u",
            text,
        )

    def test_source_only_descriptor_skips_tile_allocation(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        self.assertIn(
            "if (desc.has_size && (vector_block || tile_output)) {",
            text,
        )


if __name__ == "__main__":
    unittest.main()
