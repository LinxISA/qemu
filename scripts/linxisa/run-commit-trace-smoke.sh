#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

LLVM_BUILD="${LLVM_BUILD:-$HOME/llvm-project/build-linxisa-clang}"
LLVM_MC="${LLVM_MC:-$LLVM_BUILD/bin/llvm-mc}"

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

if [[ ! -x "$LLVM_MC" ]]; then
  echo "error: llvm-mc not found/executable: $LLVM_MC" >&2
  echo "       set LLVM_MC=... or LLVM_BUILD=... (see docs/linxisa/README.md)" >&2
  exit 1
fi
if [[ -z "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found." >&2
  echo "       set QEMU_BIN=... or QEMU_BUILD=..., or build QEMU (see docs/linxisa/README.md)" >&2
  exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-commit-trace.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

OUT_O="$TMP/commit_trace_smoke.o"
OUT_TRACE="$TMP/commit_trace_smoke.jsonl"
SRC="$ROOT/tests/linxisa/commit_trace_smoke.s"

echo "[llvm-mc] -triple=linx64 $SRC"
"$LLVM_MC" -triple=linx64 -filetype=obj "$SRC" -o "$OUT_O"

echo "[run] LINX_COMMIT_TRACE=$OUT_TRACE $QEMU_BIN -kernel $OUT_O"
LINX_COMMIT_TRACE="$OUT_TRACE" \
  "$QEMU_BIN" -nographic -monitor none -machine virt -kernel "$OUT_O"

OUT_TRACE="$OUT_TRACE" python3 - <<'PY'
import json, os, sys

path = os.environ.get("OUT_TRACE")
if not path:
  # Script-local fallback
  path = sys.argv[1] if len(sys.argv) > 1 else None
if not path or not os.path.exists(path):
  print(f"error: trace not found: {path}", file=sys.stderr)
  sys.exit(1)

with open(path, "r", encoding="utf-8") as f:
  rows = [json.loads(line) for line in f if line.strip()]

if len(rows) < 6:
  print(f"error: expected >=6 trace rows, got {len(rows)}", file=sys.stderr)
  sys.exit(1)

for i, r in enumerate(rows):
  if r.get("cycle") != i:
    print(f"error: cycle not monotonic at {i}: {r.get('cycle')}", file=sys.stderr)
    sys.exit(1)

if not any(r.get("wb_valid") for r in rows):
  print("error: expected at least one wb_valid=1 record", file=sys.stderr)
  sys.exit(1)
if not any(r.get("mem_valid") for r in rows):
  print("error: expected at least one mem_valid=1 record", file=sys.stderr)
  sys.exit(1)
print(f"ok: {len(rows)} trace rows")
PY
