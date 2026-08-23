#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument("--llvm-mc", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    args = parser.parse_args()

    source = root / "tests" / "linxisa" / "csel_value_contract.s"
    with tempfile.TemporaryDirectory(prefix="linx-csel-") as directory:
        image = Path(directory) / "csel_value_contract.o"
        subprocess.run(
            [str(args.llvm_mc), "-triple=linx64", "-filetype=obj", str(source),
             "-o", str(image)],
            check=True,
        )
        completed = subprocess.run(
            [str(args.qemu), "-nographic", "-monitor", "none", "-machine", "virt",
             "-kernel", str(image), "-bios", "none"],
            env={**os.environ, "LINX_VIRT_TEST_FINISHER": "1"},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
        if completed.returncode != 0:
            raise SystemExit(
                "CSEL decoded value contract failed\n"
                + completed.stdout.decode(errors="replace")[-4000:]
            )
    print("PASS: CSEL default=3 and .neg=2")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
