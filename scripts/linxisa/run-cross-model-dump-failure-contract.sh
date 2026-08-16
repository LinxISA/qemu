#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_MC="${LLVM_MC:-$LINX_ROOT/compiler/llvm/build-linxisa-clang/bin/llvm-mc}"
QEMU_BIN="${QEMU_BIN:-${QEMU_BUILD:-$ROOT/build-linx}/qemu-system-linx64}"

for tool in "$LLVM_MC" "$QEMU_BIN"; do
  if [[ ! -x "$tool" ]]; then
    echo "error: required executable not found: $tool" >&2
    exit 1
  fi
done

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-dump-failure.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
obj="$tmp/commit_trace_smoke.o"
"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/commit_trace_smoke.s" -o "$obj"

python3 - "$QEMU_BIN" "$obj" "$tmp" <<'PY'
import os
from pathlib import Path
import subprocess
import sys
import time

qemu, obj, tmp_arg = sys.argv[1:]
tmp = Path(tmp_arg)
result_diag = b"cannot write cross-model dump"
tile_diag = b"cannot write tile-state dump"


def dump_path(case_dir, name, shape):
    if shape is None:
        return None
    if shape == "missing":
        return case_dir / "missing" / name
    path = case_dir / name
    if shape == "stale":
        path.mkdir()
    return path


def command(result_path, tile_path, pause=False):
    machine = ["virt"]
    if result_path is not None:
        machine += [f"cross-model-dump={result_path}", "cross-model-size=8"]
    if tile_path is not None:
        machine.append(f"cross-model-tile-dump={tile_path}")
    args = [
        qemu, "-nographic", "-monitor", "none",
        "-machine", ",".join(machine), "-kernel", obj, "-bios", "none",
    ]
    if pause:
        args += ["-action", "shutdown=pause"]
    return args


def run_case(name, result_shape, tile_shape, result_errors, tile_errors,
             expect_result=False, expect_tile=False):
    case_dir = tmp / name
    case_dir.mkdir()
    result_path = dump_path(case_dir, "result.bin", result_shape)
    tile_path = dump_path(case_dir, "tile.bin", tile_shape)
    try:
        completed = subprocess.run(
            command(result_path, tile_path),
            env={**os.environ, "LINX_VIRT_TEST_FINISHER": "1"},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=3,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"error: dump case {name} timed out") from exc
    if completed.returncode != 1:
        raise SystemExit(
            f"error: dump case {name} returned "
            f"{completed.returncode}, expected 1"
        )
    if completed.stdout.count(result_diag) != result_errors:
        raise SystemExit(f"error: dump case {name} result diagnostic mismatch")
    if completed.stdout.count(tile_diag) != tile_errors:
        raise SystemExit(f"error: dump case {name} tile diagnostic mismatch")
    if expect_result and (
        not result_path.is_file() or result_path.stat().st_size != 8
    ):
        raise SystemExit(
            f"error: dump case {name} did not preserve result output"
        )
    if expect_tile and (
        not tile_path.is_file() or tile_path.stat().st_size == 0
    ):
        raise SystemExit(
            f"error: dump case {name} did not preserve tile output"
        )


run_case("result-stale", "stale", None, 1, 0)
run_case("result-missing", "missing", None, 1, 0)
run_case("tile-stale", None, "stale", 0, 1)
run_case("tile-missing", None, "missing", 0, 1)
run_case("combined-result-fail", "stale", "good", 1, 0,
         expect_tile=True)
run_case("combined-tile-fail", "good", "stale", 0, 1,
         expect_result=True)
run_case("combined-both-fail", "stale", "stale", 1, 1)

pause_dir = tmp / "pause-no-loop"
pause_dir.mkdir()
pause_result = dump_path(pause_dir, "result.bin", "stale")
pause_log = pause_dir / "qemu.log"
with pause_log.open("wb") as stream:
    process = subprocess.Popen(
        command(pause_result, None, pause=True),
        env={**os.environ, "LINX_VIRT_TEST_FINISHER": "1"},
        stdout=stream,
        stderr=subprocess.STDOUT,
    )
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise SystemExit(
                "error: shutdown=pause process exited unexpectedly"
            )
        if result_diag in pause_log.read_bytes():
            break
        time.sleep(0.01)
    else:
        process.terminate()
        process.wait(timeout=1)
        raise SystemExit("error: shutdown=pause lacked dump diagnostic")
    time.sleep(0.1)
    process.terminate()
    process.wait(timeout=1)
pause_output = pause_log.read_bytes()[:1048576]
if pause_output.count(result_diag) != 1:
    raise SystemExit("error: shutdown=pause repeated dump notification")

print("ok: bounded result/tile dump failures exit once after full cleanup")
PY
