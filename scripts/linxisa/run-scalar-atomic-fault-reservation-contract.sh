#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
LLVM_MC="${LLVM_MC:-$LLVM_BUILD/bin/llvm-mc}"
QEMU_BIN="${QEMU_BIN:-${QEMU_BUILD:-$ROOT/build-linx}/qemu-system-linx64}"

if [[ ! -x "$LLVM_MC" ]]; then
  echo "error: llvm-mc not found/executable: $LLVM_MC" >&2
  exit 1
fi
if [[ ! -x "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found/executable: $QEMU_BIN" >&2
  exit 1
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-atomic-fault.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

"$LLVM_MC" -triple=linx64 -filetype=obj --defsym=FAULT_KIND=2 \
  "$ROOT/tests/linxisa/mmu_ttbr_basic.s" \
  -o "$tmp/scalar_atomic_fault_reservation_contract.o"

LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" \
  -nographic -monitor none -machine virt \
  -kernel "$tmp/scalar_atomic_fault_reservation_contract.o" -bios none
