#!/usr/bin/env python3
"""QEMU-side LinxISA 0.58 decode and metadata contract checks."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target/linx"


class V058DecodeMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.insn16 = (TARGET / "insn16.decode").read_text(encoding="utf-8")
        cls.insn32 = (TARGET / "insn32.decode").read_text(encoding="utf-8")
        cls.ids = (TARGET / "linx_opcode_ids_gen.h").read_text(encoding="utf-8")
        cls.meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")

    def test_final_tlsu_surface_is_present(self) -> None:
        for decode_name in (
            "bstart_tload",
            "bstart_tstore",
            "bstart_tmov",
            "bstart_tprefetch",
            "bstart_mgather",
            "bstart_mscatter",
            "bstart_mgather_mask",
            "bstart_mscatter_mask",
            "bstart_mgather_cas",
            "bstart_gmov",
        ):
            self.assertIn(decode_name, self.insn32)
            self.assertIn(f'.mnemonic="{decode_name}"', self.meta)

    def test_b_ios_replaces_deleted_compressed_form(self) -> None:
        self.assertNotIn("c_b_ios", self.insn16)
        self.assertNotIn("c_b_ios", self.meta)
        self.assertIn("LINX_OP_B_IOS = 638", self.ids)
        self.assertIn('.mnemonic="b_ios"', self.meta)
        self.assertIn(".mask=UINT64_C(0xf00871ff)", self.meta)
        self.assertIn(".match=UINT64_C(0x1013)", self.meta)

    def test_vec_sfu_and_tfma_metadata_are_final(self) -> None:
        self.assertIn("LINX_OP_BSTART_TLSU = 22", self.ids)
        self.assertNotIn("LINX_OP_BSTART_TMA", self.ids)
        self.assertIn('.mnemonic="bstart_tfma"', self.meta)
        self.assertIn(".match=UINT64_C(0x1c19181)", self.meta)
        for retired in (
            "bstart_talloc",
            "bstart_taxpy",
            "bstart_tdeinterleave",
            "bstart_tfree",
            "bstart_tgatherb",
            "bstart_tinterleave",
            "bstart_tpartargmax",
            "bstart_tpartargmin",
            "bstart_tpop",
            "bstart_tprelu",
            "bstart_tpush",
            "bstart_treshape",
        ):
            self.assertNotIn(retired, self.meta)

    def test_internal_engine_names_do_not_restore_tma(self) -> None:
        self.assertIn("LINX_BLOCK_TLSU", self.helper)
        self.assertIn("LINX_BLOCK_OPERATION", self.helper)
        self.assertNotIn("LINX_BLOCK_TMA", self.helper)
        self.assertNotIn('return "tma"', self.helper)


if __name__ == "__main__":
    unittest.main()
