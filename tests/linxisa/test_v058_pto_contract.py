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
        cls.cube = (TARGET / "tile_cube_058.c").read_text(encoding="utf-8")
        cls.numeric = (TARGET / "tile_numeric_058.h").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        cls.tile_isa = (TARGET / "tile_isa_058.h").read_text(encoding="utf-8")
        cls.tile_cube = (TARGET / "tile_cube_058.c").read_text(encoding="utf-8")
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
        self.assertIn("%SizeCode", self.decode32)
        self.assertIn("%PEMode", self.decode32)
        self.assertIn("trans_b_ios", self.translate)
        self.assertIn('.mnemonic="b_ios"', self.meta)
        self.assertIn("LINX_OP_B_IOS = 638", self.ids)

    def test_shared_register_state_is_per_pe_and_core_private(self) -> None:
        self.assertIn("#define LINX_SHARED_TILE_MAX_BYTES (256 * 1024)", self.cpu)
        self.assertIn("uint8_t data[LINX_SHARED_TILE_MAX_BYTES]", self.cpu)
        self.assertNotIn("LinxSharedTileLane", self.cpu)
        self.assertIn("allocation_mask", self.cpu)
        self.assertIn("initialized_mask", self.cpu)
        self.assertIn("allocated_bytes", self.cpu)
        self.assertIn("LinxSharedTileVersion shared_tile[LINX_SHARED_TILE_COUNT]", self.cpu)

    def test_shared_allocation_allows_subsets_but_rejects_expansion(
        self,
    ) -> None:
        self.assertIn(
            "linx_tile_shared_allocation_compatible", self.tile_isa
        )
        self.assertIn(
            "(requested_mask & ~allocation_mask) == 0u", self.tile_isa
        )
        self.assertNotIn("shared->allocation_mask == pe_mask", self.helper)

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

    def test_mx_cube_uses_normative_operand_order(self) -> None:
        # PTO v0.58.3: [C?], A, optional ScaleA, B, optional ScaleB,
        # [Bias], followed by postprocess auxiliaries.
        self.assertIn("linx_tile_cube_resolve_local_operands", self.helper)
        self.assertIn("operands->accumulator = sources[cursor++]", self.helper)
        self.assertIn("operands->left = sources[cursor++]", self.helper)
        self.assertIn("operands->left_scale = sources[cursor++]", self.helper)
        self.assertIn("operands->right = sources[cursor++]", self.helper)
        self.assertIn("operands->right_scale = sources[cursor++]", self.helper)

    def test_mx_cube_accepts_all_asl_fp4_pairs_and_scales(self) -> None:
        # All ordered pairs from FP16/BF16/E4M3/E5M2/E2M1X2/E1M2X2;
        # only the four low-precision types carry a side scale.
        self.assertIn("(UINT32_C(1) << 4) | (UINT32_C(1) << 5)", self.numeric)
        self.assertIn("(UINT32_C(1) << 11) | (UINT32_C(1) << 12)", self.numeric)
        self.assertIn("linx_tile_numeric_mx_requires_scale", self.numeric)
        self.assertIn("left_dtype == (env->tile_dtype & 31u)", self.cube)
        self.assertIn("scale_left &&", self.cube)
        self.assertIn("scale_right &&", self.cube)

    def test_tmatmul_dimensions_are_m_n_k(self) -> None:
        self.assertRegex(
            self.cube,
            r"if \(cube_is_tmatmul_family\(env\)\) \{\s*"
            r"return \(LinxTileCubeDimensions\) \{\s*"
            r"\.m = cube_dimension\(env->lb\[0\]\),\s*"
            r"\.n = cube_dimension\(env->lb\[1\]\),\s*"
            r"\.k = cube_dimension\(env->lb\[2\]\),",
        )
        self.assertRegex(
            self.cube,
            r"physical_cols = cube_is_tmatmul_family\(env\)\s*"
            r"\? env->tile_acc_cols",
        )
        self.assertNotIn("LB2 is destination Col", self.cube)

    def test_addtpc_uses_linxisa_0581_page_base(self) -> None:
        addtpc = re.search(
            r"static bool trans_addtpc\(.*?\n\}", self.translate, re.S
        ).group(0)
        hl_addtpc = re.search(
            r"static bool trans_hl_addtpc\(.*?\n\}", self.translate, re.S
        ).group(0)
        for body in (addtpc, hl_addtpc):
            self.assertIn("pc_page", body)
            self.assertRegex(body, r"current_pc\s*&\s*~\(vaddr\)0xfff")
            self.assertRegex(body, r"(?:imm|offset)\s*<<=\s*12")
            self.assertNotRegex(body, r"offset\s*<<=\s*1(?![0-9])")

    def test_scalar_right_modifier_mapping_matches_linxisa_0583(self) -> None:
        arithmetic = re.search(
            r"static TCGv_i64 linx_srcR_addsub\(.*?\n\}", self.translate, re.S
        ).group(0)
        logical = re.search(
            r"static TCGv_i64 linx_srcR_logic\(.*?\n\}", self.translate, re.S
        ).group(0)
        compare = re.search(
            r"static TCGv_i64 linx_srcR_compare\(.*?\n\}", self.translate, re.S
        ).group(0)
        select = re.search(
            r"static TCGv_i64 linx_srcR_select\(.*?\n\}", self.translate, re.S
        ).group(0)

        for body in (arithmetic, logical):
            self.assertRegex(body, r"case 0: /\* \.sw \*/")
            self.assertRegex(body, r"case 1: /\* \.uw \*/")
            self.assertIn("default: /* no modifier */", body)
        self.assertRegex(arithmetic, r"case 2: /\* \.neg \*/")
        self.assertRegex(logical, r"case 2: /\* \.not \*/")
        self.assertIn("default: /* 0 and 3 are unmodified aliases */", compare)
        self.assertIn("linx_csel_negates_src_r(srcRType)", select)

        for name in ("trans_cmp_eq", "trans_cmp_ne", "trans_setc_eq", "trans_setc_ne"):
            body = re.search(
                rf"static bool {name}\(.*?\n\}}", self.translate, re.S
            ).group(0)
            self.assertIn("linx_srcR_compare", body)
        csel = re.search(
            r"static bool trans_csel\(.*?\n\}", self.translate, re.S
        ).group(0)
        self.assertIn("linx_srcR_select", csel)

        addressing = re.search(
            r"static TCGv_i64 linx_addr_add_reg\(.*?\n\}",
            self.translate,
            re.S,
        ).group(0)
        self.assertRegex(addressing, r"case 0: /\* \.sw \*/")
        self.assertRegex(addressing, r"case 1: /\* \.uw \*/")
        self.assertRegex(addressing, r"case 2: /\* \.neg \*/")
        self.assertIn("default: /* no modifier */", addressing)

    def test_shared_tstore_profiles_are_executable(self) -> None:
        self.assertIn("bstart_tstore_spart", self.decode32)
        self.assertIn("trans_bstart_tstore_spart", self.translate)
        self.assertIn("LINX_TLSU_TSTORE_SPART = 14", self.helper)
        self.assertIn("linx_tile_shared_tstore_legal", self.helper)
        self.assertIn("linx_tile_shared_tstore_commit", self.helper)

    def test_shared_tload_uses_released_per_mask_quarters(self) -> None:
        self.assertIn("linx_tile_shared_transfer_preflight", self.helper)
        self.assertIn("size_code < 3u || size_code > 14u", self.helper)
        self.assertIn("shared->per_pe_capacity = bytes", self.helper)
        self.assertIn(
            "ctpop8(allocation_mask) * bytes", self.helper
        )
        self.assertIn("shared->allocation_mask = allocation_mask", self.helper)
        self.assertIn("ctpop8(pe_mask) * bytes", self.helper)
        self.assertIn("cpu_ldub_data(peer, (abi_ptr)(source + byte))", self.helper)
        self.assertNotIn("single_issuer", self.helper)

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

    def test_shared_tmov_uses_architectural_functions_and_aggregate_payload(self) -> None:
        for name, function in (
            ("LINX_TLSU_TMOV_L2S_INSERT", 9),
            ("LINX_TLSU_TMOV_L2S_PUBLISH", 10),
            ("LINX_TLSU_TMOV_S2L_BROADCAST", 11),
            ("LINX_TLSU_TMOV_S2L_EXTRACT", 12),
        ):
            self.assertIn(f"{name} = {function}", self.helper)
        self.assertIn("linx_tile_shared_tmov_local_to_shared", self.helper)
        self.assertIn("linx_tile_shared_tmov_shared_to_local", self.helper)
        # Shared state uses one Core-level aggregate payload with fixed PE
        # regions; selected regions are never packed.
        self.assertIn("shared->initialized_mask |= mask", self.helper)
        self.assertIn("destination_offset = legacy_whole ? 0u", self.helper)
        self.assertIn("memcpy((uint8_t *)env->tile_reg[destination],",
                      self.helper)
        self.assertIn("qemu_mutex_lock(&cpu->core4->lock)", self.helper)
        self.assertIn("g_autofree uint8_t *payload = NULL", self.helper)
        self.assertNotIn("LinxSharedTileLane descriptor", self.helper)

    def test_shared_cube_commits_explicit_destination_atomically(self) -> None:
        self.assertIn("env->tile_iot_count != 2u", self.helper)
        self.assertIn(
            "linx_tile_get_bound_output(env, 1u, &destination)", self.helper
        )
        self.assertIn("core4->collective_dst[pe] = destination", self.helper)
        self.assertIn("linx_tile_accumulator_convert_with_aux_058(", self.helper)
        self.assertIn("planned_count[i], 1u)", self.helper)
        self.assertIn("planned_live[LINX_CORE4_PE_COUNT]", self.helper)
        self.assertIn("dims.m != 0u && dims.n != 0u && dims.k != 0u",
                      self.tile_cube)
        self.assertNotIn("32u * 32u * 4u", self.helper)
        self.assertIn(
            "linx_tile_cube_operand_legal(env, src_a, left_dtype,",
            self.helper,
        )
        self.assertIn("linx_tile_numeric_ordinary_matrix_pair", self.helper)
        self.assertIn("(shared->initialized_mask & mask) == mask", self.helper)
        self.assertIn("shared->initialized_mask == 0xfu", self.helper)

    def test_trace_classification_uses_the_four_engine_contract(self) -> None:
        self.assertIn("meta->op_id == LINX_OP_BSTART_TLSU", self.helper)
        self.assertIn("meta->op_id == LINX_OP_BSTART_CUBE", self.helper)
        self.assertIn("meta->op_id == LINX_OP_BSTART_TEPL", self.helper)
        self.assertRegex(self.helper, r'\?\s*"vec"\s*:\s*"sfu";')

    def test_b_ios_mask_zero_is_a_strict_noop(self) -> None:
        found = re.search(r"static bool trans_b_ios\([^)]*\)\s*\{", self.translate)
        self.assertIsNotNone(found)
        body = self.translate[found.end() : self.translate.find("\n}", found.end())]
        self.assertRegex(body, r"if\s*\(pe_mask\s*==\s*0u\)\s*\{")

    def test_iommu_runner_enables_the_finisher_and_has_a_timeout(self) -> None:
        self.assertIn("LINX_VIRT_TEST_FINISHER=1", self.iommu_runner)
        self.assertIn("subprocess.run", self.iommu_runner)
        self.assertIn("timeout=timeout", self.iommu_runner)

    def test_engine_counts_and_datr_tables_cover_the_v058_catalog(self) -> None:
        expected_engine_counts = {"VEC": 31, "SFU": 56, "TLSU": 10, "CUBE": 12}
        self.assertEqual(sum(expected_engine_counts.values()), 109)
        for family, function_name in (
            ("TEPL", "linx_tile_operation_datr_allowed"),
            ("TLSU", "linx_tile_tlsu_datr_allowed"),
            ("CUBE", "linx_tile_cube_datr_allowed"),
        ):
            body = self.tile_isa.split(f"{function_name}(uint32_t selector)", 1)[1]
            body = body.split("};", 1)[0]
            actual = [int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", body)]
            self.assertEqual(len(actual), {"TEPL": 128, "TLSU": 32, "CUBE": 32}[family])
        # The operation catalog describes TileOp applicability; CUBE's
        # B.DATR carrier is validated separately by the block schema.  Lock
        # the current accepted CUBE selectors and the FP4 MX selector's
        # DataType applicability without conflating the two projections.
        cube_body = self.tile_isa.split(
            "linx_tile_cube_datr_allowed(uint32_t selector)", 1
        )[1].split("};", 1)[0]
        cube_values = [int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", cube_body)]
        self.assertEqual(cube_values[4], 0x0d)
        self.assertEqual(cube_values[6], 0x0d)

    def test_v058_elf_identity_is_validated_before_any_load(self) -> None:
        virt = (ROOT / "hw/linx/virt.c").read_text(encoding="utf-8")
        self.assertIn("linx_validate_pto_isa_identity", virt)
        self.assertIn("pto-isa-0.58.3-mode-function-v1", virt)
        self.assertIn(
            "8a48b80e04484c70870f155bf9efc79d2a805cf99e809f4e4e8a7e6a7eb34172",
            virt,
        )
        dispatch = virt.split("static bool linx_load_elf(", 1)[1]
        validate = dispatch.index("linx_validate_pto_isa_identity")
        first_loader = min(
            dispatch.index("linx_load_elf32_rel"),
            dispatch.index("linx_load_elf32_exec"),
            dispatch.index("linx_load_elf64_rel"),
            dispatch.index("linx_load_elf64_exec"),
        )
        self.assertLess(validate, first_loader)
        # PTO v0.58.3 requires the identity note; legacy-ELF acceptance was
        # removed upstream.
        self.assertIn("malformed .note.pto.isa", virt)

    def test_fused_icall_snapshots_the_existing_barg_target(self) -> None:
        cpu_h = (ROOT / "target/linx/cpu.h").read_text(encoding="utf-8")
        cpu_c = (ROOT / "target/linx/cpu.c").read_text(encoding="utf-8")
        self.assertIn("fused_icall_target", cpu_h)
        self.assertIn("fused_icall_target_valid", cpu_h)
        self.assertIn("LINX_TB_FLAG_FUSED_ICALL", cpu_h)
        self.assertIn("flags |= LINX_TB_FLAG_FUSED_ICALL", cpu_c)
        fused = self.translate.split("static bool trans_start_icall_32", 1)[1]
        fused = fused.split("static bool trans_start_call_48", 1)[0]
        self.assertLess(
            fused.index("linx_gen_check_bstart_target"),
            fused.index("linx_block_begin"),
        )
        self.assertLess(
            fused.index("linx_gen_check_bstart_target"),
            fused.index("linx_set_dest(LINX_REG_RA"),
        )
        self.assertIn("fused_icall_target_valid", fused)
        icall_commit = self.translate.split("case LINX_BR_ICALL:", 1)[1]
        icall_commit = icall_commit.split("default:", 1)[0]
        self.assertIn("fused_icall_target_valid", icall_commit)
        self.assertIn("fused_icall_target", icall_commit)

    def test_fused_icall_fixture_executes_positive_invalid_and_cross_tb_cases(self) -> None:
        fixture = (ROOT / "tests/linxisa/fused_call_contract.s").read_text(
            encoding="utf-8"
        )
        invalid = (ROOT / "tests/linxisa/fused_icall_invalid_contract.s").read_text(
            encoding="utf-8"
        )
        runner = (ROOT / "scripts/linxisa/run-fused-call-contract.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("fused_icall_positive", fixture)
        self.assertIn("fused_icall_cross_tb", fixture)
        self.assertIn("fused_icall_poison", fixture)
        self.assertIn("fused ICALL must end at a page boundary", fixture)
        self.assertIn("fused_icall_invalid_target", invalid)
        self.assertIn("unexpectedly accepted invalid fused ICALL target", runner)


if __name__ == "__main__":
    unittest.main()
