#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check compressed scoped results and pre-effect faults on empty queues."""

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
    source = root / "tests/linxisa/scoped_scalar_runtime_queues.s"
    env = os.environ | {
        "LINX_QEMU_SCOPED_SCALAR_RUNTIME": "1", "LINX_VIRT_TEST_FINISHER": "1",
    }
    with tempfile.TemporaryDirectory(prefix="linx-scoped-queue-") as tmp:
        for kind, address, expected in (
            (0, 0x20040, struct.pack("<q", -0x7FFFFFFB) * 3),
            (1, 0x20000, struct.pack("<Q", 0x1122)),
            (2, 0x20000, struct.pack("<Q", 0x1122)),
        ):
            obj = Path(tmp) / f"case-{kind}.o"
            output = Path(tmp) / f"result-{kind}.bin"
            subprocess.run(
                [args.llvm_mc, "-triple=linx64", "-mattr=+pto-v0591-scoped",
                 "-filetype=obj", f"--defsym=FAULT={kind}", source, "-o", obj],
                check=True, timeout=10,
            )
            machine = (
                f"virt,cross-model-dump={output},cross-model-address={address},"
                f"cross-model-size={len(expected)}"
            )
            subprocess.run(
                [args.qemu, "-nographic", "-monitor", "none", "-machine",
                 machine, "-smp", "1", "-bios", "none", "-kernel", obj],
                env=env, check=True, timeout=10,
            )
            actual = output.read_bytes()
            if actual != expected:
                raise SystemExit(
                    f"queue case {kind}: {actual.hex()} != {expected.hex()}"
                )
    print("ok: all 12 compressed carriers and both empty-queue faults matched")


if __name__ == "__main__":
    main()
