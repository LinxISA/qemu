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
block_tpc = min(row["pc"] for row in writebacks[:4]) - 2
expected = [
    (block_tpc + 0x2000, "positive"),
    (block_tpc - 0x2000, "negative"),
]
matched = {name: 0 for _, name in expected}
for row in writebacks:
    value = row.get("wb_data")
    if not isinstance(value, int):
        continue
    for expected_value, name in expected:
        if value == expected_value & ((1 << 64) - 1):
            matched[name] += 1

missing = [name for _, name in expected if matched[name] != 2]
if missing:
    print(f"error: missing ADDTPC ASL writeback(s): {', '.join(missing)}", file=sys.stderr)
    print(json.dumps(writebacks, indent=2), file=sys.stderr)
    sys.exit(1)
print("ok: ADDTPC and HL.ADDTPC PTO #77 positive and negative page displacements")
PY
