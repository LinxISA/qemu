#!/usr/bin/env python3
"""QEMU-side LinxISA 0.58 decode and metadata contract checks."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target/linx"


class V058DecodeMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.insn16 = (TARGET / "insn16.decode").read_text(encoding="utf-8")
        cls.insn32 = (TARGET / "insn32.decode").read_text(encoding="utf-8")
        cls.insn48 = (TARGET / "insn48.decode").read_text(encoding="utf-8")
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
            "bstart_mgather_exch",
            "bstart_mgather_max",
            "bstart_mgather_min",
            "bstart_mgather_add",
            "bstart_gmov",
            "bstart_mgather_inc",
            "bstart_mgather_dec",
            "bstart_mgather_and",
            "bstart_mgather_or",
            "bstart_mgather_xor",
            "bstart_mscatter_max",
            "bstart_mscatter_min",
            "bstart_mscatter_add",
            "bstart_mscatter_inc",
            "bstart_mscatter_dec",
            "bstart_mscatter_and",
            "bstart_mscatter_or",
            "bstart_mscatter_xor",
            "bstart_mscatter_popc",
            "bstart_timg2col",
        ):
            self.assertIn(decode_name, self.insn32)
            self.assertIn(f'.mnemonic="{decode_name}"', self.meta)

        self.assertNotIn("bstart_acccvt", self.insn32)
        self.assertNotIn('.mnemonic="bstart_acccvt"', self.meta)

    def test_reallocated_tlsu_words_decode_to_gm_atomic_forms(self) -> None:
        expected = {
            "bstart_mgather_exch": ("1001", "0x911181"),
            "bstart_mgather_max": ("1010", "0xa11181"),
            "bstart_mgather_min": ("1011", "0xb11181"),
            "bstart_mgather_add": ("1100", "0xc11181"),
        }
        for decode_name, (function_bits, match) in expected.items():
            self.assertRegex(
                self.insn32,
                rf"(?m)^\s*{decode_name}\s+\.\.\.\.\s+\.000\s+"
                rf"{function_bits}\s+0001\s+0001\s+0001\s+1000\s+0001\b",
            )
            self.assertIn(f'.mnemonic="{decode_name}"', self.meta)
            self.assertIn(f".match=UINT64_C({match})", self.meta)
        for retired in (
            "bstart_tmov_l2s_insert",
            "bstart_tmov_l2s_publish",
            "bstart_tmov_s2l_broadcast",
            "bstart_tmov_s2l_extract",
            "bstart_tstore_spart",
        ):
            self.assertNotIn(retired, self.insn32)
            self.assertNotIn(f'.mnemonic="{retired}"', self.meta)
            self.assertNotIn(f"trans_{retired}", self.translate)

    def test_b_iot_uses_all_exact_public_forms(self) -> None:
        patterns = (
            "0000 00.. .... .000 0101 ...0 0001 0011",
            "0000 00.. .... .... .101 .... .001 0011",
            ".... .... .... .000 0100 ...0 0001 0011",
            ".... .... .... .... .100 .... .001 0011",
            "0000 0000 0000 .... .110 .... .001 0011",
            ".... .... .... .000 0010 ...0 0001 0011",
            ".... .... .... .000 0011 ...0 0001 0011",
            ".... .... .... .000 0111 ...0 0001 0011",
            ".... .... .... .... .010 .... .001 0011",
            ".... .... .... .... .011 .... .001 0011",
            ".... .... .... .... .111 .... .001 0011",
            "0000 01.. .... .000 0101 ...0 0001 0011",
            "0000 01.. .... .... .101 .... .001 0011",
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
        self.assertIn(".mask=UINT64_C(0xfc0871ff)", self.meta)
        self.assertIn(".match=UINT64_C(0x1013)", self.meta)
        self.assertIn(".mask=UINT64_C(0xfc0ff1ff)", self.meta)
        self.assertIn(".match=UINT64_C(0x4001013)", self.meta)

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

    def test_pto_0586_retired_direct_tile_ops_are_absent(self) -> None:
        for retired in (
            "tconcat",
            "textract",
            "tinsert",
            "thistogram",
            "tquant",
            "tdequant",
            "tsort",
            "tmrgsort",
        ):
            decode_name = f"bstart_{retired}"
            self.assertNotIn(decode_name, self.insn32)
            self.assertNotIn(f'.mnemonic="{decode_name}"', self.meta)
            self.assertNotIn(f"trans_{decode_name}", self.translate)

    def test_internal_engine_names_do_not_restore_tma(self) -> None:
        self.assertIn("LINX_BLOCK_TLSU", self.helper)
        self.assertIn("LINX_BLOCK_OPERATION", self.helper)
        self.assertNotIn("LINX_BLOCK_TMA", self.helper)
        self.assertNotIn('return "tma"', self.helper)

    def test_v0581_new_exact_forms_are_decoded(self) -> None:
        expected = {
            "b_fpatr": ("0x7c7f", "0x2023"),
            "start_call_48": ("0xfffff83f0000007f", "0x501600000011"),
            "start_icall_32": ("0xf83fffff", "0x50166001"),
            "l_bstop": ("0xffffffffffffffff", "0x10000000f"),
        }
        for decode_name, (mask, match) in expected.items():
            with self.subTest(decode_name=decode_name):
                self.assertIn(f'.mnemonic="{decode_name}"', self.meta)
                self.assertIn(f".mask=UINT64_C({mask})", self.meta)
                self.assertIn(f".match=UINT64_C({match})", self.meta)

        self.assertRegex(self.insn32, r"(?m)^b_fpatr\s+")
        fpatr_meta = next(
            line for line in self.meta.splitlines()
            if '.mnemonic="b_fpatr"' in line
        )
        self.assertIn(".mask=UINT64_C(0x7c7f)", fpatr_meta)
        self.assertIn(".match=UINT64_C(0x2023)", fpatr_meta)
        self.assertRegex(self.insn48, r"(?m)^start_call_48\s+")
        self.assertRegex(self.insn32, r"(?m)^start_icall_32\s+")
        self.assertRegex(self.insn64, r"(?m)^l_bstop\s+")

    def test_retired_32bit_bare_call_forms_are_illegal(self) -> None:
        retired = (
            "start_call_32",
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
        self.assertNotIn("LINX_OP_START_CALL_32", self.ids)
        self.assertNotIn("c-bstart-call-setret", self.translate)
        self.assertNotIn("ret_base_offset", self.translate)
        self.assertNotIn("if ((hw & 0x000f) == 0x0002)", self.translate)
        self.assertIn("LINX_OP_BSTART_STD_CALL = 17", self.ids)
        self.assertIn('.mnemonic="hl_bstart_std_call"', self.meta)

    def test_sync_core_encoding_is_explicitly_unsupported(self) -> None:
        self.assertRegex(
            self.insn32,
            r"(?m)^sync_core\s+0000 0011 0011 0000 1010 0000 0010 1011$",
        )
        meta = next(
            line for line in self.meta.splitlines()
            if '.mnemonic="sync_core"' in line
        )
        self.assertIn(".mask=UINT64_C(0xffffffff)", meta)
        self.assertIn(".match=UINT64_C(0x330a02b)", meta)
        self.assertIn('flags="ENCODING_ONLY_UNSUPPORTED"', meta)
        body = self.translate.split(
            "static bool trans_sync_core", 1
        )[1].split("}", 1)[0]
        self.assertIn("return linx_illegal(ctx);", body)

    def test_scalar_amo_op5_masks_and_matches_are_exact(self) -> None:
        meta = {}
        for line in self.meta.splitlines():
            match = re.search(
                r'\.mask=UINT64_C\((0x[0-9a-f]+)\), '
                r'\.match=UINT64_C\((0x[0-9a-f]+)\), '
                r'\.mnemonic="([^"]+)"',
                line,
            )
            if match:
                meta[match.group(3)] = (
                    int(match.group(1), 16),
                    int(match.group(2), 16),
                )

        expected = {
            "casb": (0x0000707F, 0x0000001B),
            "cash": (0x0000707F, 0x0000101B),
            "casw": (0x0000707F, 0x0000201B),
            "casd": (0x0000707F, 0x0000301B),
            "dma": (0xFE007FFF, 0x0000700B),
        }
        sizes = {"b": 0, "h": 1, "w": 2, "d": 3}
        for suffix, op5 in sizes.items():
            expected[f"lr_{suffix}"] = (
                0x7800707F,
                (op5 << 27) | 0x0000000B,
            )
            expected[f"sc_{suffix}"] = (
                0x7800707F,
                (op5 << 27) | 0x0000100B,
            )
            expected[f"swap{suffix}"] = (
                0x7800707F,
                (op5 << 27) | 0x0000600B,
            )

        operations = {
            "add": 0,
            "and": 1,
            "or": 2,
            "xor": 3,
            "smax": 4,
            "smin": 5,
            "umax": 6,
            "umin": 7,
        }
        for operation, op5 in operations.items():
            expected[f"lw_{operation}"] = (
                0x7800707F,
                (op5 << 27) | 0x0000200B,
            )
            expected[f"ld_{operation}"] = (
                0x7800707F,
                (op5 << 27) | 0x0000400B,
            )
            expected[f"sw_{operation}"] = (
                0xFC007FFF,
                (op5 << 27) | 0x0000300B,
            )
            expected[f"sd_{operation}"] = (
                0xFC007FFF,
                (op5 << 27) | 0x0000500B,
            )

        expected.update(
            {
                "sw_sub": (0xFC007FFF, 0x4000300B),
                "sd_sub": (0xFC007FFF, 0x4000500B),
                "sb_add": (0xFC007FFF, 0x8000300B),
                "sb_sub": (0xFC007FFF, 0xC000300B),
                "sh_add": (0xFC007FFF, 0x8000500B),
                "sh_sub": (0xFC007FFF, 0xC000500B),
            }
        )
        for mnemonic, signature in expected.items():
            with self.subTest(mnemonic=mnemonic):
                self.assertEqual(meta[mnemonic], signature)

        store32 = self.translate.split(
            "#define LINX_DEFINE_STORE32", 1
        )[1].split("#define LINX_DEFINE_STORE64", 1)[0]
        store64 = self.translate.split(
            "#define LINX_DEFINE_STORE64", 1
        )[1].split("LINX_DEFINE_FETCH32", 1)[0]
        self.assertIn("linx_get_reg(a->SrcL), val32", store32)
        self.assertIn("linx_get_reg(a->SrcL), linx_get_reg(a->SrcR)", store64)


if __name__ == "__main__":
    unittest.main()
