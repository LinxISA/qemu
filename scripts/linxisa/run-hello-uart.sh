#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"

LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
CLANG="${CLANG:-$LLVM_BUILD/bin/clang}"

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

qemu_direct_kernel_args() {
  local kernel="$1"
  QEMU_DIRECT_KERNEL_ARGS=(-nographic -monitor none -machine virt -kernel "$kernel")
  if [[ "$(basename -- "$QEMU_BIN")" != *bios-none ]]; then
    QEMU_DIRECT_KERNEL_ARGS+=(-bios none)
  fi
}

if [[ ! -x "$CLANG" ]]; then
  echo "error: clang not found/executable: $CLANG" >&2
  echo "       set CLANG=... or LLVM_BUILD=... (see docs/linxisa/README.md)" >&2
  exit 1
fi
if [[ -z "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found." >&2
  echo "       set QEMU_BIN=... or QEMU_BUILD=..., or build QEMU (see docs/linxisa/README.md)" >&2
  exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-hello.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

SRC="$ROOT/tests/linxisa/hello_uart.c"
OUT_O="$TMP/hello_uart.o"

COMMON_FLAGS=(
  -O2
  -ffreestanding
  -fno-builtin
  -fno-stack-protector
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-exceptions
)

echo "[cc] linx64-unknown-elf $SRC"
"$CLANG" -target linx64-unknown-elf "${COMMON_FLAGS[@]}" -c "$SRC" -o "$OUT_O"

qemu_direct_kernel_args "$OUT_O"
echo "[run] $QEMU_BIN ${QEMU_DIRECT_KERNEL_ARGS[*]}"
"$QEMU_BIN" "${QEMU_DIRECT_KERNEL_ARGS[@]}"
