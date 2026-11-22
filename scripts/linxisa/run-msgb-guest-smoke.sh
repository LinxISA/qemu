#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLVM_MC="${LLVM_MC:?set LLVM_MC to the matching Linx llvm-mc}"
LLVM_READOBJ="${LLVM_READOBJ:-$(dirname "$LLVM_MC")/llvm-readobj}"
QEMU_BIN="${QEMU_BIN:-$ROOT/build/qemu-system-linx64}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/linx-msgb-guest.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/msgb_guest_smoke.s" -o "$TMP/msgb.o"
LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" -nographic -monitor none \
  -machine virt -smp 1 -bios none -kernel "$TMP/msgb.o"
echo "ok: decoded MSGB configuration, BSE, receive, ACK, release, and BWE"

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/hscb_guest_launch_smoke.s" -o "$TMP/hscb.o"
worker_offset=$("$LLVM_READOBJ" --symbols "$TMP/hscb.o" | python3 -c '
import re,sys
text=sys.stdin.read()
pattern=r"Name: hscb_worker_entry .*?Value: (0x[0-9a-fA-F]+)"
match=re.search(pattern, text, re.S)
if not match:
    raise SystemExit("missing hscb_worker_entry")
print(int(match.group(1), 16) + 0x10000)
')
machine="virt,hscb-auto-launch=1,hscb-worker-pe=2,hscb-worker-thread=1"
machine+=",hscb-dispatch-id=77,hscb-entry=$worker_offset"
machine+=",hscb-argument=305419896,hscb-blocknum=8,hscb-blockid=3"
LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" -nographic -monitor none \
  -machine "$machine" \
  -smp 4 -accel tcg,thread=single -bios none -kernel "$TMP/hscb.o"
echo "ok: READY, LAUNCH, worker entry, geometry, DONE, and drain"
