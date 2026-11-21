#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLVM_MC="${LLVM_MC:?set LLVM_MC to the matching Linx llvm-mc}"
QEMU_BIN="${QEMU_BIN:-$ROOT/build/qemu-system-linx64}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/linx-hscb-geometry.XXXXXX")"
trap 'find "$TMP" -depth -delete' EXIT

READ_OBJ="$TMP/read.o"
"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/hscb_geometry_read_smoke.s" -o "$READ_OBJ"
LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" -nographic -monitor none \
  -machine virt -bios none -kernel "$READ_OBJ"

QEMU_BIN="$QEMU_BIN" LLVM_MC="$LLVM_MC" ROOT="$ROOT" TMP="$TMP" \
python3 - <<'PY'
import json
import os
import pathlib
import subprocess

root = pathlib.Path(os.environ["ROOT"])
tmp = pathlib.Path(os.environ["TMP"])
cases = (
    ("write", "hscb_geometry_write_fault.s", 0x10008),
    ("swap", "hscb_geometry_swap_fault.s", 0x10008),
    ("reserved", "hscb_geometry_alias_fault.s", 0x10002),
    ("long-alias", "hscb_geometry_long_alias_fault.s", 0x10002),
)
for name, source, fault_pc in cases:
    obj = tmp / f"{name}.o"
    commit = tmp / f"{name}.jsonl"
    log = tmp / f"{name}.log"
    source_path = root / "tests/linxisa" / source
    subprocess.run(
        [os.environ["LLVM_MC"], "-triple=linx64", "-filetype=obj",
         source_path, "-o", obj],
        check=True,
    )
    env = os.environ.copy()
    env["LINX_COMMIT_TRACE"] = str(commit)
    command = [
        os.environ["QEMU_BIN"], "-nographic", "-monitor", "none",
        "-machine", "virt", "-bios", "none", "-kernel", obj,
        "-trace", "enable=linx_deliver_sync_trap", "-D", str(log),
    ]
    try:
        subprocess.run(command, env=env, timeout=0.2, check=False)
    except subprocess.TimeoutExpired:
        pass
    output = log.read_text(encoding="utf-8")
    marker = ("linx_deliver_sync_trap trap=0 src_acr=0 dst_acr=0 "
              f"tpc=0x{fault_pc:x}")
    if marker not in output:
        raise SystemExit(f"{name}: missing precise illegal-instruction trap")
    with commit.open(encoding="utf-8") as stream:
        rows = [json.loads(line) for line in stream if line.strip()]
    traps = [row for row in rows if row.get("pc") == fault_pc]
    if traps:
        raise SystemExit(
            f"{name}: rejected SSR instruction retired: {traps[:2]}"
        )
print("ok: BLOCKNUM/BLOCKID ambient reads and rejected aliases are precise")
PY
