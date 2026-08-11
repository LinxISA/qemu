# LinxISA QEMU usage guide

`qemu-system-linx64` is the functional reference model for the LinxISA/PTO Tile
target. It implements the `linx64` CPU (`target/linx/`) and the `virt` machine
(`hw/linx/virt.c`).

## Build

```bash
cd <qemu-checkout>
mkdir -p build-linx && cd build-linx
../configure --target-list=linx64-softmmu --disable-werror
ninja qemu-system-linx64
```

`--disable-werror` is currently required: the tree still has known warnings
(duplicate `linx_mmu_translate` declaration, trace format type mismatches,
`noreturn` functions with `return`, and the unused
`linx_vec_read_reduce_dst`).

## Run a bare ELF

Direct boot of a PTO/Linx ELF:

```bash
timeout 60s qemu-system-linx64 \
  -machine virt -smp 1 -bios none \
  -kernel <program.elf> \
  -nographic -monitor none
```

Options:

- `-m <size>`: guest RAM (default 128 MiB).
- `-smp 4 -accel tcg,thread=single`: four-PE Core4 cooperative execution.
  MTTCG remains disabled because Core4 collectives synchronously update peer
  architectural state.
- Kernel images: `ET_REL` objects with `_start`, `ET_EXEC`, and `ET_DYN`
  executables are all accepted by the `virt` loader.

## Test finisher

Enable the opt-in finisher with the environment variable:

```bash
LINX_VIRT_TEST_FINISHER=1 qemu-system-linx64 ...
```

The finisher is a 32-bit MMIO store to `0x10009000`:

| Value | Meaning | Exit |
| --- | --- | --- |
| `0x5555` | pass | 0 |
| `0x3333` | fail | 1 |
| `0x7777` | reset | - |

In finisher direct boot, when the guest has not installed `EVBASE`
(`EVBASE_ACR1 == 0`), illegal instructions, block/control-flow faults, and
data access faults exit with failure (code 1) instead of replaying the faulting
instruction forever.

Without `LINX_VIRT_TEST_FINISHER=1` and without an installed exception vector, a
bare ELF that hits an illegal instruction or access fault trap-loops
indefinitely. Always run bare ELFs under `timeout` or with the finisher enabled.

## Debug

```bash
# Instruction stream and trap delivery
-d in_asm,int,guest_errors,unimp

# Linx trace events (keep logs in the checkout and bound every run)
mkdir -p build-linx/run-logs
timeout 60s qemu-system-linx64 ... \
  -trace "linx_*" -D build-linx/run-logs/linx.trace

# GDB stub (breakpoints/watchpoints)
-s -S
```

The trap log reports the exact PC that raised the exception, for example:

```text
Linx: exception 2 at PC=0x11354
Linx: illegal instruction at PC=0x11354
```

## Cross-model validation

Run the same current-ISA ELF on QEMU and gfrun, and require both to match the
manifest's independent golden:

```bash
cd <SuperScalarModel-repo>
python3 scripts/cross_model/run_diff.py \
  --models qemu,gfrun \
  --qemu <qemu-checkout>/build-linx/qemu-system-linx64 \
  --gfrun bin/gfrun \
  --case tests/cross_model/cases/<manifest>.json
```

Reports are written under
`regression_results/cross_model/<run-id>/report.md`. Exit code 0 alone is not a
pass: both models must match the independent golden and each other.

## Tests

```bash
# Native unit tests
ninja -C build-linx tests/unit/test-linx-tile-cube-numeric \
  tests/unit/test-linx-tile-transaction
./build-linx/tests/unit/test-linx-tile-cube-numeric
./build-linx/tests/unit/test-linx-tile-transaction

# Python contract tests
python3 -m unittest discover -s tests/linxisa -p 'test_*.py'
```

## Common pitfalls

- Missing `-machine virt` or `-kernel` leaves nothing to boot.
- Bare ELF without finisher/`EVBASE` hangs on illegal instructions; use
  `timeout` or `LINX_VIRT_TEST_FINISHER=1`.
- Keep generated logs under `build-linx/run-logs/` and set a timeout for every
  emulator or cross-model run so a trap loop cannot generate unbounded output.
- Four-PE cooperative runs need `-smp 4 -accel tcg,thread=single`.
- A QEMU and gfrun timeout is not a numeric mismatch: check the trap PC with
  `-d in_asm,int` before blaming the implementation.
