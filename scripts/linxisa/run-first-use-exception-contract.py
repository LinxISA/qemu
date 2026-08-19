#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the Linx VECTOR/CUBE first-use contract at the guest ISA boundary."""

from __future__ import annotations

import argparse
import os
import selectors
import subprocess
import tempfile
import time
from pathlib import Path


TRACE_PREFIX = "LINX_TRAP_DELIVERY_TRACE "
VECTOR_HEADERS = (
    "BSTART.MPAR",
    "BSTART.MSEQ",
    "BSTART.VPAR",
    "BSTART.VSEQ",
    "C.BSTART.MPAR",
    "C.BSTART.MSEQ",
    "C.BSTART.VPAR",
    "C.BSTART.VSEQ",
)
CUBE_HEADERS = (
    "BSTART.TMATMUL",
    "BSTART.TMATMUL.BIAS",
    "BSTART.TMATMUL.ACC",
    "BSTART.TMATMULMX",
    "BSTART.TMATMULMX.BIAS",
    "BSTART.TMATMULMX.ACC",
    "BSTART.TGEMV",
    "BSTART.TGEMV.BIAS",
    "BSTART.TGEMV.ACC",
    "BSTART.TGEMVMX",
    "BSTART.TGEMVMX.BIAS",
    "BSTART.TGEMVMX.ACC",
)


def trace_fields(line: str) -> dict[str, str]:
    if not line.startswith(TRACE_PREFIX):
        raise AssertionError(f"not a Linx trap trace: {line}")
    return dict(token.split("=", 1) for token in line.split()[1:] if "=" in token)


def first_use_lines(output: str) -> list[str]:
    return [
        line
        for line in output.splitlines()
        if line.startswith(TRACE_PREFIX) and trace_fields(line).get("cause") == "0x4"
    ]


