#!/usr/bin/env python3
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
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        cls.virt = (ROOT / "hw/linx/virt.c").read_text(encoding="utf-8")
        cls.tile_state_dump = (
            ROOT / "hw/linx/tile-state-dump.c"
        ).read_text(encoding="utf-8")

    def test_core4_collectives_keep_mttcg_disabled(self) -> None:
        self.assertIn(".mttcg_supported = false", self.cpu)

    def test_vmstate_v19_does_not_claim_backward_compatibility(self) -> None:
        self.assertIn(".version_id = 19", self.cpu)
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
            r"LINX_TILE_SLOT_COUNT, 19\),\s*VMSTATE_END_OF_LIST",
        )
        self.assertNotIn("if (version_id < 19)", self.cpu)

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
            self.helper.index("static uint64_t linx_tile_get_stride_elements")
        ]
        stride = self.helper[
            self.helper.index("static uint64_t linx_tile_get_stride_elements") :
            self.helper.index("static bool linx_tile_get_shared_tload_size")
        ]
        self.assertIn("*addr_reg_out = 0u", base)
        self.assertIn("(desc >> 5)", base)
        self.assertNotIn("desc >> 10", base)
        self.assertIn("return env->lb[2]", stride)
        self.assertIn("(desc >> 10)", stride)
        shared_size = self.helper[
            self.helper.index("static bool linx_tile_get_shared_tload_size") :
            self.helper.index("static inline unsigned linx_tile_shared_id")
        ]
        self.assertIn("env->tile_ior_count > 1u", shared_size)

    def test_regular_transfer_uses_valid_rectangle_and_element_stride(self) -> None:
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
        self.assertRegex(load, r"stride_elements \+ ti\) \* elem_bytes")
        self.assertRegex(store, r"stride_elements \+ gi\) \* elem_bytes")
        self.assertIn("(uint64_t)row * stride_elements + col", self.helper)
        self.assertIn("linx_tile_state_encode(out", self.virt)
        self.assertIn("row < record->valid_rows", self.tile_state_dump)
        self.assertIn("col < record->valid_cols", self.tile_state_dump)


if __name__ == "__main__":
    unittest.main()
