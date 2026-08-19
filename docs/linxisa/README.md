# LinxISA: Linux bring-up on QEMU

This QEMU tree contains:

- A LinxISA CPU target (`target/linx/`)
- A minimal LinxISA `virt` machine (`hw/linx/virt.c`)

The current executable PTO Tile subset and its fail-closed boundaries are
recorded in [`pto-tile-support.md`](pto-tile-support.md).

## Quick start (smoke test)

Build/run a tiny freestanding `_start` that prints via the `virt` UART and powers
off:

```bash
scripts/linxisa/run-hello-uart.sh
```

## Linux bootstub (boot ABI + DTB handoff)

Build/run the Linux-side bootstub (from `~/linux`) that validates the current
boot register ABI and prints a few DT properties:

```bash
scripts/linxisa/run-linux-bootstub.sh
```

You can pass extra QEMU args (for example `-append` to populate
`/chosen/bootargs`):

```bash
scripts/linxisa/run-linux-bootstub.sh -append "linxisa=1"
```

## VECTOR/CUBE first-use exception contract

Run the guest-ISA matrix against an exact QEMU build and Linx assembler:

```bash
python3 scripts/linxisa/run-first-use-exception-contract.py \
  --llvm-mc /path/to/llvm-mc \
  --qemu /path/to/qemu-system-linx64
```

The runner executes all eight VECTOR headers and all twelve canonical CUBE
headers from ACR2. It validates the precise `E_INST`/`EC_PERM` trap envelope,
the VECTOR/CUBE argument, pre-effect block/queue state, retry after clearing
the matching enable, decode priority, and the ACR0/ACR1 and TEPL negatives.

## Linux source tree setup

To (re)create a `~/linux` working tree based on the latest stable kernel tarball
and put it on a `linxisa/bringup` git branch:

```bash
scripts/linxisa/setup-linux.sh
```

## Building the Linux kernel

See `docs/linxisa/kernel-build.md` for the current build commands (macOS host),
how to run on QEMU, and how to compare kernel image sizes vs RISC-V.

## Running a kernel image

The LinxISA virt machine is driven via:

```bash
qemu-system-linx64 -nographic -monitor none -machine virt -kernel <kernel-image>
```

Current bring-up recommendation for Linux:

- Add `-m 512M -smp 1` for stability.
- Add `-append "lpj=1000000 loglevel=8 slab_nomerge"` for verbose logs and to
  avoid early SLUB cache-merging issues during bring-up.

Kernel image formats:

- ELF relocatable (`ET_REL`) object (`.o`) with `_start` (existing behavior).
- ELF executable (`ET_EXEC`) and PIE (`ET_DYN`) images (supported by the LinxISA
  virt loader in this tree).

### Boot register ABI (current)

On reset, the `virt` machine jumps to `_start` with:

- `a0` (R2): hart/cpu id (`0` for now)
- `a1` (R3): DTB/FDT physical address

See `~/linux/Documentation/linxisa/abi.md` for the full bring-up ABI notes.

### Building an `ET_EXEC` test image (optional)

If your LinxISA clang driver only compiles objects, you can link an executable
directly with `ld.lld`:

```bash
LLVM_BUILD="$HOME/llvm-project/build-linxisa-clang"

"$LLVM_BUILD/bin/clang" -target linx64-unknown-elf -O2 -ffreestanding -fno-builtin \
  -c tests/linxisa/hello_uart.c -o /tmp/hello_uart.o

"$LLVM_BUILD/bin/ld.lld" -e _start -Ttext=0x10000 \
  -o /tmp/hello_uart.elf /tmp/hello_uart.o

build/qemu-system-linx64 -nographic -monitor none -machine virt -kernel /tmp/hello_uart.elf
```

## Virt machine model

Memory map:

- RAM: `0x00000000` .. `ram_size-1` (default: 128 MiB; Linux bring-up currently
  prefers `-m 512M`)
- Kernel load base (ET_REL / PIE bias): `0x00010000`
- UART MMIO: `0x10000000` (size `0x100`)
  - `UART+0x0` write: TX data byte (printed to stdout)
  - `UART+0x4` read: status (bit0 = TX ready, always 1)
- Opt-in test finisher: `0x10009000` 32-bit write (`LINX_VIRT_TEST_FINISHER=1`)
  - `0x5555`: pass (exit status 0)
  - `0x3333`: fail (exit status 1)
  - `0x7777`: reset

### Timer + trap SSRs (bring-up)

In addition to the UART, the LinxISA CPU model provides a small subset of
privileged SSRs used for Linux bring-up:

- `TIME` (`ssrid 0x0010`): virtual time counter (ns)
- `CSTATE` (`0x0020`): common state (includes `I` interrupt enable bit)
- Managing ACR1 (low-12 indices under `0x1fxx`):
  - `EVBASE_ACR1` (`0x1f01`): trap vector base
  - `IPENDING_ACR1` (`0x1f08`): pending interrupt bitmap (bit0 = timer0)
  - `EOIEI_ACR1` (`0x1f0a`): end-of-interrupt (write IRQ id to clear pending)
  - `TIMER_TIMECMP_ACR1` (`0x1f21`): timer compare (absolute ns)

## Debug tips

- Use QEMU trace events for Linx diagnostics, for example:
  `-trace "linx_*" -D /tmp/linx.trace`.
- For breakpoint/watchpoint debugging, run QEMU with gdbstub:
  `-s -S`, then in GDB use `b *<pc>` / `watch *<addr>`.
- Use `-d in_asm,cpu` only for low-level instruction logs (very noisy).
