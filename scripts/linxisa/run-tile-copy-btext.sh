#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

LLVM_BUILD="${LLVM_BUILD:-$HOME/llvm-project/build-linxisa-clang}"
LLC="${LLC:-$LLVM_BUILD/bin/llc}"

QEMU_BUILD="${QEMU_BUILD:-$ROOT/build}"
QEMU_BIN="${QEMU_BIN:-}"

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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-tile-copy.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

OUT_O="$TMP/tile_copy_btext.o"

SRC="$ROOT/tests/linxisa/tile_copy_btext.ll"

echo "[llc] -mtriple=linx64 $SRC"
"$LLC" -mtriple=linx64 -O2 -filetype=obj "$SRC" -o "$OUT_O"

echo "[run] $QEMU_BIN -kernel $OUT_O"
"$QEMU_BIN" -nographic -monitor none -machine virt -kernel "$OUT_O"
