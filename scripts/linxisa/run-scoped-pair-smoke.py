#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check scoped pair data and precise denial of the second access."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-mc", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = root / "tests/linxisa/scoped_scalar_runtime_pair.s"
    env = os.environ | {
        "LINX_QEMU_SCOPED_SCALAR_RUNTIME": "1", "LINX_VIRT_TEST_FINISHER": "1",
    }
    with tempfile.TemporaryDirectory(prefix="linx-scoped-pair-") as tmp:
        for kind, address, expected in (
            (0, 0x20030, struct.pack("<QQ", 0x1122, 0x3344)),
            (1, 0xFFFFF8, struct.pack("<Q", 0x5566)),
            (2, 0xFFFFF8, struct.pack("<Q", 0x5566)),
        ):
            obj = Path(tmp) / f"case-{kind}.o"
            output = Path(tmp) / f"result-{kind}.bin"
            subprocess.run(
                [args.llvm_mc, "-triple=linx64", "-mattr=+pto-v0591-scoped",
                 "-filetype=obj", f"--defsym=FAULT={kind}", source, "-o", obj],
                check=True, timeout=10,
            )
            machine = (f"virt,cross-model-dump={output},cross-model-address={address},"
                       f"cross-model-size={len(expected)}")
            subprocess.run(
                [args.qemu, "-nographic", "-monitor", "none", "-machine", machine,
                 "-smp", "1", "-m", "16M", "-bios", "none", "-kernel", obj],
                env=env, check=True, timeout=10,
            )
            actual = output.read_bytes()
            if actual != expected:
                raise SystemExit(f"pair case {kind}: {actual.hex()} != {expected.hex()}")
    print("ok: scoped pairs publish both results and preflight both addresses")


if __name__ == "__main__":
    main()
