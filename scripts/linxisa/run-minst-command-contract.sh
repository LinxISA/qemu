#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "${ROOT}/../.." && pwd)"
LLVM_MC="${LLVM_MC:-$LINX_ROOT/compiler/llvm/build-linxisa-clang/bin/llvm-mc}"
QEMU_BIN="${QEMU_BIN:-${QEMU_BUILD:-$ROOT/build-linx}/qemu-system-linx64}"

for tool in "$LLVM_MC" "$QEMU_BIN"; do
  if [[ ! -x "$tool" ]]; then
    echo "error: required executable not found: $tool" >&2
    exit 1
  fi
done

tmp="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-minst-command.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/tile_descriptor_smoke.s" -o "$tmp/tile.o"
"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/mcopy_mset_basic.s" -o "$tmp/macro.o"

LINX_MINST_TRACE="$tmp/tile.jsonl" LINX_VIRT_TEST_FINISHER=1 \
  "$QEMU_BIN" -nographic -monitor none -machine virt \
  -kernel "$tmp/tile.o" -bios none
LINX_MINST_TRACE="$tmp/macro.jsonl" LINX_VIRT_TEST_FINISHER=1 \
  "$QEMU_BIN" -nographic -monitor none -machine virt \
  -kernel "$tmp/macro.o" -bios none

python3 - "$tmp/tile.jsonl" "$tmp/macro.jsonl" <<'PY'
import json
import sys


def rows(path):
    with open(path, "r", encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


b_text = [row for row in rows(sys.argv[1]) if row.get("mnemonic") == "B.TEXT"]
errors = []
if len(b_text) != 1 or b_text[0].get("form_id") != "6f2fe6f95841":
    errors.append(
        f"expected one B.TEXT form 6f2fe6f95841, got {b_text}"
    )

macros = [
    row for row in rows(sys.argv[2])
    if row.get("mnemonic") in {"MCOPY", "MSET"}
]
expected = [
    ("MSET", 2, 5),
    ("MSET", 3, 0),
    ("MCOPY", 3, 2),
]
actual = [
    (
        row.get("mnemonic"),
        row.get("src0_value") if row.get("src0_valid") == 1 else None,
        row.get("src1_value") if row.get("src1_valid") == 1 else None,
    )
    for row in macros
]
if actual != expected:
    errors.append(f"expected macro operands {expected}, got {actual}")
if errors:
    raise SystemExit("error: " + "; ".join(errors))
print("ok: B.TEXT identity and MCOPY/MSET Minst operands")
PY
