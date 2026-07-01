# LinxISA Linux bring-up plan (QEMU `virt`)

This bring-up plan is **mirrored** between:

- Linux: `/Users/zhoubot/linux/Documentation/linxisa/bringup-plan.md` (**canonical; edit here**)
- QEMU: `/Users/zhoubot/qemu/docs/linxisa/bringup-plan.md` (synced copy; do not hand-edit)

Sync helper:

```bash
cd /Users/zhoubot/linux
tools/linxisa/sync-docs.sh
```

If you don’t have the QEMU repo at `/Users/zhoubot/qemu`, you can skip syncing
the mirror by running `SKIP_QEMU=1 tools/linxisa/sync-docs.sh`.

## Summary (as of 2026-02-07)

This plan:

1. Reflects **current progress** (kernel boots to initramfs shell on QEMU `virt`).
2. Captures the explicit **gap to “full Linux”** (MMU, SMP, signals, real userspace, devices).
3. Defines **mirroring** (Linux canonical + QEMU synced copy).
4. Adds **verification expectations** for **QEMU + RTL difftest** at each milestone.

## Definitions / success criteria

**“Full Linux implementation” target (upstream-like):**

- **MMU-capable**, **SMP-capable** Linux port that can run standard ELF userspace
  (musl/glibc), signals, pthreads, and common subsystems.
- Primary execution target is **QEMU** (`qemu-system-linx64 -machine virt`), but
  every milestone has a **RTL difftest** expectation.

**Baseline assumptions (current):**

- ABI: **linx64 (LP64)** — see `Documentation/linxisa/abi.md`.
- Boot ABI (QEMU `virt`): `a0=hartid`, `a1=dtb` (initrd + cmdline via DT `/chosen`).
- Current stage is **NOMMU, 1 CPU**.
- QEMU `virt` provides: RAM + UART + exit-reg + timer SSRs — see `/Users/zhoubot/qemu/docs/linxisa/README.md`.

## Section A — Current bring-up milestones (QEMU `virt`)

### M0: Baseline sanity (toolchain + QEMU)

- [x] `scripts/linxisa/run-hello-uart.sh` prints and exits. (Validated historically; re-validate and record date if needed.)

### M1: Define and validate the LinxISA Linux boot ABI

- [x] Boot ABI is documented in `Documentation/linxisa/abi.md`.
- [x] Bootstub validates DT handoff (`tools/linxisa/bootstub/` + `scripts/linxisa/run-linux-bootstub.sh` in the QEMU repo).

### M2: Linux arch skeleton (`arch/linx/`) builds

- [x] `make ARCH=linx LLVM=1` produces a bootable `vmlinux`.
- [x] Kernel reaches `start_kernel()`.

### M3: Console output

- [x] Kernel prints reliably on QEMU `-nographic` via the `virt` UART.

### M4: Exceptions + timers (minimal)

- [x] Trap vector installed (EVBASE) and basic trap entry/return works.
- [x] Timer interrupt path works enough for `jiffies`/tick.
- [x] Delay works sufficiently for bring-up (generic calibrate + `__delay`).

### M5: MMU + memory management (required for “full Linux”)

- [ ] Define page table format + virtual memory layout.
- [ ] Implement paging_init + TLB/cache ops.
- [ ] Enable `CONFIG_MMU=y` boot on QEMU (requires QEMU MMU implementation).

### M6: Boot to userspace (bring-up initramfs)

- [x] Initramfs runs `/init` and provides an interactive shell.
- [x] Clean shutdown: `poweroff` triggers QEMU exit-reg (no infinite halt).

Evidence:

- Status snapshot: `Documentation/linxisa/bringup-status.md` (2026-02-07).
- Regression: `cd /Users/zhoubot/linux && SKIP_BUILD=1 TIMEOUT=25 python3 tools/linxisa/initramfs/smoke.py`.

## Section B — Stabilize NOMMU (remove bring-up hacks; prerequisite for real userspace)

### M6.1: Fix syscall ABI correctness

Goal: remove userspace shims (no sign-extension or “special casing” in initramfs).

- [x] Syscall return values behave like Linux expects (negative errno, full-width `long`) without userspace shims.
  - Evidence: `tools/linxisa/initramfs/smoke.py` expects `cat /no-such` prints `-ENOENT`.
  - Implementation: QEMU preserves block/queue commit state across ACR transitions; LLVM Blockify does not remap inline-asm operands into the T/U hand queues.
- [x] Stop using per-syscall bring-up switch hacks in `arch/linx/kernel/traps.c`; route through the normal syscall dispatch safely.

### M6.2: Fix procfs/sysfs directory enumeration

- [ ] `ls /proc` shows expected entries.
- [ ] `ls /sys` does not loop/garble; `getdents64` behaves correctly (dir offsets / file positions).

### M6.3: Signals minimum viable (still NOMMU)

- [ ] `rt_sigreturn` is not stubbed.
- [ ] Deliver + return from at least `SIGSEGV` and `SIGILL` in a controlled test.

### M6.4: Remove bring-up debug noise

- [ ] Remove temporary debug printing and early-boot bypasses (explicit list; current sources include `arch/linx/kernel/traps.c` and other early boot paths).
- [ ] Boot log is “normal Linux noisy,” not debug-spew.

