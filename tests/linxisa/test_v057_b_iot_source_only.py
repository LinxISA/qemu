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
            "const bool has_output = form == 6u || a->imm4 != 0u || "
            "a->dst != 0u;",
            text,
        )
        self.assertNotIn("dst == 7u", text)

    def test_output_constraints_are_canonical(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn(
            "has_output && (dst > 3u || size < 3u || size > 9u)", text
        )

    def test_source_only_descriptor_skips_tile_allocation(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        self.assertIn(
            "if (desc.has_size && (vector_block || tile_output)) {",
            text,
        )


if __name__ == "__main__":
    unittest.main()
