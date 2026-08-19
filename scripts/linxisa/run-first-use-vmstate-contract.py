#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Round-trip Linx ECONFIG banks through the production CPU VMState."""

from __future__ import annotations

import argparse
import json
import os
import selectors
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any


def connect_qmp(path: Path, deadline: float) -> tuple[socket.socket, Any]:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            client.connect(str(path))
            break
        except FileNotFoundError:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"QMP socket did not appear: {path}")
            time.sleep(0.02)
    stream = client.makefile("rwb", buffering=0)
    greeting = json.loads(stream.readline())
    if "QMP" not in greeting:
        raise AssertionError(f"invalid QMP greeting: {greeting}")
    qmp(stream, "qmp_capabilities")
    return client, stream


def qmp(stream: Any, command: str, arguments: dict[str, Any] | None = None) -> Any:
    request: dict[str, Any] = {"execute": command}
    if arguments:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\r\n")
    while True:
        response = json.loads(stream.readline())
        if "event" in response:
            continue
        if "error" in response:
            raise AssertionError(f"QMP {command} failed: {response['error']}")
        return response.get("return")


def wait_ready(process: subprocess.Popen[bytes], deadline: float) -> bytes:
    assert process.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    while time.monotonic() < deadline:
        for key, _ in selector.select(timeout=0.05):
            chunk = os.read(key.fileobj.fileno(), 4096)
            output.extend(chunk)
            if b"R" in output:
                return bytes(output)
        if process.poll() is not None:
            break
    raise AssertionError(f"source guest did not publish migration marker: {output!r}")


def wait_migration(stream: Any, deadline: float) -> None:
    while time.monotonic() < deadline:
        status = qmp(stream, "query-migrate")
        if status.get("status") == "completed":
            return
        if status.get("status") in {"failed", "cancelled"}:
            raise AssertionError(f"migration failed: {status}")
        time.sleep(0.05)
    raise TimeoutError("migration did not complete")


def qemu_command(qemu: Path, image: Path, qmp_path: Path) -> list[str]:
    return [
        str(qemu),
        "-nographic",
        "-monitor",
        "none",
        "-qmp",
        f"unix:{qmp_path},server=on,wait=off",
        "-machine",
        "virt",
        "-kernel",
        str(image),
        "-bios",
        "none",
    ]


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

    env = os.environ.copy()
    env["LINX_VIRT_TEST_FINISHER"] = "1"
    with tempfile.TemporaryDirectory(prefix="linx-first-use-vmstate-") as directory:
        temp = Path(directory)
        image = temp / "first-use-vmstate.o"
        state = temp / "migration.state"
        source_qmp = temp / "source.qmp"
        destination_qmp = temp / "destination.qmp"
        subprocess.run(
            [
                str(args.llvm_mc),
                "-triple=linx64",
                "-filetype=obj",
                str(root / "tests/linxisa/first_use_vmstate_contract.s"),
                "-o",
                str(image),
            ],
            check=True,
        )

        source = subprocess.Popen(
            qemu_command(args.qemu, image, source_qmp),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        source_client = None
        source_stream = None
        try:
            deadline = time.monotonic() + 5.0
            source_client, source_stream = connect_qmp(source_qmp, deadline)
            wait_ready(source, deadline)
            qmp(source_stream, "stop")
            qmp(source_stream, "migrate", {"uri": f"file:{state}"})
            wait_migration(source_stream, time.monotonic() + 10.0)
        finally:
            if source_stream is not None:
                source_stream.close()
            if source_client is not None:
                source_client.close()
            source.terminate()
            try:
                source.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                source.kill()
                source.wait()

        if not state.is_file() or state.stat().st_size == 0:
            raise AssertionError("migration did not create a VMState stream")

        destination = subprocess.Popen(
            qemu_command(args.qemu, image, destination_qmp)
            + ["-incoming", f"file:{state}"],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        destination_client = None
        destination_stream = None
        try:
            destination_client, destination_stream = connect_qmp(
                destination_qmp, time.monotonic() + 5.0
            )
            qmp(destination_stream, "cont")
            output, _ = destination.communicate(timeout=20.0)
        except subprocess.TimeoutExpired:
            destination.kill()
            output, _ = destination.communicate()
            raise AssertionError(f"restored guest timed out: {output!r}")
        finally:
            if destination_stream is not None:
                destination_stream.close()
            if destination_client is not None:
                destination_client.close()
        if destination.returncode != 0:
            raise AssertionError(
                f"restored ECONFIG banks/mask mismatch: rc={destination.returncode} {output!r}"
            )

    print("PASS: ECONFIG bank and reserved-mask VMState round trip")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
