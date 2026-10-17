#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
LLVM_MC="${LLVM_MC:-$LLVM_BUILD/bin/llvm-mc}"
QEMU_BIN="${QEMU_BIN:-${QEMU_BUILD:-$ROOT/build-linx}/qemu-system-linx64}"

if [[ ! -x "$LLVM_MC" || ! -x "$QEMU_BIN" ]]; then
  echo "error: set LLVM_MC/LLVM_BUILD and QEMU_BIN/QEMU_BUILD" >&2
  exit 1
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-atomic-cross-pe.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/scalar_atomic_cross_pe_contract.s" \
  -o "$tmp/scalar_atomic_cross_pe_contract.o"

LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" \
  -nographic -monitor none -machine virt -smp 4 \
  -kernel "$tmp/scalar_atomic_cross_pe_contract.o" -bios none
