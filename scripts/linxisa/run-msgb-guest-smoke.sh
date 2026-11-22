#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLVM_MC="${LLVM_MC:?set LLVM_MC to the matching Linx llvm-mc}"
QEMU_BIN="${QEMU_BIN:-$ROOT/build/qemu-system-linx64}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/linx-msgb-guest.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

"$LLVM_MC" -triple=linx64 -filetype=obj \
  "$ROOT/tests/linxisa/msgb_guest_smoke.s" -o "$TMP/msgb.o"
LINX_VIRT_TEST_FINISHER=1 "$QEMU_BIN" -nographic -monitor none \
  -machine virt -smp 1 -bios none -kernel "$TMP/msgb.o"
echo "ok: decoded MSGB configuration, BSE, receive, ACK, release, and BWE"
