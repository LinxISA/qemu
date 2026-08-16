#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_MC="${LLVM_MC:-$LINX_ROOT/compiler/llvm/build-linxisa-clang/bin/llvm-mc}"
QEMU_BIN="${QEMU_BIN:-${QEMU_BUILD:-$ROOT/build-linx}/qemu-system-linx64}"

if [[ ! -x "$LLVM_MC" ]]; then
  echo "error: llvm-mc not found/executable: $LLVM_MC" >&2
  exit 1
fi
if [[ ! -x "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found/executable: $QEMU_BIN" >&2
  exit 1
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-fused-call.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/fused_call_contract.s" -o "$tmp/fused_call_contract.o"

LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" \
  -nographic -monitor none -machine virt \
  -kernel "$tmp/fused_call_contract.o" -bios none

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/fused_icall_invalid_contract.s" \
  -o "$tmp/fused_icall_invalid_contract.o"

set +e
python3 - "$QEMU_BIN" "$tmp/fused_icall_invalid_contract.o" \
  "$tmp/invalid.log" <<'PY'
import os
import selectors
import subprocess
import sys
import time

command = [
    sys.argv[1], "-nographic", "-monitor", "none", "-machine", "virt",
    "-kernel", sys.argv[2], "-bios", "none", "-d", "guest_errors",
]
env = os.environ.copy()
env["LINX_VIRT_TEST_FINISHER"] = "1"
process = subprocess.Popen(
    command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
)
selector = selectors.DefaultSelector()
selector.register(process.stdout, selectors.EVENT_READ)
output = bytearray()
deadline = time.monotonic() + 3
found = False
while time.monotonic() < deadline and process.poll() is None:
    for key, _ in selector.select(timeout=0.1):
        chunk = os.read(key.fileobj.fileno(), 4096)
        output.extend(chunk)
        if b"invalid branch target" in output:
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
code = 1 if found else (process.returncode or 124)
open(sys.argv[3], "wb").write(output)
raise SystemExit(code)
PY
invalid_rc=$?
set -e
if [[ "$invalid_rc" -eq 0 ]]; then
  echo "error: unexpectedly accepted invalid fused ICALL target" >&2
  exit 1
fi
if ! grep -q "invalid branch target" "$tmp/invalid.log"; then
  cat "$tmp/invalid.log" >&2
  echo "error: invalid fused ICALL did not fail target validation" >&2
  exit 1
fi
