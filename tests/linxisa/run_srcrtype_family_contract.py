#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = ROOT / "tests" / "linxisa"


@dataclass(frozen=True)
class Case:
    name: str
    source: str
    illegal_length: int | None = None


CASES = (
    Case("families", "srcrtype_family_contract.s"),
    Case("reserved32", "srcrtype_address_reserved32_contract.s", 4),
    Case("reserved48", "srcrtype_address_reserved48_contract.s", 6),
)


def evaluate_case(case: Case, returncode: int, output: str) -> tuple[bool, str]:
    if returncode != 0:
        return False, f"qemu exited {returncode}"
    if case.illegal_length is None:
        return True, "family results reached the pass finisher"

    marker = f"insn_len={case.illegal_length}"
    if "illegal instruction" not in output or marker not in output:
        return False, f"missing illegal-instruction evidence ({marker})"
    if (
        "data page fault" in output.lower()
        or "data alignment" in output.lower()
    ):
        return False, "reserved address selector reached a memory-fault path"
    return (
        True,
        f"illegal selector trapped before destination effect ({marker})",
    )


def require_tool(path: str, label: str) -> Path:
    selected = Path(path).expanduser().absolute()
    if not selected.is_file() or not os.access(selected, os.X_OK):
        raise SystemExit(f"error: {label} is not executable: {selected}")
    return selected


def run_command(
    command: list[str],
    *,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
    )


def run_case(
    case: Case,
    *,
    clang: Path,
    lld: Path,
    qemu: Path,
    out_dir: Path,
    timeout: float,
) -> tuple[bool, str]:
    source = TEST_DIR / case.source
    obj = out_dir / f"{case.name}.o"
    kernel = out_dir / f"{case.name}.kernel.o"
    log = out_dir / f"{case.name}.qemu.log"

    compile_result = run_command(
        [
            str(clang),
            "-target",
            "linx64",
            "-nostdlib",
            "-c",
            str(source),
            "-o",
            str(obj),
        ]
    )
    if compile_result.returncode != 0:
        return (
            False,
            f"clang exited {compile_result.returncode}: "
            f"{compile_result.stdout.strip()}",
        )

    link_result = run_command([str(lld), "-r", "-o", str(kernel), str(obj)])
    if link_result.returncode != 0:
        return (
            False,
            f"ld.lld exited {link_result.returncode}: "
            f"{link_result.stdout.strip()}",
        )

    qemu_env = dict(os.environ)
    qemu_env["LINX_VIRT_TEST_FINISHER"] = "1"
    try:
        qemu_result = run_command(
            [
                str(qemu),
                "-machine", "virt",
                "-kernel", str(kernel),
                "-nographic",
                "-monitor", "none",
                "-no-reboot",
                "-bios", "none",
                "-d", "guest_errors",
            ],
            env=qemu_env,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, f"qemu timed out after {timeout:g}s"

    log.write_text(qemu_result.stdout, encoding="utf-8")
    return evaluate_case(case, qemu_result.returncode, qemu_result.stdout)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run PTO SrcRType family contracts"
    )
    parser.add_argument(
        "--clang", required=True, help="exact Linx clang binary"
    )
    parser.add_argument("--lld", required=True, help="exact ld.lld binary")
    parser.add_argument(
        "--qemu", required=True, help="exact qemu-system-linx64 binary"
    )
    parser.add_argument(
        "--out-dir", help="preserve objects and per-case logs here"
    )
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    clang = require_tool(args.clang, "clang")
    lld = require_tool(args.lld, "ld.lld")
    qemu = require_tool(args.qemu, "qemu-system-linx64")

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.out_dir:
        out_dir = Path(args.out_dir).expanduser().resolve()
        out_dir.mkdir(parents=True, exist_ok=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="linx-srcrtype-")
        out_dir = Path(temporary.name)

    failed = False
    try:
        for case in CASES:
            ok, detail = run_case(
                case,
                clang=clang,
                lld=lld,
                qemu=qemu,
                out_dir=out_dir,
                timeout=args.timeout,
            )
            print(f"{'PASS' if ok else 'FAIL'} {case.name}: {detail}")
            failed = failed or not ok
    finally:
        if temporary is not None:
            temporary.cleanup()

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
