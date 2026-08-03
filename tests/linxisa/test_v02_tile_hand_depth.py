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

    def test_all_physical_tile_bounds_use_slot_count(self) -> None:
        sources = {
            "helper.c": self.helper,
            "tile_cube_057.c": (
                ROOT / "target/linx/tile_cube_057.c"
            ).read_text(encoding="utf-8"),
            "tile_tepl_preflight.h": (
                ROOT / "target/linx/tile_tepl_preflight.h"
            ).read_text(encoding="utf-8"),
            "tile_isa_057.h": (
                ROOT / "target/linx/tile_isa_057.h"
            ).read_text(encoding="utf-8"),
        }
        stale_guards = (
            "tile >= 32u",
            "tile < 32u",
            "src >= 32u",
            "src0 < 32u",
            "src_a < 32u",
            "src_a >= 32u",
            "src_b < 32u",
            "src_b >= 32u",
            "dst >= 32u",
            "dst_tile >= 32u",
            "value_dst >= 32u",
            "index_dst >= 32u",
            "source >= 32u",
            "tile_bytes[32]",
        )
        for name, source in sources.items():
            with self.subTest(source=name):
                for stale_guard in stale_guards:
                    self.assertNotIn(stale_guard, source)
        self.assertIn(
            "const uint32_t tile_bytes[LINX_TILE_SLOT_COUNT]",
            sources["tile_isa_057.h"],
        )

    def test_vector_tbase_preserves_slot_sixty_three_for_local_access(
        self,
    ) -> None:
        resolve = self.helper[
            self.helper.index("static bool linx_vec_resolve_tile_base") :
            self.helper.index("static void linx_tile_commit_vector_bindings")
        ]
        self.assertNotIn("*tile_out = inputs[base_idx] & 0x1f;", resolve)
        self.assertNotIn("*tile_out = outputs[output] & 0x1f;", resolve)
        self.assertEqual(resolve.count("tile >= LINX_TILE_SLOT_COUNT"), 2)
        self.assertEqual(resolve.count("*tile_out = tile;"), 2)

        read_reg_start = self.helper.index("static uint64_t linx_vec_read_reg")
        tbase_start = self.helper.index(
            "case LINX_VEC_REGCLASS_TBASE:", read_reg_start
        )
        read_tbase = self.helper[
            tbase_start : self.helper.index("default:", tbase_start)
        ]
        self.assertIn("return (uint64_t)tile;", read_tbase)
        self.assertNotIn("& 0x1f", read_tbase)

        local_resolve = self.helper[
            self.helper.index("static bool linx_vec_resolve_local_tile") :
            self.helper.index("static bool linx_vec_local_ensure_store_bytes")
        ]
        self.assertIn("tile >= LINX_TILE_SLOT_COUNT", local_resolve)
        self.assertIn("*tile_out = tile;", local_resolve)

        local_access = self.helper[
            self.helper.index("void HELPER(linx_v_sw_local)") :
            self.helper.index("static unsigned linx_insn_len")
        ]
        self.assertEqual(
            local_access.count("linx_vec_resolve_local_tile(env, srcL, &tile)"),
            2,
        )
        self.assertIn("env->tile_reg[tile][word]", local_access)

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
