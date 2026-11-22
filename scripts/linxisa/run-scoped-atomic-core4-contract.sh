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

for thread in single multi; do
  LINX_QEMU_SCOPED_SCALAR_RUNTIME=1 LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" \
    -nographic -monitor none -accel "tcg,thread=$thread" -machine virt -smp 4 \
    -kernel "$tmp/scoped-atomic-core4.o" -bios none
done
