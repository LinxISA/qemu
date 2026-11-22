#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot directly relocated and LLD-linked PTO 0.59.1 scoped PCR images."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def boot(qemu, image, trace, env):
    return subprocess.run(
        [qemu, "-nographic", "-monitor", "none", "-machine", "virt",
         "-smp", "1", "-bios", "none", "-kernel", image],
        env=env | {"LINX_COMMIT_TRACE": str(trace)},
        capture_output=True, text=True, timeout=10,
    )


def check_pass(result, trace):
    if result.returncode:
        raise SystemExit(result.stderr or f"guest exited {result.returncode}")
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    finishers = [row for row in rows if row["mem_valid"] and
                 row["mem_is_store"] and row["mem_addr"] == 0x10009000]
    if len(finishers) != 1 or finishers[0]["mem_wdata"] != 0x5555:
        raise SystemExit(f"guest did not retire one pass finisher: {finishers}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-mc", required=True, type=Path)
    parser.add_argument("--ld-lld", required=True, type=Path)
    parser.add_argument("--qemu", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "tests/linxisa/scoped_scalar_pcr_reloc.s").read_text()
    env = os.environ | {
        "LINX_QEMU_SCOPED_SCALAR_RUNTIME": "1",
        "LINX_VIRT_TEST_FINISHER": "1",
    }

    with tempfile.TemporaryDirectory(prefix="linx-scoped-pcr-") as tmp:
        directory = Path(tmp)
        for label, assembly in (
            ("valid", source),
            ("unaligned", source.replace(".Lbefore + 4", ".Lbefore + 1")),
        ):
            obj = directory / f"{label}.o"
            commits = directory / f"{label}.jsonl"
            subprocess.run(
                [args.llvm_mc, "-triple=linx64", "-mattr=+pto-v0591-scoped",
                 "-filetype=obj", "-o", obj],
                input=assembly, text=True, check=True, timeout=10,
            )
            result = boot(args.qemu, obj, commits, env)
            if label == "valid":
                check_pass(result, commits)
                elf = directory / "linked.elf"
                linked_trace = directory / "linked.jsonl"
                subprocess.run(
                    [args.ld_lld, "-m", "elf64linx", "--linx-pto-v0591-scoped",
                     "-Ttext=0x10000", "-e", "_start", "-o", elf, obj],
                    check=True, timeout=10,
                )
                check_pass(boot(args.qemu, elf, linked_trace, env), linked_trace)
            elif (
                not result.returncode or "scoped PCR target is unaligned" not in result.stderr
            ):
                raise SystemExit(f"loader did not reject unaligned PCR: {result.stderr}")
    print("ok: direct and LLD-linked boots cover all four scoped PCR layouts")


if __name__ == "__main__":
    main()
