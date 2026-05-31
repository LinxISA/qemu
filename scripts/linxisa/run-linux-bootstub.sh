#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"

LINUX_DIR="${LINUX_DIR:-$LINX_ROOT/kernel/linux}"
BOOTSTUB_DIR="$LINUX_DIR/tools/linxisa/bootstub"

LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"

QEMU_BUILD="${QEMU_BUILD:-$ROOT/build}"
QEMU_BIN="${QEMU_BIN:-}"

find_qemu() {
  local name="$1"
  local cand
  for cand in \
    "$QEMU_BUILD/$name" \
    "$QEMU_BUILD/${name}-unsigned" \
    "$ROOT/build/$name" \
    "$ROOT/build/${name}-unsigned" \
    "$ROOT/build-tci/$name" \
    "$ROOT/build-tci/${name}-unsigned" \
    "$ROOT/build-linx/$name" \
    "$ROOT/build-linx/${name}-unsigned"; do
    if [[ -x "$cand" ]]; then
      echo "$cand"
      return 0
    fi
  done
  return 1
}

if [[ -z "$QEMU_BIN" ]]; then
  QEMU_BIN="$(find_qemu qemu-system-linx64 || true)"
fi

qemu_direct_kernel_args() {
  local kernel="$1"
  shift || true
  QEMU_DIRECT_KERNEL_ARGS=(-nographic -monitor none -machine virt -kernel "$kernel" "$@")
  if [[ "$(basename -- "$QEMU_BIN")" != *bios-none ]]; then
    QEMU_DIRECT_KERNEL_ARGS+=(-bios none)
  fi
}

if [[ ! -d "$BOOTSTUB_DIR" ]]; then
  echo "error: bootstub dir not found: $BOOTSTUB_DIR" >&2
  echo "       set LINUX_DIR=... or run scripts/linxisa/setup-linux.sh first." >&2
  exit 1
fi

if [[ -z "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found." >&2
  echo "       set QEMU_BIN=... or QEMU_BUILD=..., or build QEMU." >&2
  exit 1
fi

echo "[make] $BOOTSTUB_DIR"
make -C "$BOOTSTUB_DIR" LLVM_BUILD="$LLVM_BUILD" >/dev/null

VMLINUX="${VMLINUX:-$BOOTSTUB_DIR/build/bootstub.o}"
if [[ ! -f "$VMLINUX" ]]; then
  echo "error: bootstub image not found: $VMLINUX" >&2
  exit 1
fi

qemu_direct_kernel_args "$VMLINUX" "$@"
echo "[run] $QEMU_BIN ${QEMU_DIRECT_KERNEL_ARGS[*]}"
"$QEMU_BIN" "${QEMU_DIRECT_KERNEL_ARGS[@]}"
