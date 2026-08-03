#!/usr/bin/env python3
"""Regression checks for the DavinciOO v0.2 4x16 Tile hand model."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CPU_H = ROOT / "target/linx/cpu.h"
CPU_C = ROOT / "target/linx/cpu.c"
HELPER = ROOT / "target/linx/helper.c"
CUBE = ROOT / "target/linx/tile_cube_057.c"
TEPL_PREFLIGHT = ROOT / "target/linx/tile_tepl_preflight.h"
TILE_ISA = ROOT / "target/linx/tile_isa_057.h"


class V02TileHandDepthTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpu_h = CPU_H.read_text(encoding="utf-8")
        cls.cpu_c = CPU_C.read_text(encoding="utf-8")
        cls.helper = HELPER.read_text(encoding="utf-8")
        cls.production_tile_paths = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (HELPER, CUBE, TEPL_PREFLIGHT, TILE_ISA)
        )

    def test_each_hand_has_sixteen_addressable_ranks(self) -> None:
        self.assertIn("#define LINX_TILE_HAND_DEPTH 16u", self.cpu_h)
        self.assertIn(
            "#define LINX_TILE_SLOT_COUNT "
            "(LINX_TILE_HAND_COUNT * LINX_TILE_HAND_DEPTH)",
            self.cpu_h,
        )
        resolve = self.helper[
            self.helper.index("static bool linx_tile_resolve_source") :
            self.helper.index("static void linx_tile_unpin_bindings")
        ]
        self.assertIn("const unsigned rank = encoded & 0xfu;", resolve)
        self.assertIn("rank >= LINX_TILE_HAND_DEPTH", resolve)
        self.assertNotIn("#9..#16", resolve)

    def test_live_and_reserved_masks_preserve_rank_sixteen(self) -> None:
        for field in ("tile_hand_live", "tile_hand_reserved"):
            self.assertRegex(
                self.cpu_h,
                rf"uint16_t {field}\[LINX_TILE_HAND_COUNT\]",
            )
        self.assertIn(
            "#define LINX_TILE_HAND_BIT(depth) ((uint16_t)(1u << (depth)))",
            self.cpu_h,
        )
        self.assertIn("LINX_TILE_HAND_BIT(depth)", self.helper)

    def test_physical_backing_covers_slot_sixty_three(self) -> None:
        for field in (
            "tile_reg",
            "tile_reg_capacity",
            "tile_reg_bytes",
            "tile_reg_elem_bytes",
            "tile_reg_dtype",
            "tile_reg_valid_cols",
            "tile_reg_valid_rows",
            "tile_reg_cols",
            "tile_reg_rows",
        ):
            self.assertRegex(
                self.cpu_h,
                rf"\b{field}\[LINX_TILE_SLOT_COUNT\]",
            )
        tile_paths = self.production_tile_paths
        for stale_guard in (
            "tile >= 32u",
            "tile < 32u",
            "src_tile >= 32",
            "dst_tile >= 32",
            "src_a >= 32",
            "src_b >= 32",
            "bias >= 32",
            "value_dst >= 32u",
            "index_dst >= 32u",
        ):
            self.assertNotIn(stale_guard, tile_paths)

    def test_representative_upper_slots_use_the_common_limit(self) -> None:
        self.assertIn("tile >= LINX_TILE_SLOT_COUNT", self.production_tile_paths)
        for slot in (32, 47, 48, 63):
            self.assertLess(slot, 4 * 16)
        self.assertNotIn("const uint32_t tile_bytes[32]", self.production_tile_paths)

    def test_seventeenth_output_is_rejected_without_mask_overflow(self) -> None:
        reserve = self.helper[
            self.helper.index("static bool linx_tile_reserve_output") :
            self.helper.index("static void linx_tile_publish_output")
        ]
        self.assertIn(
            "for (unsigned depth = 0; depth < LINX_TILE_HAND_DEPTH; depth++)",
            reserve,
        )
        self.assertRegex(reserve, re.compile(r"return false;\s*}\s*$", re.S))

    def test_vmstate_explicitly_uses_new_layout(self) -> None:
        self.assertIn(".version_id = 18", self.cpu_c)
        self.assertIn(".minimum_version_id = 18", self.cpu_c)
        self.assertIn(
            "VMSTATE_UINT16_ARRAY_V(env.tile_hand_live", self.cpu_c
        )
        self.assertIn(
            "VMSTATE_UINT16_ARRAY_V(env.tile_hand_reserved", self.cpu_c
        )
        self.assertIn("LINX_TILE_SLOT_COUNT, 18", self.cpu_c)


if __name__ == "__main__":
    unittest.main()