def run_qemu(
    qemu: Path, image: Path, *, stop_after_traces: int | None
) -> tuple[int, str]:
    env = os.environ.copy()
    env.update(
        {
            "LINX_TRAP_DELIVERY_TRACE": "1",
            "LINX_TRAP_DELIVERY_TRACE_LIMIT": "4",
            "LINX_VIRT_TEST_FINISHER": "1",
        }
    )
    process = subprocess.Popen(
        [
            str(qemu),
            "-nographic",
            "-monitor",
            "none",
            "-machine",
            "virt",
            "-kernel",
            str(image),
            "-bios",
            "none",
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    output: list[str] = []
    deadline = time.monotonic() + 4.0
    while time.monotonic() < deadline and process.poll() is None:
        for key, _ in selector.select(timeout=0.05):
            line = key.fileobj.readline()
            if not line:
                continue
            output.append(line)
            trace_count = sum(item.startswith(TRACE_PREFIX) for item in output)
            if stop_after_traces is not None and trace_count >= stop_after_traces:
                process.terminate()
                break
        trace_count = sum(item.startswith(TRACE_PREFIX) for item in output)
        if stop_after_traces is not None and trace_count >= stop_after_traces:
            break
    if process.poll() is None:
        process.terminate()
    try:
        tail, _ = process.communicate(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        tail, _ = process.communicate()
    output.append(tail)
    return process.returncode or 0, "".join(output)


def assemble(
    llvm_mc: Path,
    source: Path,
    output: Path,
    *,
    test_case: int,
    source_acr: int,
    retry_kind: int,
    cross_kind: int,
) -> None:
    subprocess.run(
        [
            str(llvm_mc),
            "-triple=linx64",
            "-filetype=obj",
            f"--defsym=TEST_CASE={test_case}",
            f"--defsym=SOURCE_ACR={source_acr}",
            f"--defsym=RETRY_KIND={retry_kind}",
            f"--defsym=CROSS_KIND={cross_kind}",
            str(source),
            "-o",
            str(output),
        ],
        check=True,
    )


def assert_first_use(line: str, kind: int, *, pristine: bool = True) -> None:
    fields = trace_fields(line)
    expected = {
        "trapnum": "0",
        "cause": "0x4",
        "argv": "1",
        "is_trap": "0",
        "bi": "0",
        "precise": "1",
        "src_acr": "2",
        "dst_acr": "1",
        "pending_arg0": hex(kind),
        "pending_cause": "0x4",
        "in_body": "0",
        "src_blocktype": "0",
        "src_tq0": "0x0",
        "src_tq1": "0x0",
        "src_tq2": "0x0",
        "src_tq3": "0x0",
        "src_uq0": "0x0",
        "src_uq1": "0x0",
        "src_uq2": "0x0",
        "src_uq3": "0x0",
        "src_lb": "0x0:0:0",
        "src_lc": "0x0:0:0",
        "cstate": "0x2",
    }
    if pristine:
        expected.update({"brtype": "0", "tgt": "0x0"})
    for key, value in expected.items():
        if fields.get(key) != value:
            raise AssertionError(f"{key}: expected {value}, got {fields.get(key)}\n{line}")
    if not (fields["tpc"] == fields["src_bpc"] == fields["report_bpc"]):
        raise AssertionError(f"first-use retry PC is not exact: {line}")
    if int(fields["tpc_next"], 16) <= int(fields["tpc"], 16):
        raise AssertionError(f"first-use next PC did not advance: {line}")


def main() -> int:
    script = Path(__file__).resolve()
    root = script.parents[2]
    linx_root = root.parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--llvm-mc",
        type=Path,
        default=linx_root / "compiler/llvm/build-linxisa-clang/bin/llvm-mc",
    )
    parser.add_argument(
        "--qemu",
        type=Path,
        default=root / "build-linx/qemu-system-linx64",
    )
    args = parser.parse_args()
    for tool in (args.llvm_mc, args.qemu):
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise SystemExit(f"error: required executable is missing: {tool}")

    source = root / "tests/linxisa/first_use_exception_contract.s"
    with tempfile.TemporaryDirectory(prefix="linx-first-use-") as directory:
        temp = Path(directory)
        for test_case, name in enumerate(VECTOR_HEADERS + CUBE_HEADERS):
            image = temp / f"case-{test_case}.o"
            assemble(
                args.llvm_mc,
                source,
                image,
                test_case=test_case,
                source_acr=2,
                retry_kind=-1,
                cross_kind=0,
            )
            _, output = run_qemu(args.qemu, image, stop_after_traces=1)
            lines = first_use_lines(output)
            if len(lines) != 1:
                raise AssertionError(f"{name}: expected one first-use trap\n{output}")
            assert_first_use(lines[0], 0 if test_case < len(VECTOR_HEADERS) else 1)

        for name, test_case, retry_kind in (
            ("VECTOR retry", 6, 0),
            ("CUBE retry", 8, 1),
        ):
            image = temp / f"retry-{retry_kind}.o"
            assemble(
                args.llvm_mc,
                source,
                image,
                test_case=test_case,
                source_acr=2,
                retry_kind=retry_kind,
                cross_kind=0,
            )
            returncode, output = run_qemu(args.qemu, image, stop_after_traces=None)
            if returncode != 0:
                raise AssertionError(f"{name}: QEMU did not reach the finisher\n{output}")
            lines = first_use_lines(output)
            if len(lines) != 1:
                raise AssertionError(f"{name}: faulting header was not retried exactly once\n{output}")
            assert_first_use(lines[0], retry_kind)

        for name, test_case, retry_kind, expected_kinds in (
            ("VECTOR preserves CUBE", 6, 0, (0, 1)),
            ("CUBE preserves VECTOR", 8, 1, (1, 0)),
        ):
            image = temp / f"cross-kind-{retry_kind}.o"
            assemble(
                args.llvm_mc,
                source,
                image,
                test_case=test_case,
                source_acr=2,
                retry_kind=retry_kind,
                cross_kind=1,
            )
            _, output = run_qemu(args.qemu, image, stop_after_traces=2)
            lines = first_use_lines(output)
            if len(lines) < 2:
                raise AssertionError(f"{name}: expected two first-use traps\n{output}")
            for index, (line, kind) in enumerate(
                zip(lines[:2], expected_kinds, strict=True)
            ):
                assert_first_use(line, kind, pristine=index == 0)

        for name, test_case, source_acr in (
            ("TEPL carrier", 20, 2),
            ("ACR0 VECTOR", 6, 0),
            ("ACR1 VECTOR", 6, 1),
        ):
            image = temp / name.lower().replace(" ", "-")
            assemble(
                args.llvm_mc,
                source,
                image,
                test_case=test_case,
                source_acr=source_acr,
                retry_kind=-1,
                cross_kind=0,
            )
            returncode, output = run_qemu(args.qemu, image, stop_after_traces=None)
            if returncode != 0 or first_use_lines(output):
                raise AssertionError(f"{name}: unexpected first-use trap\n{output}")

        image = temp / "illegal-decode.o"
        assemble(
            args.llvm_mc,
            source,
            image,
            test_case=21,
            source_acr=2,
            retry_kind=-1,
            cross_kind=0,
        )
        _, output = run_qemu(args.qemu, image, stop_after_traces=1)
        traces = [line for line in output.splitlines() if line.startswith(TRACE_PREFIX)]
        if not traces:
            raise AssertionError(f"illegal decode did not produce an architectural trap\n{output}")
        fields = trace_fields(traces[0])
        if fields.get("cause") == "0x4" or fields.get("argv") != "0":
            raise AssertionError(f"first-use took priority over illegal decode\n{traces[0]}")

    print("PASS: executable VECTOR/CUBE first-use exception contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
