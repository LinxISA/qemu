#!/usr/bin/env python3
"""Regression checks for canonical PTO ISA 0.58 B.IOT forms."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
TRANSLATE = ROOT / "target/linx/translate.c"
HELPER = ROOT / "target/linx/helper.c"


class V058BIOTSourceOnlyTest(unittest.TestCase):
    def test_v058_forms_preserve_pe_mask_and_source_arity(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn(
            "const uint32_t flags = func == 4u ? 0u",
            text,
        )
        self.assertIn("func == 5u ? LINX_IOT_S1V", text)
        self.assertIn("LINX_IOT_S0V | LINX_IOT_S1V", text)
        self.assertNotIn("dst == 7u", text)
        self.assertIn(
            "linx_emit_tile_iot_desc(ctx, flags, dst, last, src0, src1, pe_mask,",
            text,
        )
        self.assertNotIn("IOTV4", text)

    def test_v058_shape_constraints_are_canonical(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn("if (pe_mask == 0u)", text)
        self.assertIn("func < 4u || func > 6u", text)
        self.assertIn("has_size && size_code > 10u", text)
        self.assertIn(
            "const uint32_t local_size_code = size_code == 0u ? 0u : size_code + 2u;",
            text,
        )

    def test_zero_size_code_descriptor_skips_tile_allocation(self) -> None:
        translate = TRANSLATE.read_text(encoding="utf-8")
        text = HELPER.read_text(encoding="utf-8")
        self.assertIn("local_size_code, has_size", translate)
        self.assertIn(
            "if (desc.has_size && (vector_block || tile_output)) {",
            text,
        )


if __name__ == "__main__":
    unittest.main()
