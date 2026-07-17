#!/usr/bin/env python3
"""Regression checks for v0.57 source-only B.IOT descriptor semantics."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
TRANSLATE = ROOT / "target/linx/translate.c"
HELPER = ROOT / "target/linx/helper.c"


class V057BIOTSourceOnlyTest(unittest.TestCase):
    def test_dst_tile_seven_and_zero_size_suppress_output(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertRegex(
            text,
            re.compile(
                r"const bool has_output = !\(dst == 7u && size == 0u\);"
                r".*?linx_emit_tile_iot_desc\(ctx, flags, dst, last, src0, src1,"
                r" 0, size,\s*has_output\);",
                re.S,
            ),
        )

    def test_source_only_descriptor_skips_tile_allocation(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        self.assertIn(
            "if (desc.has_size && (vector_block || tile_output)) {",
            text,
        )


if __name__ == "__main__":
    unittest.main()
