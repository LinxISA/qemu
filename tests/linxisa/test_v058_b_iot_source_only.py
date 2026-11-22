#!/usr/bin/env python3
"""Regression checks for canonical PTO ISA 0.58 B.IOT forms."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
TRANSLATE = ROOT / "target/linx/translate.c"
HELPER = ROOT / "target/linx/helper.c"


class V058BIOTSourceOnlyTest(unittest.TestCase):
    def test_v058_forms_preserve_pe_mask_and_source_arity(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn("func == 5u ? LINX_IOT_S1V", text)
        self.assertIn("LINX_IOT_S0V | LINX_IOT_S1V", text)
        self.assertIn("LINX_IOT_S0_REUSE", text)
        self.assertIn("LINX_IOT_S1_REUSE", text)
        self.assertNotIn("dst == 7u", text)
        self.assertIn(
            "linx_emit_tile_iot_desc(ctx, flags, dst, last, src0, src1, pe_mask,",
            text,
        )
        self.assertNotIn("IOTV4", text)

    def test_v058_shape_constraints_are_canonical(self) -> None:
        text = TRANSLATE.read_text(encoding="utf-8")
        self.assertIn("if (pe_mask == 0u)", text)
        self.assertIn("func < 2u || func > 7u", text)
        self.assertIn("has_size && size_code > 10u", text)
        self.assertIn(
            "const uint32_t local_size_code = size_code == 0u ? 0u : size_code + 2u;",
            text,
        )
        body = text.split("static bool linx_trans_b_iot", 1)[1].split(
            "static bool trans_b_iot", 1
        )[0]
        self.assertLess(
            body.index("has_size && size_code > 10u"),
            body.index("if (pe_mask == 0u)"),
        )

    def test_zero_size_code_descriptor_skips_tile_allocation(self) -> None:
        translate = TRANSLATE.read_text(encoding="utf-8")
        text = HELPER.read_text(encoding="utf-8")
        self.assertIn("local_size_code, has_size", translate)
        self.assertIn(
            "if (desc.has_size && (vector_block || tile_output)) {",
            text,
        )

    def test_last_use_payloads_commit_after_success(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        commit = text.split("void HELPER(linx_tile_commit)", 1)[1]
        apply_at = commit.index("linx_tile_apply_source_lifetimes(")
        guarded_at = commit.index("linx_tile_txn_guarded_apply(")
        publish_at = commit.index("memcpy(env->tile_hand_live", apply_at)

        self.assertLess(guarded_at, apply_at)
        self.assertLess(apply_at, publish_at)
        table = (ROOT / "target/linx/tile_isa_058.h").read_text(encoding="utf-8")
        self.assertIn("last_use[LINX_TILE_SLOT_COUNT]", table)
        self.assertIn("retained[LINX_TILE_SLOT_COUNT]", table)
        self.assertIn("tile_bytes[tile] = 0u", table)

    def test_arity_ignores_lifetime_and_early_reset_paths_finalize(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        self.assertIn(
            "(desc.flags & LINX_IOT_ABSENT_MASK) != LINX_IOT_S1V", text
        )
        self.assertIn(
            "(desc.flags & LINX_IOT_ABSENT_MASK) == 0u", text
        )

        single_gmov = text.split(
            "static bool linx_tile_group_gmov_single_pe_commit", 1
        )[1].split("static bool linx_tile_materialize_planned_outputs", 1)[0]
        group_mma = text.split(
            "static bool linx_tile_group_mma_commit", 1
        )[1].split("static bool linx_tile_group_gmov_commit", 1)[0]
        group_gmov = text.split(
            "static bool linx_tile_group_gmov_commit", 1
        )[1].split("static bool linx_tile_commit_writes_gm", 1)[0]
        vector = text.split(
            "static void linx_tile_commit_vector_bindings", 1
        )[1].split("static uint64_t linx_vec_read_reg", 1)[0]

        for success_path in (single_gmov, group_mma, group_gmov):
            self.assertLess(
                success_path.rindex("linx_tile_apply_source_lifetimes("),
                success_path.rindex("linx_tile_group_reset_block("),
            )
        self.assertLess(
            vector.index("linx_tile_apply_source_lifetimes("),
            vector.index("linx_tile_unpin_bindings("),
        )
        self.assertIn(
            "(pe_mask & linx_tile_shared_current_pe_bit(peer_env)) != 0u",
            group_gmov,
        )


if __name__ == "__main__":
    unittest.main()
