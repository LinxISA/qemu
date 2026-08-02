#!/usr/bin/env python3
"""Source-derived guards for PTO ISA 0.57.1 numeric/layout contracts."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = (ROOT / "target/linx/helper.c").read_text(encoding="utf-8")
TRANSLATE = (ROOT / "target/linx/translate.c").read_text(encoding="utf-8")


class V0571TileNumericLayoutContract(unittest.TestCase):
    def test_all_13_layout_codes_have_exact_source_destination_pairs(self):
        body = re.search(
            r"linx_tile_decode_datr_layout\(.*?\n\}", HELPER, re.S
        ).group(0)
        expected = {
            0: ("ND", "ND"), 1: ("ND", "DN"),
            3: ("ND", "ZN"), 4: ("ND", "NZ"),
            6: ("DN", "ND"), 8: ("DN", "ZN"),
            9: ("DN", "NZ"), 17: ("ZN", "ND"),
            18: ("ZN", "DN"), 20: ("ZN", "NZ"),
            27: ("NZ", "ND"), 28: ("NZ", "DN"),
            30: ("NZ", "ZN"),
        }
        observed = {
            int(code): (src, dst)
            for code, src, dst in re.findall(
                r"case (\d+)u:\s+d\.src = LINX_TILE_LAYOUT_(\w+);\s+"
                r"d\.dst = LINX_TILE_LAYOUT_(\w+);",
                body,
            )
        }
        self.assertEqual(expected, observed)

    def test_blocked_layouts_use_16_by_32_byte_cells(self):
        self.assertIn("MAX(1u, 32u / elem_bytes)", HELPER)
        self.assertIn("const unsigned blk_outer = 16u", HELPER)
        self.assertIn("LINX_TILE_CELL_BYTES 128u", (ROOT / "target/linx/cpu.h").read_text())

    def test_all_rounding_codes_decode_and_cmode_order_is_normative(self):
        datr = re.search(r"static bool trans_b_datr\(.*?\n\}", TRANSLATE, re.S).group(0)
        self.assertNotIn("rmode >", datr)
        cmp_body = re.search(r"linx_tile_tcmp_lane\(.*?\n\}", HELPER, re.S).group(0)
        order = [
            ("0u", "EQ"), ("1u", "NE"), ("2u", "LT"),
            ("3u", "GT"), ("4u", "LE"), ("5u", "GE"),
        ]
        for code, name in order:
            self.assertIn(f"case {code}: /* {name} */", cmp_body)

    def test_canonical_nan_table_matches_hardware_profile(self):
        for literal in (
            "0x7fc00000u", "0x7e00u", "0x7fc0u", "0x7fu", "0x7eu",
            "0x1eu", "0x1cu", "0x7u", "0x6u", "0xffu",
        ):
            self.assertIn(literal, HELPER)
        self.assertIn("Every produced NaN is canonical", HELPER)

    def test_64_bit_tile_memory_carriers_are_not_truncated(self):
        for symbol in (
            "linx_tile_mem_read64", "linx_tile_mem_write64",
            "linx_tile_set_elem64", "linx_tile_get_elem64",
        ):
            self.assertIn(symbol, HELPER)
        self.assertIn("address_space_ldq_le", HELPER)
        self.assertIn("address_space_stq_le", HELPER)

    def test_capacity_is_separate_from_migration_backing_shape(self):
        cpu_h = (ROOT / "target/linx/cpu.h").read_text(encoding="utf-8")
        self.assertIn("LINX_TILE_PE_CAPACITY_BYTES (256u * 1024u)", cpu_h)
        self.assertIn("LINX_TILE_MAX_BYTES (64u * 1024u)", cpu_h)
        self.assertIn("tile_reg_capacity[32]", cpu_h)
        self.assertIn("capacity > LINX_TILE_PE_CAPACITY_BYTES", HELPER)
        self.assertNotIn("capacity > LINX_TILE_MAX_BYTES", HELPER)
        self.assertIn("in_use + capacity > LINX_TILE_PE_CAPACITY_BYTES", HELPER)
        self.assertIn("linx_tile_raise_allocation_fault(env)", HELPER)

    def test_datr_and_cube_auxiliary_operands_preflight_before_effects(self):
        append = re.search(
            r"void HELPER\(linx_tile_append_iot\).*?\n\}", HELPER, re.S
        ).group(0)
        commit = re.search(
            r"void HELPER\(linx_tile_commit\).*?\n\}", HELPER, re.S
        ).group(0)

        # Append is descriptor-only staging: no pin, reservation, backing, or
        # ACC state may become architecturally visible before commit.
        for mutation in (
            "linx_tile_pin_source", "tile_hand_reserved[hand] |=",
            "tile_reg_capacity[", "tile_acc[",
        ):
            self.assertNotIn(mutation, append)
        apply_gate = "linx_tile_txn_guarded_apply"
        self.assertLess(commit.index("linx_tile_datr_applicable"),
                        commit.index(apply_gate))
        self.assertLess(commit.index("linx_tile_preflight_cube"),
                        commit.index(apply_gate))
        self.assertLess(commit.index("linx_tile_preflight_talloc"),
                        commit.index(apply_gate))

    def test_fp64_s64_u64_have_executable_tepl_carriers(self):
        for literal in (
            "LINX_TILE_DTYPE_MASK(0u)",
            "LINX_TILE_DTYPE_MASK(16u)",
            "LINX_TILE_DTYPE_MASK(24u)",
            "0x7ff8000000000000",
        ):
            self.assertIn(literal, HELPER.lower() if literal.startswith("0x") else HELPER)
        self.assertIn("linx_tile_tepl_binary_qword", HELPER)
        self.assertIn("linx_tile_tepl_convert64", HELPER)
        self.assertIn("elem_bytes != 8u", HELPER)


if __name__ == "__main__":
    unittest.main()
