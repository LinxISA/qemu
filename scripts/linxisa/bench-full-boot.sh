#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LINX_ROOT="$(cd "$ROOT/../.." && pwd)"

FULL_BOOT_PY="${FULL_BOOT_PY:-$LINX_ROOT/kernel/linux/tools/linxisa/initramfs/full_boot.py}"
BENCH_RUNS="${BENCH_RUNS:-7}"
TIMEOUT="${TIMEOUT:-120}"
QEMU_BUILD="${QEMU_BUILD:-$ROOT/build}"
QEMU_BIN="${QEMU_BIN:-}"
QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-}"
SCRIPT="${SCRIPT:-}"

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

if [[ ! -f "$FULL_BOOT_PY" ]]; then
  echo "error: full_boot.py not found: $FULL_BOOT_PY" >&2
  exit 1
fi
if [[ -z "$QEMU_BIN" ]]; then
  echo "error: qemu-system-linx64 not found." >&2
  echo "       set QEMU_BIN=... or QEMU_BUILD=..." >&2
  exit 1
fi

echo "[bench] full_boot=$FULL_BOOT_PY"
echo "[bench] qemu=$QEMU_BIN"
echo "[bench] runs=$BENCH_RUNS timeout=${TIMEOUT}s"
if [[ -n "$QEMU_EXTRA_ARGS" ]]; then
  echo "[bench] extra_args=$QEMU_EXTRA_ARGS"
fi

export SCRIPT
python3 - "$FULL_BOOT_PY" "$BENCH_RUNS" "$QEMU_BIN" "$QEMU_EXTRA_ARGS" "$TIMEOUT" <<'PY'
import math
import os
import statistics
import subprocess
import sys
import time

full_boot = sys.argv[1]
runs = int(sys.argv[2])
qemu_bin = sys.argv[3]
qemu_extra = sys.argv[4]
timeout_s = sys.argv[5]
script = os.environ.get("SCRIPT", "")

times = []
for i in range(1, runs + 1):
    env = os.environ.copy()
    env["SKIP_BUILD"] = "1"
    env["TIMEOUT"] = timeout_s
    env["QEMU"] = qemu_bin
    env["QEMU_EXTRA_ARGS"] = qemu_extra
    if script:
        env["SCRIPT"] = script
    elif "SCRIPT" in env:
        del env["SCRIPT"]

    t0 = time.perf_counter()
    proc = subprocess.run(
        [sys.executable, full_boot],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    elapsed = time.perf_counter() - t0
    print(f"run {i:02d}/{runs}: {elapsed:.3f}s rc={proc.returncode}")
    if proc.returncode != 0:
        tail = "\n".join(proc.stdout.splitlines()[-240:])
        print(tail)
        sys.exit(proc.returncode)
    times.append(elapsed)

times_sorted = sorted(times)
median = statistics.median(times_sorted)
p95_index = max(0, math.ceil(0.95 * len(times_sorted)) - 1)
p95 = times_sorted[p95_index]
print(f"summary: median={median:.3f}s p95={p95:.3f}s runs={runs}")
PY
