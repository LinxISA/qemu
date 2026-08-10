#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"

LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
LLC="${LLC:-$LLVM_BUILD/bin/llc}"

QEMU_BUILD="${QEMU_BUILD:-$ROOT/build}"
QEMU_BIN="${QEMU_BIN:-}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-15}"

if [[ -z "$QEMU_BIN" ]]; then
  for cand in \
    "$QEMU_BUILD/qemu-system-linx64" \
    "$ROOT/build/qemu-system-linx64" \
    "$ROOT/build-tci/qemu-system-linx64" \
    "$ROOT/build-linx/qemu-system-linx64" \
    "$QEMU_BUILD/qemu-system-linx64-unsigned" \
    "$ROOT/build/qemu-system-linx64-unsigned" \
    "$ROOT/build-tci/qemu-system-linx64-unsigned" \
    "$ROOT/build-linx/qemu-system-linx64-unsigned"; do
    if [[ -x "$cand" ]]; then
      QEMU_BIN="$cand"
      break
    fi
  done
fi

qemu_direct_kernel_args() {
  local kernel="$1"
  QEMU_DIRECT_KERNEL_ARGS=(-nographic -monitor none -machine virt -kernel "$kernel")
  if [[ "$(basename -- "$QEMU_BIN")" != *bios-none ]]; then
    QEMU_DIRECT_KERNEL_ARGS+=(-bios none)
  fi
}

if [[ ! -x "$LLC" ]]; then
  echo "error: llc not found/executable: $LLC" >&2
  echo "       set LLC=... or LLVM_BUILD=... (see docs/linxisa/README.md)" >&2
  exit 1
fi
if [[ -z "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found." >&2
  echo "       set QEMU_BIN=... or QEMU_BUILD=..., or build QEMU (see docs/linxisa/README.md)" >&2
  exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-iommu-tile.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

OUT_O="$TMP/iommu_tile_basic.o"
SRC="$ROOT/tests/linxisa/iommu_tile_basic.ll"

echo "[llc] -mtriple=linx64 $SRC"
"$LLC" -mtriple=linx64 -O2 -filetype=obj "$SRC" -o "$OUT_O"

qemu_direct_kernel_args "$OUT_O"
echo "[run] $QEMU_BIN ${QEMU_DIRECT_KERNEL_ARGS[*]}"
LINX_VIRT_TEST_FINISHER=1 python3 - "$TIMEOUT_SECONDS" \
  "$QEMU_BIN" "${QEMU_DIRECT_KERNEL_ARGS[@]}" <<'PY'
import os
import subprocess
import sys

timeout = float(sys.argv[1])
cmd = sys.argv[2:]
try:
    result = subprocess.run(cmd, env=os.environ.copy(), timeout=timeout)
except subprocess.TimeoutExpired:
    print(f"error: QEMU timed out after {timeout:g}s", file=sys.stderr)
    raise SystemExit(124)
if result.returncode != 0:
    print(f"error: QEMU exited with status {result.returncode}", file=sys.stderr)
    raise SystemExit(result.returncode or 1)
PY
echo "[PASS] iommu-tile-basic (PTO 0.58 TLSU descriptor path)"
