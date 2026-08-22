#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute PTO 0.58.3 Local CELL and Shared transpose guest vectors."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run_case(llvm_mc: Path, qemu: Path, source: Path, smp: int) -> None:
    with tempfile.TemporaryDirectory(prefix="linx-cube-0583-") as directory:
        image = Path(directory) / f"{source.stem}.o"
        subprocess.run(
            [str(llvm_mc), "-triple=linx64", "-filetype=obj", str(source),
             "-o", str(image)],
            check=True,
        )
        completed = subprocess.run(
            [str(qemu), "-nographic", "-monitor", "none", "-machine", "virt",
             "-smp", str(smp), "-kernel", str(image), "-bios", "none"],
            env={**os.environ, "LINX_VIRT_TEST_FINISHER": "1"},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
        )
        if completed.returncode != 0:
            raise AssertionError(
                f"{source.name}: guest failed ({completed.returncode})\n"
                + completed.stdout.decode(errors="replace")[-4000:]
            )


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--llvm-mc", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    args = parser.parse_args()
    run_case(args.llvm_mc, args.qemu,
             root / "tests/linxisa/cube_cell_guest_contract.s", 1)
    run_case(args.llvm_mc, args.qemu,
             root / "tests/linxisa/cube_shared_transpose_guest_contract.s", 4)
    print("PASS: PTO 0.58.3 CUBE CELL and Shared TransA/TransB guests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
