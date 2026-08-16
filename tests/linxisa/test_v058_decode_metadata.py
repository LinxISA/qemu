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
        cls.insn64 = (TARGET / "insn64.decode").read_text(encoding="utf-8")
        cls.ids = (TARGET / "linx_opcode_ids_gen.h").read_text(encoding="utf-8")
        cls.meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")

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

        self.assertNotIn("bstart_acccvt", self.insn32)
        self.assertNotIn('.mnemonic="bstart_acccvt"', self.meta)

    def test_shared_tmov_source_forms_decode_to_distinct_functions(self) -> None:
        expected = {
            "bstart_tmov_l2s_insert": ("1001", "0x911181"),
            "bstart_tmov_l2s_publish": ("1010", "0xa11181"),
            "bstart_tmov_s2l_broadcast": ("1011", "0xb11181"),
            "bstart_tmov_s2l_extract": ("1100", "0xc11181"),
        }
        for decode_name, (function_bits, match) in expected.items():
            self.assertRegex(
                self.insn32,
                rf"(?m)^\s*{decode_name}\s+\.\.\.\.\s+\.000\s+"
                rf"{function_bits}\s+0001\s+0001\s+0001\s+1000\s+0001\b",
            )
            self.assertIn(f'.mnemonic="{decode_name}"', self.meta)
            self.assertIn(f".match=UINT64_C({match})", self.meta)

    def test_b_iot_uses_all_exact_public_forms(self) -> None:
        patterns = (
            "0000 00.. .... .... .101 .... .001 0011",
            ".... .... .... .... .100 000. .001 0011",
            ".... .... .... .... .100 .... .001 0011",
            "0000 00.. .... .... .101 000. .001 0011",
            "0000 0000 0000 .... .110 .... .001 0011",
        )
        b_iot_lines = tuple(
            " ".join(line.split())
            for line in self.insn32.splitlines()
            if line.strip().startswith("b_iot ")
        )
        self.assertEqual(len(b_iot_lines), len(patterns))
        for pattern in patterns:
            self.assertTrue(
                any(line.startswith(f"b_iot {pattern}") for line in b_iot_lines),
                pattern,
            )

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

    def test_v0581_new_exact_forms_are_decoded(self) -> None:
        expected = {
            "b_fpatr": ("0x7fff", "0x2023"),
            "start_call_32": ("0xf83f000f", "0x50160002"),
            "start_icall_32": ("0xf83fffff", "0x50166001"),
            "l_bstop": ("0xffffffffffffffff", "0x10000000f"),
        }
        for decode_name, (mask, match) in expected.items():
            with self.subTest(decode_name=decode_name):
                self.assertIn(f'.mnemonic="{decode_name}"', self.meta)
                self.assertIn(f".mask=UINT64_C({mask})", self.meta)
                self.assertIn(f".match=UINT64_C({match})", self.meta)

        self.assertRegex(self.insn32, r"(?m)^b_fpatr\s+")
        self.assertRegex(self.insn32, r"(?m)^start_icall_32\s+")
        self.assertRegex(self.insn64, r"(?m)^l_bstop\s+")

    def test_retired_32bit_bare_call_forms_are_illegal(self) -> None:
        retired = (
            "bstart_call",
            "bstart_icall",
            "bstart_fp_call",
            "bstart_fp_icall",
        )
        for decode_name in retired:
            with self.subTest(decode_name=decode_name):
                self.assertNotRegex(self.insn32, rf"(?m)^{decode_name}\s+")
                self.assertNotIn(f'.mnemonic="{decode_name}"', self.meta)
                self.assertNotIn(f"trans_{decode_name}(", self.translate)

        self.assertNotIn("LINX_OP_BSTART_FP_CALL", self.ids)
        self.assertNotIn("LINX_OP_BSTART_FP_ICALL", self.ids)
        self.assertNotIn("LINX_OP_BSTART_ICALL", self.ids)
        self.assertIn("LINX_OP_BSTART_STD_CALL = 17", self.ids)
        self.assertIn('.mnemonic="hl_bstart_std_call"', self.meta)


if __name__ == "__main__":
    unittest.main()
