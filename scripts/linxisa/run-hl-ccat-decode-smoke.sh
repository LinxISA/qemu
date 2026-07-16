#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"

LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
LLVM_MC="${LLVM_MC:-$LLVM_BUILD/bin/llvm-mc}"
QEMU_BUILD="${QEMU_BUILD:-$ROOT/build-linx}"
QEMU_BIN="${QEMU_BIN:-}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-10}"

if [[ -z "$QEMU_BIN" ]]; then
  for cand in \
    "$QEMU_BUILD/qemu-system-linx64" \
    "$ROOT/build-linx/qemu-system-linx64" \
    "$ROOT/build/qemu-system-linx64" \
    "$ROOT/build-tci/qemu-system-linx64"; do
    if [[ -x "$cand" ]]; then
      QEMU_BIN="$cand"
      break
    fi
  done
fi

if [[ ! -x "$LLVM_MC" ]]; then
  echo "error: llvm-mc not found/executable: $LLVM_MC" >&2
  exit 1
fi
if [[ -z "$QEMU_BIN" || ! -x "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found; set QEMU_BIN or QEMU_BUILD" >&2
  exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-hl-ccat-decode.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

SRC="$ROOT/tests/linxisa/hl_ccat_decode_smoke.s"
OUT_O="$TMP/hl_ccat_decode_smoke.o"

python3 - <<'PY'
legacy_mask = 0xffff0600707f07ff
golden_mask = 0xffff0000707f07ff
for word, match in (
    (0x8231125d280e, 0x105d000e),  # HL.CCAT shamt=65
    (0x7e31225d280e, 0x205d000e),  # HL.CCATW shamt=63
):
    assert word & golden_mask == match
    assert word & legacy_mask != match
print("[decode] high shamt raw forms require the golden 7-bit signature")
PY

echo "[llvm-mc] -triple=linx64 $SRC"
"$LLVM_MC" -triple=linx64 -filetype=obj "$SRC" -o "$OUT_O"

QEMU_ARGS=(-nographic -monitor none -machine virt -kernel "$OUT_O")
if [[ "$(basename -- "$QEMU_BIN")" != *bios-none ]]; then
  QEMU_ARGS+=(-bios none)
fi

echo "[run] $QEMU_BIN ${QEMU_ARGS[*]}"
LINX_VIRT_TEST_FINISHER=1 python3 - "$TIMEOUT_SECONDS" \
  "$QEMU_BIN" "${QEMU_ARGS[@]}" <<'PY'
import os
import subprocess
import sys

timeout = float(sys.argv[1])
try:
    result = subprocess.run(sys.argv[2:], env=os.environ.copy(), timeout=timeout)
except subprocess.TimeoutExpired:
    print(f"error: QEMU timed out after {timeout:g}s", file=sys.stderr)
    raise SystemExit(124)
if result.returncode != 0:
    print(f"error: QEMU exited with status {result.returncode}", file=sys.stderr)
    raise SystemExit(result.returncode or 1)
PY

echo "[PASS] hl-ccat-decode-smoke"
