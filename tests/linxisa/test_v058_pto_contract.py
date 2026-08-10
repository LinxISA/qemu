#!/usr/bin/env python3
"""Source-level guards for the final LinxISA/PTO 0.58 QEMU contract."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target/linx"


class PtoV058ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.decode16 = (TARGET / "insn16.decode").read_text(encoding="utf-8")
        cls.decode32 = (TARGET / "insn32.decode").read_text(encoding="utf-8")
        cls.cpu = (TARGET / "cpu.h").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        cls.tile_isa = (TARGET / "tile_isa_058.h").read_text(encoding="utf-8")
        cls.meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
        cls.ids = (TARGET / "linx_opcode_ids_gen.h").read_text(encoding="utf-8")
        cls.iommu_runner = (ROOT / "scripts/linxisa/run-iommu-tile-basic.sh").read_text(
            encoding="utf-8"
        )

    def test_retired_compressed_ios_is_absent(self) -> None:
        self.assertNotRegex(self.decode16, r"(?m)^c_b_ios\b")
        self.assertNotIn("trans_c_b_ios", self.translate)

    def test_b_ios_uses_final_32_bit_encoding(self) -> None:
        self.assertRegex(
            self.decode32,
            r"(?m)^b_ios\s+0000\s+\.\.\.\.\s+\.\.\.\.\s+0\.\.\.\s+\.001\s+\.\.\.0\s+0001\s+0011\b",
        )
        self.assertIn("%SharedTID", self.decode32)
        self.assertIn("%PE_MASK", self.decode32)
        self.assertIn("%TSize", self.decode32)
        self.assertIn("trans_b_ios", self.translate)
        self.assertIn('.mnemonic="b_ios"', self.meta)
        self.assertIn("LINX_OP_B_IOS = 638", self.ids)

    def test_shared_register_state_is_per_pe_and_core_private(self) -> None:
        self.assertIn("LinxSharedTileLane lane[LINX_CORE4_PE_COUNT]", self.cpu)
        self.assertIn("allocation_mask", self.cpu)
        self.assertIn("initialized_mask", self.cpu)
        self.assertIn("allocated_bytes", self.cpu)
        self.assertIn("LinxSharedTileVersion shared_tile[LINX_SHARED_TILE_COUNT]", self.cpu)

    def test_engine_names_are_final_058_names(self) -> None:
        self.assertNotIn("LINX_BLOCK_TMA", self.helper)
        self.assertNotRegex(self.helper, r"\bLINX_TMA_[A-Z0-9_]+\b")
        self.assertIn("LINX_BLOCK_TLSU", self.helper)
        self.assertIn("LINX_TLSU_TLOAD", self.helper)
        self.assertIn("linx_tile_operation_engine", self.tile_isa)
        self.assertIn("LINX_TILE_ENGINE_VEC", self.tile_isa)
        self.assertIn("LINX_TILE_ENGINE_SFU", self.tile_isa)
        self.assertNotIn('return "tma"', self.helper)
        self.assertNotIn("LINX_OP_BSTART_TMA", self.ids)
        self.assertIn("LINX_OP_BSTART_TLSU", self.ids)

    def test_tfma_is_fused_and_has_three_sources(self) -> None:
        self.assertIn("case 0x01cu: /* TFMA */ return 0x10cu;", self.helper)
        self.assertIn("case 0x10cu: /* TFMA */", self.helper)
        self.assertRegex(
            self.helper,
            r"case 0x10cu: /\* TFMA \*/\s*return 3;",
        )
        self.assertIn("fma(linx_tile_qword_as_f64(left)", self.helper)
        self.assertIn("const float fused = fmaf(", self.helper)
        self.assertIn('.mnemonic="bstart_tfma"', self.meta)
        self.assertIn(".match=UINT64_C(0x1c19181)", self.meta)

    def test_final_tlsu_cas_and_gmov_paths_are_executable(self) -> None:
        self.assertIn("trans_bstart_mgather_cas", self.translate)
        self.assertIn("trans_bstart_gmov", self.translate)
        self.assertIn("LINX_TLSU_MGATHER_CAS = 8", self.helper)
        self.assertIn("LINX_TLSU_GMOV = 13", self.helper)
        self.assertIn("linx_tile_group_gmov_profile", self.helper)
        self.assertIn("linx_tile_group_gmov_commit", self.helper)
        self.assertIn("collective_peer", self.cpu)
        self.assertIn("collective_pe_mask", self.cpu)
        self.assertIn("linx_tile_gmov_source_matches_destination", self.helper)

    def test_trace_classification_uses_the_four_engine_contract(self) -> None:
        self.assertIn("meta->op_id == LINX_OP_BSTART_TLSU", self.helper)
        self.assertIn("meta->op_id == LINX_OP_BSTART_CUBE", self.helper)
        self.assertIn("meta->op_id == LINX_OP_BSTART_TEPL", self.helper)
        self.assertRegex(self.helper, r'\?\s*"vec"\s*:\s*"sfu";')

    def test_b_ios_mask_zero_is_a_strict_noop(self) -> None:
        found = re.search(r"static bool trans_b_ios\([^)]*\)\s*\{", self.translate)
        self.assertIsNotNone(found)
        body = self.translate[found.end() : self.translate.find("\n}", found.end())]
        self.assertRegex(body, r"if\s*\(a->pe_mask\s*==\s*0u\)\s*\{\s*return true;")

    def test_iommu_runner_enables_the_finisher_and_has_a_timeout(self) -> None:
        self.assertIn("LINX_VIRT_TEST_FINISHER=1", self.iommu_runner)
        self.assertIn("subprocess.run", self.iommu_runner)
        self.assertIn("timeout=timeout", self.iommu_runner)


if __name__ == "__main__":
    unittest.main()
