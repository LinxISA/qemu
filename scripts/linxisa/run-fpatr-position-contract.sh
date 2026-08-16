#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_MC="${LLVM_MC:-$LINX_ROOT/compiler/llvm/build-linxisa-clang/bin/llvm-mc}"
QEMU_BIN="${QEMU_BIN:-${QEMU_BUILD:-$ROOT/build-linx}/qemu-system-linx64}"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-fpatr-position.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

for case_id in 0 1 2 3 4; do
  elf="$tmp/fpatr-position-$case_id.o"
  log="$tmp/fpatr-position-$case_id.log"
  "$LLVM_MC" -triple=linx64 -filetype=obj --defsym="CASE=$case_id" \
    "$ROOT/tests/linxisa/fpatr_position_contract.s" -o "$elf"
  python3 - "$QEMU_BIN" "$elf" "$log" <<'PY'
import os
import selectors
import subprocess
import sys
import time

process = subprocess.Popen(
    [sys.argv[1], "-nographic", "-monitor", "none", "-machine", "virt",
     "-kernel", sys.argv[2], "-bios", "none", "-d", "guest_errors"],
    env={**os.environ, "LINX_VIRT_TEST_FINISHER": "1"},
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
)
selector = selectors.DefaultSelector()
selector.register(process.stdout, selectors.EVENT_READ)
output = bytearray()
deadline = time.monotonic() + 3
found = False
while time.monotonic() < deadline and process.poll() is None:
    for key, _ in selector.select(timeout=0.1):
        output.extend(os.read(key.fileobj.fileno(), 4096))
        if b"illegal B.FPATR position" in output:
            found = True
            process.terminate()
            break
    if found:
        break
try:
    process.wait(timeout=1)
except subprocess.TimeoutExpired:
    process.kill()
    process.wait()
open(sys.argv[3], "wb").write(output)
raise SystemExit(0 if found else 1)
PY
done
