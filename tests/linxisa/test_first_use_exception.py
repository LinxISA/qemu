#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exact source and executable-test contract for extension first-use traps."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target/linx"


class FirstUseExceptionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cpu = (TARGET / "cpu.c").read_text(encoding="utf-8")
        cls.cpu_h = (TARGET / "cpu.h").read_text(encoding="utf-8")
        cls.helper = (TARGET / "helper.c").read_text(encoding="utf-8")
        cls.helper_h = (TARGET / "helper.h").read_text(encoding="utf-8")
        cls.translate = (TARGET / "translate.c").read_text(encoding="utf-8")
        first_use_h = TARGET / "first_use.h"
        first_use_c = TARGET / "first_use.c"
        cls.first_use_h = (
            first_use_h.read_text(encoding="utf-8") if first_use_h.is_file() else ""
        )
        cls.first_use_c = (
            first_use_c.read_text(encoding="utf-8") if first_use_c.is_file() else ""
        )
        cls.unit_meson = (ROOT / "tests/unit/meson.build").read_text(encoding="utf-8")
        cls.executable_fixture = (
            ROOT / "tests/linxisa/first_use_exception_contract.s"
        ).read_text(encoding="utf-8")
        cls.executable_runner = (
            ROOT / "scripts/linxisa/run-first-use-exception-contract.py"
        ).read_text(encoding="utf-8")

    def test_econfig_layout_reset_and_mask_are_exact(self) -> None:
        self.assertTrue(self.first_use_h, "target/linx/first_use.h is missing")
        self.assertTrue(self.first_use_c, "target/linx/first_use.c is missing")
        self.assertIn("LINX_SSR_ECONFIG 0xF07", self.first_use_h)
        self.assertIn("UINT64_C(0x0000000300000008)", self.first_use_h)
        self.assertIn("UINT64_C(0x000000030000000f)", self.first_use_h)
        self.assertIn("linx_first_use_reset", self.cpu)
        self.assertIn("linx_econfig_sanitize", self.helper)

    def test_trap_envelope_and_source_acr_are_exact(self) -> None:
        self.assertIn("LINX_FIRST_USE_CAUSE UINT32_C(4)", self.first_use_h)
        self.assertIn("LINX_FIRST_USE_VECTOR = 0", self.first_use_h)
        self.assertIn("LINX_FIRST_USE_CUBE = 1", self.first_use_h)
        self.assertIn("env->acr != 2u", self.first_use_c)
        self.assertIn("env->ssr_acr[1][LINX_SSR_ECONFIG]", self.first_use_c)
        self.assertIn("LINX_EXCP_EXTENSION_FIRST_USE", self.cpu_h)
        self.assertIn("LINX_TRAPNUM_INSN_EXP", self.cpu)
        self.assertRegex(
            self.cpu,
            r"case LINX_EXCP_EXTENSION_FIRST_USE:[\s\S]*?true,\s*/\* argv \*/"
            r"[\s\S]*?false,\s*/\* fault \*/[\s\S]*?false\s*/\* BI \*/",
        )

    def test_vector_headers_check_before_block_begin(self) -> None:
        for name in ("mpar", "mseq"):
            for prefix in ("", "c_"):
                body = re.search(
                    rf"static bool trans_{prefix}bstart_{name}\(.*?\n\}}",
                    self.translate,
                    re.S,
                ).group(0)
                self.assertIn("linx_gen_extension_first_use", body)
                self.assertLess(
                    body.index("linx_gen_extension_first_use"),
                    body.index("linx_block_begin"),
                )
        self.assertIn("return trans_bstart_mpar", self.translate)
        self.assertIn("return trans_bstart_mseq", self.translate)
        self.assertIn("return trans_c_bstart_mpar", self.translate)
        self.assertIn("return trans_c_bstart_mseq", self.translate)

    def test_cube_only_tile_headers_are_checked(self) -> None:
        body = re.search(
            r"static bool trans_bstart_tile_func_common\([^;]+?\n\{.*?\n\}",
            self.translate,
            re.S,
        ).group(0)
        self.assertIn("blocktype == LINX_FIRST_USE_BLOCK_CUBE", body)
        self.assertLess(
            body.index("linx_gen_extension_first_use"),
            body.index("linx_block_begin"),
        )
        tepl = re.search(
            r"static bool trans_bstart_tile_common\([^;]+?\n\{.*?\n\}",
            self.translate,
            re.S,
        ).group(0)
        self.assertNotIn("linx_gen_extension_first_use", tepl)
        cube_functions = (
            "tmatmul",
            "tmatmul_bias",
            "tmatmul_acc",
            "tmatmulmx",
            "tmatmulmx_bias",
            "tmatmulmx_acc",
            "tgemv",
            "tgemv_bias",
            "tgemv_acc",
            "tgemvmx",
            "tgemvmx_bias",
            "tgemvmx_acc",
        )
        for name in cube_functions:
            function = re.search(
                rf"static bool trans_bstart_{name}\(.*?\n\}}",
                self.translate,
                re.S,
            ).group(0)
            self.assertIn("trans_bstart_tile_func_common", function)
            self.assertRegex(function, r",\s*6,\s*(?:0|1|2|4|5|6|16|17|18|20|21|22)\)")

    def test_runtime_and_native_regression_are_wired(self) -> None:
        self.assertIn(
            "DEF_HELPER_3(linx_extension_first_use, void, env, i32, i64)",
            self.helper_h,
        )
        self.assertIn("HELPER(linx_extension_first_use)", self.helper)
        self.assertIn("test-linx-first-use", self.unit_meson)
        self.assertIn(
            "VMSTATE_UINT64_2DARRAY(env.ssr_acr, LinxCPU, LINX_ACR_COUNT, LINX_SSR_COUNT)",
            self.cpu,
        )

    def test_guest_isa_matrix_is_executable_and_fail_closed(self) -> None:
        for header in (
            "BSTART.MPAR 0",
            "BSTART.MSEQ 0",
            "BSTART.VPAR 0",
            "BSTART.VSEQ 0",
            "BSTART.TMATMUL FP16",
            "BSTART.TMATMUL.BIAS FP16",
            "BSTART.TMATMUL.ACC FP16",
            "BSTART.TMATMULMX FP16",
            "BSTART.TMATMULMX.BIAS FP16",
            "BSTART.TMATMULMX.ACC FP16",
            "BSTART.TGEMV FP16",
            "BSTART.TGEMV.BIAS FP16",
            "BSTART.TGEMV.ACC FP16",
            "BSTART.TGEMVMX FP16",
            "BSTART.TGEMVMX.BIAS FP16",
            "BSTART.TGEMVMX.ACC FP16",
        ):
            self.assertIn(header, self.executable_fixture)
        for raw in ("0x08c0", "0x48c0", "0x88c0", "0xc8c0"):
            self.assertIn(raw, self.executable_fixture)
        self.assertIn("VECTOR_HEADERS", self.executable_runner)
        self.assertIn("CUBE_HEADERS", self.executable_runner)
        self.assertIn("assert_first_use", self.executable_runner)
        self.assertIn('"cause": "0x4"', self.executable_runner)
        self.assertIn('"pending_arg0": hex(kind)', self.executable_runner)
        self.assertIn('"src_blocktype": "0"', self.executable_runner)
        self.assertIn('"src_tq0": "0x0"', self.executable_runner)
        self.assertIn('"src_uq0": "0x0"', self.executable_runner)
        self.assertIn('"src_lb": "0x0:0:0"', self.executable_runner)
        self.assertIn('"src_lc": "0x0:0:0"', self.executable_runner)
        self.assertIn('(\"VECTOR retry\", 6, 0)', self.executable_runner)
        self.assertIn('(\"CUBE retry\", 8, 1)', self.executable_runner)
        self.assertIn('(\"VECTOR preserves CUBE\", 6, 0, (0, 1))', self.executable_runner)
        self.assertIn('(\"CUBE preserves VECTOR\", 8, 1, (1, 0))', self.executable_runner)
        self.assertIn('(\"TEPL carrier\", 20, 2)', self.executable_runner)
        self.assertIn('(\"ACR0 VECTOR\", 6, 0)', self.executable_runner)
        self.assertIn('(\"ACR1 VECTOR\", 6, 1)', self.executable_runner)
        self.assertIn("first-use took priority over illegal decode", self.executable_runner)
        self.assertIn(".4byte 0x78031181", self.executable_fixture)


if __name__ == "__main__":
    unittest.main()
