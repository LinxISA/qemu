#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLVM_MC="${LLVM_MC:?set LLVM_MC to the matching Linx llvm-mc}"
QEMU_BIN="${QEMU_BIN:-$ROOT/build/qemu-system-linx64}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/linx-scoped-runtime.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

OBJ="$TMP/scoped-scalar-runtime.o"
FAULT_OBJ="$TMP/scoped-scalar-runtime-fault.o"
TRACE="$TMP/scoped-scalar-runtime.trace"
COMMIT="$TMP/scoped-scalar-runtime.jsonl"

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/scoped_scalar_runtime_smoke.s" -o "$OBJ"
"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/scoped_scalar_runtime_fault.s" -o "$FAULT_OBJ"

LINX_QEMU_SCOPED_SCALAR_RUNTIME=1 \
LINX_VIRT_TEST_FINISHER=1 \
LINX_COMMIT_TRACE="$COMMIT" \
  "$QEMU_BIN" -nographic -monitor none -machine virt -smp 1 -bios none \
    -kernel "$OBJ" -trace "enable=linx_scoped_scalar_access" \
    -D "$TRACE"

TRACE="$TRACE" COMMIT="$COMMIT" python3 - <<'PY'
import json
import os
import re

with open(os.environ["TRACE"], encoding="utf-8") as stream:
    trace = stream.read()
rows = re.findall(
    r"linx_scoped_scalar_access PC=0x([0-9a-f]+) scope=(\d+) "
    r"addr=0x([0-9a-f]+) size=(\d+) store=(\d+)",
    trace,
)
expected = [
    (0, 0x20000, 8, 0),
    (1, 0x20008, 4, 1),
    (2, 0x20008, 4, 0),
]
if len(rows) != 4:
    raise SystemExit(f"expected four scoped accesses, got {rows}")
for actual, wanted in zip(rows[:3], expected, strict=True):
    _, scope, address, size, store = actual
    observed = (int(scope), int(address, 16), int(size), int(store))
    if observed != wanted:
        raise SystemExit(f"scoped access mismatch: {observed} != {wanted}")
_, scope, pcr_address, size, store = rows[3]
if (int(scope), int(size), int(store)) != (0, 4, 0):
    raise SystemExit(f"scoped PCR metadata mismatch: {rows[3]}")

with open(os.environ["COMMIT"], encoding="utf-8") as stream:
    commits = [json.loads(line) for line in stream if line.strip()]
scoped_pcs = [int(row[0], 16) for row in rows]
for pc in scoped_pcs:
    matching = [row for row in commits if row.get("pc") == pc]
    if len(matching) != 1:
        raise SystemExit(
            f"scoped carrier at 0x{pc:x} retired {len(matching)} times"
        )
next_pcs = {row.get("pc") for row in commits}
for pc in scoped_pcs:
    if pc + 8 not in next_pcs:
        raise SystemExit(f"carrier at 0x{pc:x} did not advance by eight bytes")
print("ok: scoped immediate/PCR accesses carried scope and retired eight bytes")
PY

QEMU_BIN="$QEMU_BIN" OBJ="$OBJ" TMP="$TMP" python3 - <<'PY'
import json
import os
import pathlib
import subprocess

qemu = os.environ["QEMU_BIN"]
obj = os.environ["OBJ"]
tmp = pathlib.Path(os.environ["TMP"])
for name, smp, enabled in (("disabled", "1", False), ("smp4", "4", True)):
    log = tmp / f"{name}.log"
    commit = tmp / f"{name}.jsonl"
    env = os.environ.copy()
    env["LINX_COMMIT_TRACE"] = str(commit)
    if enabled:
        env["LINX_QEMU_SCOPED_SCALAR_RUNTIME"] = "1"
    command = [
        qemu, "-nographic", "-monitor", "none", "-machine", "virt",
        "-smp", smp, "-bios", "none", "-kernel", obj,
        "-d", "guest_errors", "-trace", "enable=linx_scoped_scalar_access",
        "-D", str(log),
    ]
    try:
        subprocess.run(command, env=env, timeout=0.2, check=False)
    except subprocess.TimeoutExpired:
        pass
    output = log.read_text(encoding="utf-8")
    inactive = "inactive scoped Scalar form ldi_32_d82a643f7a2b at PC=0x10016"
    if inactive not in output:
        raise SystemExit(f"{name}: missing inactive-carrier diagnostic")
    if "illegal instruction @ 0x10016 (insn_len=8" not in output:
        raise SystemExit(f"{name}: carrier trap lost outer TPC or length")
    if "linx_scoped_scalar_access" in output:
        raise SystemExit(f"{name}: rejected carrier reached memory")
    with commit.open(encoding="utf-8") as stream:
        rows = [json.loads(line) for line in stream if line.strip()]
    traps = [row for row in rows if row.get("pc") == 0x10016]
    if not traps or any(
        row.get("trap_cause") != 4 or row.get("wb_valid") or
        row.get("mem_valid") or row.get("next_pc") != 0x10016
        for row in traps
    ):
        raise SystemExit(f"{name}: imprecise carrier rejection: {traps[:2]}")
print("ok: disabled and SMP configurations reject at the outer TPC "
      "without effects")
PY

QEMU_BIN="$QEMU_BIN" OBJ="$FAULT_OBJ" TMP="$TMP" python3 - <<'PY'
import json
import os
import pathlib
import subprocess

log = pathlib.Path(os.environ["TMP"]) / "fault.log"
commit = pathlib.Path(os.environ["TMP"]) / "fault.jsonl"
env = os.environ.copy()
env["LINX_QEMU_SCOPED_SCALAR_RUNTIME"] = "1"
env["LINX_COMMIT_TRACE"] = str(commit)
command = [
    os.environ["QEMU_BIN"], "-nographic", "-monitor", "none", "-machine",
    "virt", "-smp", "1", "-bios", "none", "-kernel", os.environ["OBJ"],
    "-trace", "enable=linx_scoped_scalar_access",
    "-trace", "enable=linx_deliver_sync_trap", "-D", str(log),
]
try:
    subprocess.run(command, env=env, timeout=0.2, check=False)
except subprocess.TimeoutExpired:
    pass
output = log.read_text(encoding="utf-8")
access = ("linx_scoped_scalar_access PC=0x1000e scope=0 "
          "addr=0x20001 size=8 store=0")
if access not in output:
    raise SystemExit("fault: scope was not attached to the faulting access")
trap = "linx_deliver_sync_trap trap=34 src_acr=0 dst_acr=0 tpc=0x1000e"
if trap not in output:
    raise SystemExit("fault: alignment trap did not preserve the outer TPC")
with commit.open(encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream if line.strip()]
faults = [row for row in rows if row.get("pc") == 0x1000e]
if faults:
    raise SystemExit(
        f"fault: faulting carrier retired or published state: {faults[:2]}"
    )
print("ok: scoped alignment fault preserved outer TPC and destination state")
PY
