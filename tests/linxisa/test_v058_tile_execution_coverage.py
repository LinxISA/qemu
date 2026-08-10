#!/usr/bin/env python3
"""Generated-contract coverage for PTO ISA 0.58 tile execution."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = (ROOT / "target/linx/helper.c").read_text()
HEADER = (ROOT / "target/linx/tile_isa_058.h").read_text()


class TileExecutionCoverage(unittest.TestCase):
    def test_every_accepted_vec_sfu_selector_has_an_executable_mapping(self):
        masks_match = re.search(
            r"function_masks\[4\]\s*=\s*\{(.*?)\};", HEADER, re.S
        )
        self.assertIsNotNone(masks_match)
        masks = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)", masks_match.group(1))]
        accepted = {
            mode * 32 + function
            for mode, mask in enumerate(masks)
            for function in range(32)
            if mask & (1 << function)
        }
        mapping_body = re.search(
            r"linx_tile_operation_impl_selector\(.*?\)\s*\{(.*?)\n\}", HELPER, re.S
        ).group(1)
        mappings = {
            int(selector, 16): int(implementation, 16)
            for selector, implementation in re.findall(
                r"case 0x([0-9a-f]+)u:.*?return 0x([0-9a-f]+)u;",
                mapping_body,
            )
        }
        executable_body = re.search(
            r"linx_tile_operation_impl_selector_executable\(.*?\)\s*\{(.*?)\n\}",
            HELPER,
            re.S,
        ).group(1)
        executable = {
            int(value, 16)
            for value in re.findall(r"case 0x([0-9a-f]+)u:", executable_body)
        }
        selector_body = re.search(
            r"linx_tile_operation_selector_executable\(.*?\)\s*\{(.*?)\n\}",
            HELPER,
            re.S,
        ).group(1)
        executable_selectors = {
            int(value, 16)
            for value in re.findall(r"case 0x([0-9a-f]+)u:", selector_body)
        }
        self.assertEqual(87, len(accepted))
        self.assertEqual(accepted, mappings.keys())
        self.assertEqual(accepted, executable_selectors)
        self.assertEqual(set(), set(mappings.values()) - executable)

        vec_match = re.search(
            r"vec_function_masks\[4\]\s*=\s*\{(.*?)\};", HEADER, re.S
        )
        self.assertIsNotNone(vec_match)
        vec_masks = [int(x, 16) for x in re.findall(
            r"0x([0-9a-f]+)", vec_match.group(1)
        )]
        self.assertEqual(35, sum(mask.bit_count() for mask in vec_masks))
        self.assertEqual(52, len(accepted) - sum(
            mask.bit_count() for mask in vec_masks
        ))

    def test_all_cube_functions_have_execution_cases(self):
        cube_body = re.search(
            r"linx_tile_cube_function_accepted\(.*?\n\}", HEADER, re.S
        ).group(0)
        accepted_mask = int(re.search(r"0x([0-9a-f]+)", cube_body).group(1), 16)
        accepted = {bit for bit in range(32) if accepted_mask & (1 << bit)}
        enum_body = re.search(r"enum \{\s*LINX_CUBE_TMATMUL(.*?)\n\};", HELPER, re.S).group(0)
        enum_values = {
            int(value)
            for value in re.findall(r"LINX_CUBE_[A-Z0-9_]+\s*=\s*(\d+)", enum_body)
        }
        self.assertEqual({0, 1, 2, 4, 5, 6, 8, 16, 17, 18, 20, 21, 22}, accepted)
        self.assertEqual(accepted, enum_values)
        for name in re.findall(r"(LINX_CUBE_[A-Z0-9_]+)\s*=", enum_body):
            self.assertRegex(HELPER, rf"case {name}:")

    def test_implicit_acc_and_catalog_datr_guards_are_executable_contracts(self):
        collector = re.search(
            r"linx_tile_collect_cube_sources\(.*?\n\}", HELPER, re.S
        ).group(0)
        self.assertIn("ACC is implicit", collector)
        self.assertIn("if (d.has_size)", collector)
        self.assertIn("return false", collector)
        self.assertIn("linx_tile_datr_applicable", HELPER)
        self.assertIn("Generated from pto-spec", HEADER)


if __name__ == "__main__":
    unittest.main()
