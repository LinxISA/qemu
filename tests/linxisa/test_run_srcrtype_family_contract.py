# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_srcrtype_family_contract import Case, evaluate_case


class SrcRTypeRunnerTest(unittest.TestCase):
    def test_positive_case_requires_zero_exit(self) -> None:
        case = Case("families", "unused.s")
        self.assertTrue(evaluate_case(case, 0, "")[0])
        self.assertFalse(evaluate_case(case, 1, "")[0])

    def test_reserved_case_requires_exact_illegal_evidence(self) -> None:
        case = Case("reserved32", "unused.s", 4)
        self.assertTrue(
            evaluate_case(case, 0, "Linx: illegal instruction (insn_len=4)")[0]
        )
        self.assertFalse(evaluate_case(case, 0, "")[0])
        self.assertFalse(
            evaluate_case(case, 0, "Linx: illegal instruction (insn_len=6)")[0]
        )

    def test_reserved_case_rejects_memory_fault_substitution(self) -> None:
        case = Case("reserved48", "unused.s", 6)
        self.assertFalse(
            evaluate_case(
                case,
                0,
                "Linx: illegal instruction (insn_len=6) data page fault",
            )[0]
        )


if __name__ == "__main__":
    unittest.main()
