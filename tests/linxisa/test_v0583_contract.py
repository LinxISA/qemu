#!/usr/bin/env python3
"""Source guards for the LinxISA/PTO ISA 0.58.6 QEMU contract."""

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
        cls.cpu_impl = (TARGET / "cpu.c").read_text(encoding="utf-8")
        cls.cube = (TARGET / "tile_cube_058.c").read_text(encoding="utf-8")
        cls.meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
        cls.ids = (TARGET / "linx_opcode_ids_gen.h").read_text(encoding="utf-8")
        cls.virt = (ROOT / "hw/linx/virt.c").read_text(encoding="utf-8")

    def test_exact_0586_elf_identity_is_fail_closed(self) -> None:
        authority = json.loads(
            (ROOT / "tests/linxisa/pto-isa-0586-authority.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            authority["pto_spec_commit"],
            "dea0b75e803cffa873982c90f9aa0cd17c6d243b",
        )
        self.assertIn('release\\\":\\\"0.58.6', self.virt)
        self.assertIn("pto-isa-0.58.6-mode-function-v1", self.virt)
        self.assertIn(
            "a757f2e50ec8050d2131b6b9ad38657511df80cf3f9424d5f009ea6e0cc35839",
            self.virt,
        )
        self.assertIn("Linx: error:", self.virt)
        self.assertIn("No guest instruction may execute", self.virt)
        self.assertIn("return false", self.virt)

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
        self.assertIn("has_size && size_code > 10u", self.translate)

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

    def test_shared_tload_accepts_optional_ior_stride(self) -> None:
        """TLOAD Shared uses B.IOR for the per-PE GM base/stride tuple."""
        binder = re.search(
            r"void HELPER\(linx_tile_append_shared_binder_v058\).*?\n\}",
            self.helper,
            re.S,
        )
        self.assertIsNotNone(binder)
        self.assertNotIn("env->tile_ior_count != 0u", binder.group(0))
        self.assertIn("peer->tile_ior_count == 0u", self.helper)

    def test_core4_allows_mttcg_for_guest_memory_barriers(self) -> None:
        self.assertIn(".mttcg_supported = true", self.cpu_impl)
        self.assertNotIn("does not support MTTCG", self.virt)

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
        self.assertNotIn("LINX_TLSU_TMOV_L2S_INSERT", self.helper)
        self.assertIn("LINX_TLSU_MGATHER_EXCH = 9", self.helper)
        self.assertIn("linx_tile_gm_atom_red", self.helper)
        self.assertIn("allowed |= LINX_DATR_PAD_OR_BYTE_ID", self.table)

    def test_v0586_gm_and_timg2col_preflight_before_effects(self) -> None:
        gm = self.helper.split("static bool linx_tile_gm_atom_red", 1)[1]
        gm = gm.split("typedef struct LinxTIMG2COLParams", 1)[0]
        self.assertIn("linx_tile_gm_index_dtype_supported", gm)
        self.assertIn("linx_tile_iommu_enabled(env)", gm)
        self.assertIn("atom && env->tile_attr_pad != 0u", gm)
        self.assertIn("address & (elem_bytes - 1u)", gm)
        self.assertLess(
            gm.index("Validate every source and address"),
            gm.index("linx_tile_gm_atomic_update"),
        )
        timg = self.helper.split("static bool linx_tile_timg2col_local", 1)[1]
        timg = timg.split("static bool linx_tile_cube_operand_legal", 1)[0]
        self.assertLess(
            timg.index("Probe the complete valid GM footprint"),
            timg.index("linx_tile_mem_read64"),
        )
        self.assertIn("*produced_out = pe_rows != 0u", timg)

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

    def test_hl_lui_is_reserved_for_upper_word_materialization(self) -> None:
        uses = []
        for source in (ROOT / "tests/linxisa").glob("*.s"):
            for line_number, line in enumerate(
                source.read_text(encoding="utf-8").splitlines(), start=1
            ):
                if "hl.lui" in line.lower():
                    uses.append((source.name, line_number, line.strip()))
        self.assertEqual(
            uses,
            [("hl_lui_value_contract.s", 5, "hl.lui 65536, ->a0")],
        )
        self.assertIn("linx_hl_lui_value(a->imm)", self.translate)
        self.assertIn("linx_hl_liu_value(a->uimm)", self.translate)
        self.assertIn(
            '{ "HL.LUI", "255991889818", "int", NULL }', self.helper
        )
        self.assertIn(
            '{ "HL.LIU", "9dd207ce3aea", "int", NULL }', self.helper
        )
        self.assertIn(
            '{ "HL.LIS", "908853d6ef87", "int", NULL }', self.helper
        )

    def test_csel_src_r_type_matches_authority(self) -> None:
        authority = json.loads(
            (ROOT / "tests/linxisa/linxisa-v0583-csel-authority.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(authority["default_src_r_type"], 3)
        self.assertEqual(authority["neg_src_r_type"], 2)
        self.assertEqual(
            authority["authority_commit"],
            "eddb0a2bd23399dd008381d21d89f20f742e7e53",
        )
        self.assertEqual(
            authority["decision_commit"],
            "d231a27e566f1a5bc12003caedcb6b73bcefe341",
        )
        catalog_blob = (
            json.dumps(
                authority["catalog_csel"],
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n"
        ).encode()
        self.assertEqual(
            hashlib.sha256(catalog_blob).hexdigest(),
            authority["catalog_csel_sha256"],
        )
        self.assertEqual(
            authority["catalog_sha256"],
            "e003d9a8e8e68de63afe0e8662e59658c173bb651e90f670c490aec2121ad1c8",
        )
        self.assertEqual(
            authority["sail_sha256"],
            "36b7b814ab567c69be42f1417efcc9aec3f5acad2151d8ebd9c946da1df842e5",
        )
        self.assertEqual(
            authority["sail_exec_csel_sha256"],
            "2b26f8624064d33f7f9cee4d0f39765498935903b8fe977abfdfce71fe101912",
        )
        numeric = (TARGET / "insn_numeric_058.h").read_text(encoding="utf-8")
        self.assertIn("(src_r_type & 3u) == 2u", numeric)
        self.assertIn("linx_csel_negates_src_r(srcRType)", self.translate)

    def test_tlsu_cpu_virtual_memory_route_is_guarded(self) -> None:
        self.assertIn("linx_tile_iommu_enabled", self.helper)
        self.assertIn("cpu_ldub_data(env, (abi_ptr)addr)", self.helper)
        self.assertIn("cpu_stb_data(env, (abi_ptr)addr", self.helper)
        mmu_guest = (ROOT / "tests/linxisa/mmu_ttbr_basic.s").read_text(
            encoding="utf-8"
        )
        self.assertIn("BSTART.TLOAD FP32", mmu_guest)
        self.assertIn("BSTART.TSTORE FP32", mmu_guest)
        self.assertIn("TTBR1-mapped", mmu_guest)
        self.assertIn("ssrset a1, 0x0f00", mmu_guest)
        self.assertIn("acre 0", mmu_guest)
        self.assertIn("ssrget 0x0020, ->a4", mmu_guest)
        self.assertIn("xori a4, 2, ->a4", mmu_guest)
        self.assertIn("hl.ssrget 0x1f03", mmu_guest)
        self.assertIn("hl.ssrget 0x1f45", mmu_guest)
        load_commit = self.helper.index("linx_tile_load(env, dst_tile")
        load_publish = self.helper.index(
            "linx_tile_complete_bound_output(", load_commit
        )
        store_commit = self.helper.index("linx_tile_store(env, src_tile")
        store_consume = self.helper.index(
            "linx_tile_consume_bound_sources(env, live, i", store_commit
        )
        self.assertLess(load_commit, load_publish)
        self.assertLess(store_commit, store_consume)

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
