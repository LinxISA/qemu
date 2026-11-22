#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLVM_MC="${LLVM_MC:?set LLVM_MC to the scoped Linx llvm-mc}"
QEMU_BIN="${QEMU_BIN:-$ROOT/build-scoped-amo/qemu-system-linx64}"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linx-scoped-atomic.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

"$LLVM_MC" -triple=linx64 -mattr=+pto-v0591-scoped -filetype=obj \
  "$ROOT/tests/linxisa/scoped_atomic_core4_contract.s" \
  -o "$tmp/scoped-atomic-core4.o"
"$LLVM_MC" -triple=linx64 -mattr=+pto-v0591-scoped -filetype=obj \
  "$ROOT/tests/linxisa/scoped_atomic_discard_contract.s" \
  -o "$tmp/scoped-atomic-discard.o"
for kind in rmw cas; do
  "$LLVM_MC" -triple=linx64 -mattr=+pto-v0591-scoped -filetype=obj \
    "$ROOT/tests/linxisa/scoped_atomic_mmio_${kind}.s" \
    -o "$tmp/scoped-atomic-mmio-${kind}.o"
done

for thread in single multi; do
  LINX_QEMU_SCOPED_SCALAR_RUNTIME=1 LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" \
    -nographic -monitor none -accel "tcg,thread=$thread" -machine virt -smp 4 \
    -kernel "$tmp/scoped-atomic-core4.o" -bios none
done

QEMU_BIN="$QEMU_BIN" TMP="$tmp" python3 - <<'PY'
import os
from pathlib import Path
import struct
import subprocess

qemu = os.environ["QEMU_BIN"]
tmp = Path(os.environ["TMP"])
result = tmp / "discard.bin"
expected = struct.pack("<6Q", 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666)
machine = (f"virt,cross-model-dump={result},cross-model-address=131072,"
           f"cross-model-size={len(expected)}")
subprocess.run(
    [qemu, "-nographic", "-monitor", "none", "-machine", machine,
     "-smp", "1", "-bios", "none", "-kernel",
     str(tmp / "scoped-atomic-discard.o")],
    env=os.environ | {"LINX_QEMU_SCOPED_SCALAR_RUNTIME": "1",
                      "LINX_VIRT_TEST_FINISHER": "1"},
    check=True, timeout=10,
)
if result.read_bytes() != expected:
    raise SystemExit("scoped atomic destinations 24..29 changed queue state")

for kind in ("rmw", "cas"):
    trace = tmp / f"mmio-{kind}.trace"
    command = [
        qemu, "-nographic", "-monitor", "none", "-machine", "virt",
        "-smp", "1", "-bios", "none", "-kernel",
        str(tmp / f"scoped-atomic-mmio-{kind}.o"),
        "-trace", "enable=linx_deliver_sync_trap", "-D", str(trace),
    ]
    try:
        subprocess.run(
            command,
            env=os.environ | {"LINX_QEMU_SCOPED_SCALAR_RUNTIME": "1",
                              "LINX_VIRT_TEST_FINISHER": "1"},
            check=False, timeout=0.3, capture_output=True, text=True,
        )
    except subprocess.TimeoutExpired:
        pass
    else:
        raise SystemExit(f"scoped MMIO {kind} reached a device callback")
    if "linx_deliver_sync_trap" not in trace.read_text(encoding="utf-8"):
        raise SystemExit(f"scoped MMIO {kind} did not raise a precise trap")

print("ok: scoped atomics reject MMIO without callbacks and discard RegDst 24..29")
PY
