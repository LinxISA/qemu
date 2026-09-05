#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression guards for the architectural issues raised in QEMU PR 50."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target/linx"


class V058ReviewContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpu = (TARGET / "cpu.c").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")
        cls.helper_h = (TARGET / "helper.h").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        cls.virt = (ROOT / "hw/linx/virt.c").read_text(encoding="utf-8")
        cls.tile_state_dump = (
            ROOT / "hw/linx/tile-state-dump.c"
        ).read_text(encoding="utf-8")

    def test_core4_collectives_support_mttcg_guest_barriers(self) -> None:
        self.assertIn(".mttcg_supported = true", self.cpu)

    def test_vmstate_v19_does_not_claim_backward_compatibility(self) -> None:
        self.assertIn(".version_id = 23", self.cpu)
        self.assertIn(".minimum_version_id = 19", self.cpu)
        vmstate = self.cpu[
            self.cpu.index("static const VMStateDescription vmstate_linx_cpu") :
        ]
        for unversioned_field in (
            "env.tile_data_attr_valid",
            "env.tile_fixp_attr",
            "env.tile_fixp_attr_valid",
        ):
            self.assertNotIn(unversioned_field, vmstate)
        layout = "VMSTATE_UINT8_ARRAY_V(env.tile_reg_layout, LinxCPU,"
        self.assertIn(layout, vmstate)
        self.assertRegex(
            vmstate,
            r"VMSTATE_UINT8_ARRAY_V\(env\.tile_reg_layout, LinxCPU,\s*"
            r"LINX_TILE_SLOT_COUNT, 19\),",
        )
        self.assertIn("env.tile_reg_cube_storage_bytes", vmstate)
        self.assertNotIn("if (version_id < 19)", self.cpu)

    def test_fpatr_is_consumed_and_inactive_acr_migration_is_rejected(self) -> None:
        cube = (TARGET / "tile_cube_058.c").read_text(encoding="utf-8")
        convert = cube.split("bool linx_tile_accumulator_convert", 1)[1]
        self.assertIn("linx_tile_fpatr_postprocess", convert)
        self.assertIn("tile_fpatr_raw", cube)
        self.assertIn("tile_fpatr_valid", cube)
        pre_save = self.cpu.split("static int linx_cpu_pre_save", 1)[1]
        pre_save = pre_save.split("static bool linx_cpu_post_load", 1)[0]
        self.assertIn("acr_block_state[acr].tile_fpatr_valid", pre_save)
        vmstate = self.cpu.split("static const VMStateDescription vmstate_linx_cpu", 1)[1]
        self.assertIn("VMSTATE_UINT32_V(env.tile_fpatr_raw, LinxCPU, 20)", vmstate)

    def test_fpatr_position_is_fail_closed_and_executable(self) -> None:
        helper = self.helper[
            self.helper.index("HELPER(linx_validate_fpatr_position)") :
            self.helper.index("HELPER(linx_tile_reset_block)")
        ]
        self.assertIn("env->blocktype == LINX_BLOCK_CUBE", helper)
        self.assertIn("env->tile_fpatr_valid", helper)
        self.assertIn("env->tile_ior_count", helper)
        self.assertIn("env->tile_iot_count", helper)
        self.assertIn("env->tile_iot_valid", helper)
        fpatr = self.translate.split("static bool trans_b_fpatr", 1)[1]
        fpatr = fpatr.split("static bool trans_b_hint", 1)[0]
        self.assertLess(
            fpatr.index("gen_helper_linx_validate_fpatr_position"),
            fpatr.index("tile_fpatr_raw"),
        )
        pre_save = self.cpu.split("static int linx_cpu_pre_save", 1)[1]
        pre_save = pre_save.split("static bool linx_cpu_post_load", 1)[0]
        self.assertIn("acr_block_state[acr].tile_ior_count", pre_save)
        fixture = (ROOT / "tests/linxisa/fpatr_position_contract.s").read_text()
        runner = (ROOT / "scripts/linxisa/run-fpatr-position-contract.sh").read_text()
        for name in ("non_cube", "duplicate", "post_ior", "post_iot", "post_ios"):
            self.assertIn(name, fixture)
        self.assertIn("fpatr_position_cross_tb", fixture)
        self.assertIn("illegal B.FPATR position", runner)

    def test_signed_divrem_overflow_remainder_is_zero(self) -> None:
        scalar = self.translate[
            self.translate.index("static void linx_emit_divrem") :
            self.translate.index("static bool trans_div_like")
        ]
        pair = self.translate[
            self.translate.index("static bool linx_hl_divrem_pair_common") :
            self.translate.index("static bool trans_hl_mul")
        ]
        self.assertRegex(
            scalar,
            r"gen_set_label\(overflow\);[\s\S]*?tcg_gen_movi_i64\(out, 0\);",
        )
        self.assertRegex(
            pair,
            r"gen_set_label\(overflow\);[\s\S]*?tcg_gen_movi_i64\(rem, 0\);",
        )
        self.assertIn("tcg_gen_brcond_i64(TCG_COND_NE, lhs, minval", pair)

    def test_b_ior_tlsu_binding_and_authored_order(self) -> None:
        self.assertIn(
            "static const unsigned shifts[] = { 5, 10, 15, 0 };",
            self.helper,
        )
        base = self.helper[
            self.helper.index("static bool linx_tile_get_base_reg") :
            self.helper.index("static uint64_t linx_tile_get_stride_bytes")
        ]
        stride = self.helper[
            self.helper.index("static uint64_t linx_tile_get_stride_bytes") :
            self.helper.index("static bool linx_tile_get_shared_tload_size")
        ]
        self.assertIn("*addr_reg_out = 0u", base)
        self.assertIn("(desc >> 5)", base)
        self.assertNotIn("desc >> 10", base)
        self.assertIn("return 0", stride)
        self.assertIn("(desc >> 10)", stride)
        shared_size = self.helper[
            self.helper.index("static bool linx_tile_get_shared_tload_size") :
            self.helper.index("static inline unsigned linx_tile_shared_id")
        ]
        self.assertIn("env->tile_ior_count > 1u", shared_size)

    def test_scalar_store_invalidates_raw_tile_transport_after_success(self) -> None:
        store = self.translate[
            self.translate.index("static bool linx_store_from_reg") :
            self.translate.index("static bool trans_sbi")
        ]
        self.assertIn(
            "DEF_HELPER_3(linx_invalidate_raw_tile_transport, void, env, i64, i64)",
            self.helper_h,
        )
        self.assertLess(
            store.index("tcg_gen_qemu_st_i64"),
            store.index("gen_helper_linx_invalidate_raw_tile_transport"),
        )
        invalidation = self.helper[
            self.helper.index("HELPER(linx_invalidate_raw_tile_transport)") :
            self.helper.index("static void linx_tile_record_raw_transport")
        ]
        self.assertIn("linx_tile_invalidate_raw_transports", invalidation)

    def test_regular_transfer_uses_valid_rectangle_and_byte_stride(
        self,
    ) -> None:
        load = self.helper[
            self.helper.index("static void linx_tile_load") :
            self.helper.index("static void linx_tile_store")
        ]
        store = self.helper[
            self.helper.index("static void linx_tile_store") :
            self.helper.index("static bool linx_tile_sparse_shape")
        ]
        self.assertIn("to < gm_outer", load)
        self.assertIn("ti < gm_inner", load)
        self.assertNotIn("linx_tile_pad_value64", load)
        self.assertRegex(load, r"effective_stride_bytes")
        self.assertRegex(store, r"effective_stride_bytes")
        self.assertIn("(uint64_t)row * stride + (uint64_t)col * elem_bytes", self.helper)
        self.assertIn("linx_tile_state_encode(out", self.virt)
        self.assertIn("row < record->valid_rows", self.tile_state_dump)
        self.assertIn("col < record->valid_cols", self.tile_state_dump)


if __name__ == "__main__":
    unittest.main()