## Section C — Full Linux gap checklist (upstream-like)

Each item below includes “done means” plus at least one concrete test.

### C1: Privilege / exception model completeness

- [ ] Illegal instruction, breakpoint, alignment, and access faults produce correct Linux signals/oops.
- Tests: userspace fault suite (see “Testing / acceptance” below).

### C2: MMU + virtual memory

- [ ] `CONFIG_MMU=y` boots.
- [ ] `mmap/munmap/brk` work for real libc.
- [ ] Page faults, copy-on-write, and `fork()` correctness.
- Tests: musl `hello`, `fork` test, `mmap` stress microtest.

### C3: SMP + atomics + memory ordering

- [ ] Define/implement atomic ISA primitives required by Linux (or LL/SC) with a documented memory model.
- [ ] Bring up a second CPU in QEMU + RTL.
- [ ] Timer IRQ per CPU and IPIs (define mechanism and model it).
- Tests: Linux `locktorture`, simple pthread contention tests.

### C4: Device / interrupt architecture

- [ ] Define an interrupt controller model (QEMU + RTL) beyond “timer pending bit”.
- [ ] External interrupts route through Linux IRQ subsystem properly.
- Tests: interrupt storm/latency microtests; UART RX IRQ (if implemented).

### C5: Virt machine devices for real OS workloads

- [ ] Add a block device (recommend **virtio-mmio** first) so Linux can mount a real rootfs.
- [ ] (Optional next) virtio-net for networking.
- Tests: boot with ext4 rootfs; run `init`; run `sh -c 'uname -a; ls'`.

### C6: Toolchain + userspace readiness

- [ ] Clang/LLD (+ binutils if used) produce fully-correct ELF for LinxISA (relocs, TLS, unwind as needed).
- [ ] Run stock BusyBox (musl static) without kernel/userspace ABI hacks.
- Tests: `busybox --help`, `ash` script, syscall surface test set.

### C7: Debugging / profiling

- [ ] GDB works end-to-end (QEMU + kernel support).
- [ ] Ptrace register set stable; core dumps minimally correct.
- (Optional) perf events, ftrace basics.
- Tests: attach gdb, single-step across syscall/trap, ptrace read regs.

### C8: Upstreaming hygiene

- [ ] Kconfig/defconfig reasonable.
- [ ] DT bindings documented if new.
- [ ] Selftests for arch-specific behavior included.
- [ ] Docs (ABI + boot protocol + virt machine notes) consistent.

## Section D — QEMU + RTL difftest requirements (per milestone)

### D0: Reference model policy

- **QEMU is the architectural reference** until RTL is proven golden.
- RTL must match QEMU on the directed tests for each milestone.
- Any known RTL-vs-QEMU deltas must be explicitly listed (with rationale and a plan to eliminate).

### D1: Commit trace schema (minimal contract)

Use a line-oriented, machine-parseable trace format (recommend: JSONL). Each
retired instruction emits one record:

- `pc` (u64): architectural PC of retired instruction
- `insn` (u32/u64): raw encoding (as carried in the ISA)
- `priv` / `acr` (u32): privilege/ACR level at retirement
- `reg_writes`: list of `{ reg, value }`
- `mem_writes`: list of `{ paddr, size, value }`
- `trap`: null or `{ cause, tval, handler_pc }`
- `block`: optional Linx Block-ISA metadata `{ brtype, carg, cond, tgt }` when applicable

Minimum to be “difftest useful”:

- PC + reg writes + mem writes + trap cause/arg.

### D2: Milestone-by-milestone difftest gating

For each milestone, define:

- A directed test list (see “Testing / acceptance”).
- The expected trace fields that must match.
- Whether timing/interrupt interleaving is deterministic (if not, define “compare rules”).

## Testing / acceptance

### Current QEMU smoke (already exists)

```bash
cd /Users/zhoubot/linux
SKIP_BUILD=1 TIMEOUT=25 python3 tools/linxisa/initramfs/smoke.py
```

Expected outcome:

- Reaches `#` prompt and basic commands succeed; no hang/loop.

Convenience runner (today: delegates to the smoke test):

```bash
cd /Users/zhoubot/linux
python3 tools/linxisa/tests/run.py
```

### Required next: syscall/exception conformance microtests

Add a small suite under:

- `/Users/zhoubot/linux/tools/linxisa/tests/` (freestanding + no-libc variants)

Include tests for:

- syscall return sign correctness (negative errno)
- `getdents64` correctness on procfs/sysfs
- signal delivery/return (`SIGILL`, `SIGSEGV`)
- fork/clone basics (once enabled)

### RTL difftest gate (required)

For each test in the suite:

- QEMU run produces a reference commit trace.
- RTL run produces a commit trace that matches (or a documented acceptable delta).

## Notes

- Default ABI: `linx64` only until MMU+SMP stable.
- Default machine: QEMU `virt`.
- “Full Linux” requires coordinated changes across:
  - Linux `arch/linx/`
  - QEMU Linx CPU + `virt` machine
  - RTL + trace/difftest infra
  - Toolchain (relocs + ABI correctness)
