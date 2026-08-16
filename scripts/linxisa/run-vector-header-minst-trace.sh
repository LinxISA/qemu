#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
LLVM_MC="${LLVM_MC:-$LLVM_BUILD/bin/llvm-mc}"
LD_LLD="${LD_LLD:-$LLVM_BUILD/bin/ld.lld}"
QEMU_BIN="${QEMU_BIN:-${QEMU_BUILD:-$ROOT/build-linx}/qemu-system-linx64}"

for tool in "$LLVM_MC" "$LD_LLD" "$QEMU_BIN"; do
  if [[ ! -x "$tool" ]]; then
    echo "error: required executable not found: $tool" >&2
    exit 1
  fi
done

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-vector-header-trace.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

obj="$tmp/vector_header_smoke.o"
elf="$tmp/vector_header_smoke.elf"
trace="$tmp/vector_header_smoke.jsonl"

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/vector_header_smoke.s" -o "$obj"
"$LD_LLD" -e _start "$obj" -o "$elf"

python3 - "$elf" <<'PY'
import struct
import sys

with open(sys.argv[1], "rb") as stream:
    ident = stream.read(16)
    if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1:
        raise SystemExit("error: expected ELF64 little-endian executable")
    elf_type = struct.unpack("<H", stream.read(2))[0]
if elf_type != 2:
    raise SystemExit(f"error: expected ET_EXEC (2), got {elf_type}")
PY

LINX_MINST_TRACE="$trace" LINX_VIRT_TEST_FINISHER=1 \
  "$QEMU_BIN" -nographic -monitor none -machine virt -kernel "$elf" -bios none

python3 - "$trace" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream if line.strip()]

stops = [row for row in rows if row.get("mnemonic") == "C.BSTOP"]
expected = [("vpar", 0), ("vseq", 0)]
actual = [(row.get("block_kind"), row.get("lane_id")) for row in stops[:2]]
if actual != expected:
    raise SystemExit(
        f"error: first two C.BSTOP trace rows must be {expected}, got {actual}"
    )
print("ok: C.BSTOP trace rows preserve vpar/vseq lane 0")
PY
