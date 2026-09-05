#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_BUILD="${LLVM_BUILD:-$LINX_ROOT/compiler/llvm/build-linxisa-clang}"
LLVM_MC="${LLVM_MC:-$LLVM_BUILD/bin/llvm-mc}"
QEMU_BIN="${QEMU_BIN:-$ROOT/build-linx/qemu-system-linx64}"

if [[ ! -x "$LLVM_MC" ]]; then
  echo "error: llvm-mc not found/executable: $LLVM_MC" >&2
  exit 1
fi
if [[ ! -x "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found/executable: $QEMU_BIN" >&2
  exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-addtpc-asl.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
OBJ="$TMP/addtpc_asl_smoke.o"
TRACE="$TMP/addtpc_asl_smoke.jsonl"

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/addtpc_asl_smoke.s" -o "$OBJ"

LINX_VIRT_TEST_FINISHER=1 LINX_COMMIT_TRACE="$TRACE" \
  "$QEMU_BIN" -nographic -monitor none -machine virt -bios none -kernel "$OBJ"

TRACE="$TRACE" python3 - <<'PY'
import json
import os
import sys

with open(os.environ["TRACE"], "r", encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream if line.strip()]

writebacks = [row for row in rows if row.get("wb_valid")]
displacements = (0x2000, -0x2000, 0x2000, -0x2000)
if len(writebacks) < len(displacements):
    print("error: missing ADDTPC/HL.ADDTPC writeback(s)", file=sys.stderr)
    print(json.dumps(writebacks, indent=2), file=sys.stderr)
    sys.exit(1)

failures = []
for row, displacement in zip(writebacks[:4], displacements):
    expected = (row["pc"] + displacement) & ((1 << 64) - 1)
    if row.get("wb_data") != expected:
        failures.append({
            "pc": row.get("pc"),
            "expected": expected,
            "actual": row.get("wb_data"),
        })
if failures:
    print("error: ADDTPC/HL.ADDTPC is not TPC-relative", file=sys.stderr)
    print(json.dumps(failures, indent=2), file=sys.stderr)
    print(json.dumps(writebacks[:4], indent=2), file=sys.stderr)
    sys.exit(1)
print("ok: ADDTPC and HL.ADDTPC use each instruction TPC for page displacements")
PY

RELOC_OBJ="$TMP/addtpc_pcrel_reloc_smoke.o"
RELOC_TRACE="$TMP/addtpc_pcrel_reloc_smoke.jsonl"

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/addtpc_pcrel_reloc_smoke.s" -o "$RELOC_OBJ"

LINX_VIRT_TEST_FINISHER=1 LINX_COMMIT_TRACE="$RELOC_TRACE" \
  "$QEMU_BIN" -nographic -monitor none -machine virt -bios none -kernel "$RELOC_OBJ"

RELOC_TRACE="$RELOC_TRACE" python3 - <<'PY'
import json
import os
import sys

with open(os.environ["RELOC_TRACE"], "r", encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream if line.strip()]

stores = [row for row in rows if row.get("mem_valid") and row.get("mem_is_store")]
finisher = [row for row in stores if row.get("mem_addr") == 0x10009000]
if len(finisher) != 1 or finisher[0].get("mem_wdata") != 0x5555:
    print("error: ADDTPC PCREL relocation did not resolve the data target", file=sys.stderr)
    print(json.dumps(finisher, indent=2), file=sys.stderr)
    sys.exit(1)
print("ok: QEMU ET_REL ADDTPC HI20/LO12 relocation is TPC-relative")
PY
