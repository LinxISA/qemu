#!/usr/bin/env python3
"""Regression checks for explicit and inferred MSCATTER transfer sizes."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "target/linx/helper.c"


class V02MScatterSizeTest(unittest.TestCase):
    def test_explicit_size_wins_over_data_tile_size(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        helper = text[
            text.index("static bool linx_tile_indexed_tma_size") :
            text.index("static bool linx_tile_transfer_preflight")
        ]
        self.assertIn("if (!d.has_size)", helper)
        self.assertIn("explicit_size = true;", helper)
        self.assertRegex(
            helper,
            re.compile(
                r"if \(!explicit_size &&\s*\(func == LINX_TMA_MSCATTER.*?"
                r"size_code = \(unsigned\)__builtin_ctz\(bytes\) - 4u;",
                re.S,
            ),
        )

    def test_preflight_and_commit_share_size_selection(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        preflight = text[
            text.index("static bool linx_tile_preflight_tma") :
            text.index("void HELPER(linx_tile_reset_block)")
        ]
        commit = text[
            text.index("void HELPER(linx_tile_commit)") :
            text.index("static void linx_tile_commit_vector_bindings")
        ]
        call = "linx_tile_indexed_tma_size(env, func, sources, source_count,"
        self.assertIn(call, preflight)
        self.assertIn(call, commit)


if __name__ == "__main__":
    unittest.main()
