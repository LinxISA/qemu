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

for fault_kind in 2 3 4 5 6 7 8; do
  echo "[atomic-fault] kind=$fault_kind"
  object="$tmp/scalar_atomic_fault_reservation_contract_$fault_kind.o"
  "$LLVM_MC" -triple=linx64 -filetype=obj \
    --defsym=FAULT_KIND="$fault_kind" \
    "$ROOT/tests/linxisa/mmu_ttbr_basic.s" -o "$object"

  LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" \
    -nographic -monitor none -machine virt \
    -kernel "$object" -bios none
  echo "[PASS] atomic-fault kind=$fault_kind"
done
