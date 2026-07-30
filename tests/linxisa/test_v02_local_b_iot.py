#!/usr/bin/env python3
"""Regression checks for the DavinciOO PTO ISA v0.2 Local B.IOT ABI."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
TRANSLATE = ROOT / "target/linx/translate.c"
HELPER = ROOT / "target/linx/helper.c"


class V02LocalBIOTTest(unittest.TestCase):
    def test_v5_fields_replace_legacy_reuse_layout(self) -> None:
        decode = (ROOT / "target/linx/insn32.decode").read_text(
            encoding="utf-8"
        )
        for field in (
            "%IOTV5Src1 26:6",
            "%IOTV5Src0 20:6",
            "%IOTV5Last 19:1",
            "%IOTV5PeMask 15:4",
            "%IOTV5TSize 9:3",
            "%IOTV5Dst 7:2",
        ):
            self.assertIn(field, decode)
        self.assertNotIn("%IOTS0R", decode)
        self.assertNotIn("%IOTS1R", decode)

    def test_only_full_mask_is_executable_in_single_pe_model(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn(
            "if (pe_mask != 0xfu || func < 4u || func > 6u || tsize > 7u)",
            text,
        )

    def test_tsize_zero_suppresses_output_allocation(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn(
            "tsize != 0u);",
            text,
        )

    def test_local_sources_retain_producer_age(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        self.assertRegex(
            text,
            re.compile(
                r"DavinciOO v5 Local source use never pops producer age\."
                r".*?linx_tile_release_source\(live, order, count_by_hand, tile, true,",
                re.S,
            ),
        )
        self.assertNotIn("LINX_IOT_S0R", text)
        self.assertNotIn("LINX_IOT_S1R", text)


if __name__ == "__main__":
    unittest.main()
