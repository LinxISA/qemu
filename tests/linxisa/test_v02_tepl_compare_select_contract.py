#!/usr/bin/env python3
"""PTO ISA v0.2 compare/select TEPL profile contract checks."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "target/linx/helper.c"
TILE_ISA = ROOT / "target/linx/tile_isa_057.h"


class V02TeplCompareSelectContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.helper = HELPER.read_text(encoding="utf-8")
        cls.tile_isa = TILE_ISA.read_text(encoding="utf-8")

    def test_profile_gate_runs_before_pin_and_output_reservation(self) -> None:
        start = self.helper.index("void HELPER(linx_tile_append_iot)")
        end = self.helper.index("static bool linx_tile_cube_primary_legal", start)
        append = self.helper[start:end]
        validation = append.index("linx_tile_tepl_compare_select_profile_valid")
        first_pin = append.index("env->tile_pin_owner", validation)
        first_reservation = append.index("linx_tile_reserve_output", first_pin)
        self.assertLess(validation, first_pin)
        self.assertLess(validation, first_reservation)

    def test_gate_checks_latest_selector_and_operand_contracts(self) -> None:
        start = self.helper.index(
            "static bool linx_tile_tepl_compare_select_profile_valid"
        )
        end = self.helper.index("static int linx_tile_tepl_source_arity", start)
        gate = self.helper[start:end]
        for selector in ("0x0du", "0x2du", "0x1au", "0x3au"):
            self.assertIn(selector, gate)
        for contract in (
            "env->tile_attr_dtype & 0x100u",
            "((env->tile_attr_raw >> 22) & 0x7u) > 5u",
            "const uint32_t mask_words = (cols + 31u) / 32u",
            "env->tile_reg_dtype[mask_tile] & 0x1fu",
            "env->tile_reg_elem_bytes[mask_tile] == 4u",
            "env->tile_iot_count == 1u",
        ):
            self.assertIn(contract, gate)

    def test_compare_datr_allows_dtype_and_cmode(self) -> None:
        start = self.tile_isa.index("linx_tile_tepl_datr_allowed")
        end = self.tile_isa.index("linx_tile_datr_allowed", start)
        table = self.tile_isa[start:end]
        self.assertEqual(table.count("0x44"), 2)
        self.assertIn("LINX_DATR_DATA_TYPE", self.tile_isa)
        self.assertIn("LINX_DATR_CMODE", self.tile_isa)

    def test_scalar_compare_select_requires_one_b_ior(self) -> None:
        start = self.helper.index("static bool linx_tile_tepl(CPULinxState")
        end = self.helper.index("static bool linx_tile_cube_primary_legal", start)
        execute = self.helper[start:end]
        self.assertIn(
            "const bool scalar_compare_select = impl_op == 0x033u || "
            "impl_op == 0x034u;",
            execute,
        )
        self.assertIn("env->tile_ior_count == 1u", execute)

    def test_transaction_preflight_restores_destination_snapshot(self) -> None:
        start = self.helper.index("static bool linx_tile_preflight_tepl")
        end = self.helper.index("static bool linx_tile_apply_materialization", start)
        preflight = self.helper[start:end]
        dry_execute = preflight.index("linx_tile_tepl(env")
        restore = preflight.index("linx_tile_restore_reg", dry_execute)
        self.assertLess(dry_execute, restore)


if __name__ == "__main__":
    unittest.main()
