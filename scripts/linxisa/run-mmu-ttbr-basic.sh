#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"

LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
LLVM_MC="${LLVM_MC:-$LLVM_BUILD/bin/llvm-mc}"
LLVM_READOBJ="${LLVM_READOBJ:-$LLVM_BUILD/bin/llvm-readobj}"

QEMU_BUILD="${QEMU_BUILD:-$ROOT/build-linx}"
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

if [[ ! -x "$LLVM_MC" ]]; then
  echo "error: llvm-mc not found/executable: $LLVM_MC" >&2
  echo "       set LLVM_MC=... or LLVM_BUILD=... (see docs/linxisa/README.md)" >&2
  exit 1
fi
if [[ ! -x "$LLVM_READOBJ" ]]; then
  echo "error: llvm-readobj not found/executable: $LLVM_READOBJ" >&2
  echo "       set LLVM_READOBJ=... or LLVM_BUILD=..." >&2
  exit 1
fi
if [[ -z "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found." >&2
  echo "       set QEMU_BIN=... or QEMU_BUILD=..., or build QEMU (see docs/linxisa/README.md)" >&2
  exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-mmu-ttbr.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

OUT_O="$TMP/mmu_ttbr_basic.o"
OUT_BAD_RELATIVE="$TMP/mmu_ttbr_relative.o"
OUT_BAD_GOT_HI20="$TMP/mmu_ttbr_got_hi20.o"
OUT_BAD_GOT_LO12="$TMP/mmu_ttbr_got_lo12.o"
SRC="$ROOT/tests/linxisa/mmu_ttbr_basic.s"

echo "[llvm-mc] -triple=linx64 $SRC"
"$LLVM_MC" -triple=linx64 -filetype=obj "$SRC" -o "$OUT_O"

RELOCS="$($LLVM_READOBJ --relocations "$OUT_O")"
for reloc in R_LINX_PCREL_HI20 R_LINX_LO12 R_LINX_64; do
  if ! grep -q "$reloc" <<<"$RELOCS"; then
    echo "error: expected current-ABI relocation $reloc in $OUT_O" >&2
    exit 1
  fi
done
HI20_COUNT="$(grep -c 'R_LINX_PCREL_HI20' <<<"$RELOCS")"
LO12_COUNT="$(grep -c 'R_LINX_LO12' <<<"$RELOCS")"
R64_COUNT="$(grep -c 'R_LINX_64' <<<"$RELOCS")"
if [[ "$HI20_COUNT" -ne "$LO12_COUNT" ]]; then
  echo "error: unpaired current-ABI HI20/LO12 relocations" >&2
  exit 1
fi
echo "[reloc] current ABI: HI20=$HI20_COUNT LO12=$LO12_COUNT R64=$R64_COUNT"

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
echo "[PASS] mmu-ttbr-basic (current ET_REL relocation ABI)"

for fault_kind in 0 1; do
  if [[ "$fault_kind" -eq 0 ]]; then
    fault_name="TLOAD"
    fault_object="$TMP/mmu_ttbr_tload_fault.o"
  else
    fault_name="TSTORE"
    fault_object="$TMP/mmu_ttbr_tstore_fault.o"
  fi
  echo "[llvm-mc] precise ACR2 $fault_name page fault"
  "$LLVM_MC" -triple=linx64 -filetype=obj --defsym=FAULT_KIND="$fault_kind" \
    "$SRC" -o "$fault_object"
  qemu_direct_kernel_args "$fault_object"
  echo "[run] precise ACR2 $fault_name page fault"
  LINX_VIRT_TEST_FINISHER=1 python3 - "$TIMEOUT_SECONDS" \
    "$QEMU_BIN" "${QEMU_DIRECT_KERNEL_ARGS[@]}" <<'PY'
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
  echo "[PASS] precise ACR2 $fault_name VA/cause and commit boundary"
done

# Preserve matching instruction sites while changing their relocation types to
# unsupported dynamic/GOT types.  Opcode fallback must reject all three.
python3 - "$OUT_O" \
  "$OUT_BAD_RELATIVE" 17 9 \
  "$OUT_BAD_GOT_HI20" 15 23 \
  "$OUT_BAD_GOT_LO12" 17 24 <<'PY'
import shutil
import struct
import sys

src = sys.argv[1]
for dst, old_text, new_text in zip(sys.argv[2::3], sys.argv[3::3], sys.argv[4::3]):
    old_type, new_type = int(old_text), int(new_text)
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        ident = f.read(16)
        if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
            raise SystemExit("error: expected little-endian ELF64 test object")
        f.seek(0)
        eh = struct.unpack("<16sHHIQQQIHHHHHH", f.read(64))
        shoff, shentsize, shnum = eh[6], eh[11], eh[12]
        mutated = False
        for index in range(shnum):
            f.seek(shoff + index * shentsize)
            sh = struct.unpack("<IIQQQQIIQQ", f.read(64))
            if sh[1] != 4:  # SHT_RELA
                continue
            offset, size, entsize = sh[4], sh[5], sh[9]
            for rel_off in range(offset, offset + size, entsize):
                f.seek(rel_off)
                r_offset, r_info, r_addend = struct.unpack("<QQq", f.read(24))
                if (r_info & 0xffffffff) != old_type:
                    continue
                r_info = (r_info & ~0xffffffff) | new_type
                f.seek(rel_off)
                f.write(struct.pack("<QQq", r_offset, r_info, r_addend))
                mutated = True
                break
            if mutated:
                break
        if not mutated:
            raise SystemExit(f"error: no relocation type {old_type} to mutate")
PY

expect_unsupported_reloc() {
  local object="$1"
  local rtype="$2"
  local label="$3"

  qemu_direct_kernel_args "$object"
  echo "[run-negative] unsupported $label at matching opcode site"
  set +e
  BAD_OUTPUT="$(LINX_VIRT_TEST_FINISHER=1 python3 - "$TIMEOUT_SECONDS" \
    "$QEMU_BIN" "${QEMU_DIRECT_KERNEL_ARGS[@]}" 2>&1 <<'PY'
import os
import subprocess
import sys

timeout = float(sys.argv[1])
try:
    result = subprocess.run(sys.argv[2:], env=os.environ.copy(),
                            timeout=timeout, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
except subprocess.TimeoutExpired:
    print(f"error: QEMU timed out after {timeout:g}s")
    raise SystemExit(124)
print(result.stdout, end="")
raise SystemExit(result.returncode)
PY
)"
  BAD_STATUS=$?
  set -e
  if [[ "$BAD_STATUS" -eq 0 ]]; then
    echo "error: unsupported relocation unexpectedly loaded" >&2
    exit 1
  fi
  if ! grep -q "unsupported LinxISA ET_REL relocation type $rtype" <<<"$BAD_OUTPUT"; then
    echo "error: loader rejection did not identify relocation type $rtype" >&2
    echo "$BAD_OUTPUT" >&2
    exit 1
  fi
  echo "[PASS] $label rejected before opcode fallback"
}

expect_unsupported_reloc "$OUT_BAD_RELATIVE" 9 R_LINX_RELATIVE
expect_unsupported_reloc "$OUT_BAD_GOT_HI20" 23 R_LINX_GOT_HI20
expect_unsupported_reloc "$OUT_BAD_GOT_LO12" 24 R_LINX_GOT_LO12
