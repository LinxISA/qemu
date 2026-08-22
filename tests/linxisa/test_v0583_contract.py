#!/usr/bin/env python3
"""Source guards for the LinxISA/PTO ISA 0.58.3 QEMU contract."""

from __future__ import annotations

import hashlib
import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target/linx"


class PtoV0583ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.decode = (TARGET / "insn32.decode").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")
        cls.table = (TARGET / "tile_isa_058.h").read_text(encoding="utf-8")
        cls.cpu = (TARGET / "cpu.h").read_text(encoding="utf-8")
        cls.cube = (TARGET / "tile_cube_058.c").read_text(encoding="utf-8")
        cls.meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
        cls.ids = (TARGET / "linx_opcode_ids_gen.h").read_text(encoding="utf-8")
        cls.virt = (ROOT / "hw/linx/virt.c").read_text(encoding="utf-8")

    def test_exact_0583_elf_identity_is_fail_closed(self) -> None:
        authority = json.loads(
            (ROOT / "tests/linxisa/pto-isa-0583-authority.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(authority["linxisa_commit"], "dd52a2e5")
        self.assertEqual(
            authority["pto_spec_commit"],
            "e599a3d36ebfad43362ff591ea5e128816c684c7",
        )
        self.assertIn('release\\\":\\\"0.58.3', self.virt)
        self.assertIn("pto-isa-0.58.3-mode-function-v1", self.virt)
        self.assertIn(
            "8a48b80e04484c70870f155bf9efc79d2a805cf99e809f4e4e8a7e6a7eb34172",
            self.virt,
        )
        self.assertNotIn("accepting legacy ELF", self.virt)

    def test_iot_ios_use_size_code_and_fixed_pe_mode(self) -> None:
        for field in (
            "%IOTV5SizeCode 15:4",
            "%IOTV5PEMode 9:3",
            "%SizeCode 15:4",
            "%PEMode 9:3",
        ):
            self.assertIn(field, self.decode)
        self.assertIn("linx_pemode_to_mask", self.translate)
        self.assertIn("0x0u, 0x1u, 0x2u, 0x4u, 0x8u, 0x3u, 0x7u, 0xfu", self.translate)
        self.assertIn("a->size_code > 12u", self.translate)
        self.assertIn("size_code > 10u", self.translate)

    def test_fpatr_carries_shared_transpose_bits(self) -> None:
        self.assertIn("%FP_TransA 7:1", self.decode)
        self.assertIn("%FP_TransB 8:1", self.decode)
        fpatr = re.search(r"(?m)^b_fpatr\s+.*$", self.decode)
        self.assertIsNotNone(fpatr)
        self.assertIn("transa=%FP_TransA", fpatr.group(0))
        self.assertIn("transb=%FP_TransB", fpatr.group(0))
        self.assertIn("(a->transb << 8)", self.translate)
        self.assertIn("(a->transa << 7)", self.translate)

    def test_tlsu_row_stride_is_bytes(self) -> None:
        self.assertIn("linx_tile_get_stride_bytes", self.helper)
        self.assertIn(
            "(uint64_t)row * stride + (uint64_t)col * elem_bytes",
            self.helper,
        )

    def test_cube_cell_layout_and_accumulator_are_distinct(self) -> None:
        self.assertIn("#define LINX_TILE_CELL_BYTES 128u", self.cpu)
        self.assertIn("linx_tile_cube_output_descriptor_058", self.cube)
        self.assertIn("required > sizeof(env->tile_acc)", self.cube)
        self.assertIn("shared_a_cols", self.cube)
        self.assertIn("shared_b_cols", self.cube)
        self.assertIn("linx_tile_cube_payload_index_058", self.cube)
        self.assertIn("tile_reg_cube_cell_count", self.cpu)
        self.assertIn("linx_tile_numeric_acc_dtype(env->tile_dtype)", self.helper)

    def test_binding_stream_is_closed_before_mutation(self) -> None:
        self.assertIn("#define LINX_TILE_MAX_IOT 4u", self.cpu)
        self.assertIn("#define LINX_TILE_MAX_SHARED_BINDERS 4u", self.cpu)
        self.assertIn("split_size_completion", self.helper)
        self.assertIn("output_count == expected_outputs", self.helper)
        self.assertIn("env->tile_fpatr_valid != 1u", self.helper)
        self.assertNotIn("legacy_whole", self.helper)
        self.assertIn("allowed |= LINX_DATR_PAD_OR_BYTE_ID", self.table)

    def test_cube_guest_vectors_are_wired(self) -> None:
        runner = (ROOT / "scripts/linxisa/run-cube-0583-contract.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("cube_cell_guest_contract.s", runner)
        self.assertIn("cube_shared_transpose_guest_contract.s", runner)
        self.assertIn("cube_acc_packed_guest_contract.s", runner)
        self.assertIn("cube_fpatr_aux_guest_contract.s", runner)
        self.assertIn("cube_mx_type_matrix_guest_contract.s", runner)
        shared = (ROOT / "tests/linxisa/cube_shared_transpose_guest_contract.s").read_text(
            encoding="utf-8"
        )
        self.assertIn(".4byte 0x000021a3", shared)
        self.assertIn("B.IOS S1, mask=1111", shared)
        self.assertIn("B.IOS S2, mask=1111", shared)

    def test_complete_fpatr_modes_and_auxiliary_sources_are_executable(self) -> None:
        for symbol in (
            "linx_tile_fpatr_mode_uses_vector_parameter_058",
            "linx_tile_fpatr_mode_uses_scalar_parameter_058",
            "linx_tile_fpatr_quant_parameter_legal_058",
            "linx_tile_accumulator_convert_with_aux_058",
            "linx_tile_cube_reduction_outputs_with_input_058",
        ):
            self.assertIn(symbol, self.cube + self.helper)
        self.assertNotIn("parameter-free postprocess subset", self.cube)

    def test_cube_type_conformance_is_authority_generated(self) -> None:
        generator = (ROOT / "scripts/linxisa/generate-cube-0583-conformance.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("pto-v0583-matrix-type-authority.json", generator)
        self.assertIn("ordinary_pairs = sum", generator)
        self.assertIn("len(mx_types) ** 2", generator)

    def test_retired_b_branches_are_not_decoded_or_advertised(self) -> None:
        for name in ("b_eq", "b_ne", "b_lt", "b_ltu", "b_ge", "b_geu", "b_z", "b_nz"):
            self.assertNotRegex(self.decode, rf"(?m)^{name}\s+")
            self.assertNotIn(f'.mnemonic="{name}"', self.meta)
            self.assertNotIn(f"trans_{name}(", self.translate)
            self.assertNotIn(f"LINX_OP_{name.upper()}", self.ids)

    def test_official_0583_numeric_vectors_are_vendored_exactly(self) -> None:
        vectors = ROOT / "tests/linxisa/pto-isa-0583-hardware-numeric-vectors.json"
        self.assertTrue(vectors.is_file())
        self.assertEqual(
            hashlib.sha256(vectors.read_bytes()).hexdigest(),
            "09d863e39e5fcd932353f4dc3bc5a7d2eed91ec9818c83886747aa69d28c3890",
        )
        runner = (ROOT / "tests/linxisa/test_v058_hardware_numeric_vectors.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("pto-isa-0583-hardware-numeric-vectors.json", runner)
        self.assertIn("matrix_postprocess", runner)
        self.assertIn("tcvt_e8m0", runner)


if __name__ == "__main__":
    unittest.main()
