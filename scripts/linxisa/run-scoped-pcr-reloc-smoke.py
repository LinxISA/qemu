#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify direct boot of a PTO 0.59.1 object with scoped PCR relocations."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-mc", required=True, type=Path)
    parser.add_argument("--qemu", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "tests/linxisa/scoped_scalar_pcr_reloc.s").read_text()
    env = os.environ | {
        "LINX_QEMU_SCOPED_SCALAR_RUNTIME": "1",
        "LINX_VIRT_TEST_FINISHER": "1",
    }

    with tempfile.TemporaryDirectory(prefix="linx-scoped-pcr-") as directory:
        for label, assembly in (
            ("valid", source),
            ("unaligned", source.replace(".Lbefore + 4", ".Lbefore + 1")),
        ):
            obj = Path(directory) / f"{label}.o"
            commits = Path(directory) / f"{label}.jsonl"
            subprocess.run(
                [args.llvm_mc, "-triple=linx64", "-mattr=+pto-v0591-scoped",
                 "-filetype=obj", "-o", obj],
                input=assembly, text=True, check=True, timeout=10,
            )
            result = subprocess.run(
                [args.qemu, "-nographic", "-monitor", "none", "-machine", "virt",
                 "-smp", "1", "-bios", "none", "-kernel", obj],
                env=env | {"LINX_COMMIT_TRACE": str(commits)},
                capture_output=True, text=True, timeout=10,
            )
            if label == "valid":
                if result.returncode:
                    raise SystemExit(result.stderr or f"guest exited {result.returncode}")
                rows = [json.loads(line) for line in commits.read_text().splitlines()]
                finishers = [row for row in rows if row["mem_valid"] and
                             row["mem_is_store"] and row["mem_addr"] == 0x10009000]
                if len(finishers) != 1 or finishers[0]["mem_wdata"] != 0x5555:
                    raise SystemExit(f"guest did not retire one pass finisher: {finishers}")
            if label == "unaligned" and (
                not result.returncode or "scoped PCR target is unaligned" not in result.stderr
            ):
                raise SystemExit(f"loader did not reject unaligned PCR: {result.stderr}")
    print("ok: direct ELF relocation covers four scoped PCR layouts and rejects unaligned targets")


if __name__ == "__main__":
    main()
