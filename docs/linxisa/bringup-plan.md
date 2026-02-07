# LinxISA Linux bring-up plan (QEMU `virt`)

Goal: boot a real Linux kernel on the LinxISA QEMU `virt` machine, iterating
from “prints a banner” to “boots initramfs”.

This plan assumes **linx64 first**, **single CPU**, and the current `virt`
hardware model (RAM + UART + exit register).

## Milestones

### M0: Baseline sanity (toolchain + QEMU)

- [ ] `scripts/linxisa/run-hello-uart.sh` prints and exits.
- [ ] `scripts/linxisa/run-c-tests.sh` passes (optional; depends on `~/linxisa`).

### M1: Define a LinxISA Linux boot ABI (for QEMU + kernel)

Decide and document a stable register ABI for the first-stage boot protocol
(similar to RISC-V/ARM64):

- [ ] How the kernel receives: hart/cpu id, DTB pointer, initrd pointer/size,
      and cmdline pointer/length.
- [ ] Whether to use DT at all for `virt` (recommended), vs hard-coded platform.
- [ ] Alignment/placement rules for boot blobs (DTB/initrd/cmdline).
- [ ] Validate the ABI using `scripts/linxisa/run-linux-bootstub.sh` (prints
      `hartid`, `fdt`, and a few `/chosen` + `/memory` properties).

### M2: Linux architecture skeleton (`arch/linx/`) builds

Target outcome: `make ARCH=linx LLVM=1` produces a `vmlinux` that QEMU can load.

- [ ] Add `arch/linx/Kconfig` + `arch/linx/Makefile`
- [ ] Add `arch/linx/kernel/head.S` that defines `_start`
- [ ] Provide minimum required headers in `arch/linx/include/asm/`
- [ ] Reach `start_kernel()` and `panic()` with a working stack

### M3: Early console output

Target outcome: kernel prints reliably on QEMU `-nographic`.

- [ ] Implement earlycon/early printk using the `virt` UART (`0x10000000`)
- [ ] Add a simple `virt` platform description (DT or fixed addresses)

### M4: Exceptions + timers (still minimal)

- [ ] Basic trap handler (illegal instruction / page faults / breakpoint)
- [x] Trap vector base (`EVBASE`) + timer interrupt skeleton
- [x] Timer source for `sched_clock()` / clocksource + `jiffies` tick
- [ ] `delay` calibration (even a crude one initially)

### M5: MMU + memory management

- [ ] Page table format + `paging_init()`
- [ ] `ioremap`, `vmalloc`, and basic cache/TLB ops
- [ ] `copy_to/from_user` stubs (if no user mode yet, keep minimal)

### M6: Boot to userspace

- [ ] Initramfs loading/passing (QEMU `-initrd` support or built-in initramfs)
- [ ] `/init` runs and you get a shell (busybox)
- [ ] Clean shutdown path (write exit register or poweroff)

## Notes / recommendations

- Keep `virt` minimal: UART-first, no interrupts at the beginning.
- Make QEMU and Linux agree on **one** boot ABI early; it saves time later.
- Start with a fixed physical load address (`0x10000`) for simplicity; you can
  add relocations/PIE once basic boot works.
