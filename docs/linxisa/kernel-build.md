# LinxISA Linux kernel: build + run (QEMU `virt`)

This note captures the current **known-good** build commands for the LinxISA
Linux bring-up kernel and a **size comparison** against a comparable RISC-V
NOMMU build from the same Linux tree.

## Prereqs (macOS host)

- GNU Make: Homebrew `gmake` (kernel requires GNU Make >= 4.0)
- GNU sed: Homebrew `gsed` (some kernel build steps assume GNU sed)
- LLVM toolchain (with LinxISA backend): `~/llvm-project/build-linxisa-clang`
- QEMU LinxISA: `~/qemu` (this repo)

## Build LinxISA kernel (`ARCH=linx`)

```bash
LLVM_BUILD="$HOME/llvm-project/build-linxisa-clang"
LINUX="$HOME/linux"
OUT="$LINUX/build-linx-fixed"

export PATH="$LLVM_BUILD/bin:$PATH"

cd "$LINUX"
gmake O="$OUT" ARCH=linx LLVM=1 LLVM_IAS=1 \
  HOSTCC=/usr/bin/clang HOSTCXX=/usr/bin/clang++ \
  HOSTLD="$LLVM_BUILD/bin/ld.lld" \
  SED=/opt/homebrew/bin/gsed \
  linxisa_virt_defconfig

gmake O="$OUT" ARCH=linx LLVM=1 LLVM_IAS=1 \
  HOSTCC=/usr/bin/clang HOSTCXX=/usr/bin/clang++ \
  HOSTLD="$LLVM_BUILD/bin/ld.lld" \
  SED=/opt/homebrew/bin/gsed \
  -j"$(sysctl -n hw.ncpu)" \
  vmlinux
```

Optional: produce a flat binary image for size comparisons:

```bash
"$LLVM_BUILD/bin/llvm-objcopy" -O binary "$OUT/vmlinux" "$OUT/vmlinux.bin"
```

## Run on QEMU

```bash
"$HOME/qemu/build/qemu-system-linx64" \
  -nographic -monitor none -machine virt -m 512M -smp 1 \
  -kernel "$OUT/vmlinux" \
  -append "lpj=1000000 loglevel=8 slab_nomerge"
```

Notes:

- `-m 512M` is currently recommended for bring-up stability.
- `slab_nomerge` is still recommended while the allocator/per-cpu paths are
  being stabilized (see `~/linux/Documentation/linxisa/bringup-status.md`).
- `lpj=` is still recommended for bring-up even though the LinxISA port now
  registers a QEMU-backed timer (SSR TIME + TIMER_TIMECMP) and a trap vector
  (EVBASE). Delay-loop calibration needs a more robust early timing path.

## Build RISC-V NOMMU kernel (`ARCH=riscv`) for comparison

Use the `nommu_virt_defconfig` so the config family is comparable to the current
LinxISA `virt` bring-up kernel (NOMMU).

```bash
LLVM_BUILD="$HOME/llvm-project/build-linxisa-clang"
LINUX="$HOME/linux"
OUT="$LINUX/build-riscv-virt"

export PATH="$LLVM_BUILD/bin:$PATH"

cd "$LINUX"
gmake O="$OUT" ARCH=riscv LLVM=1 LLVM_IAS=1 \
  HOSTCC=/usr/bin/clang HOSTCXX=/usr/bin/clang++ \
  HOSTLD="$LLVM_BUILD/bin/ld.lld" \
  SED=/opt/homebrew/bin/gsed \
  nommu_virt_defconfig

gmake O="$OUT" ARCH=riscv LLVM=1 LLVM_IAS=1 \
  HOSTCC=/usr/bin/clang HOSTCXX=/usr/bin/clang++ \
  HOSTLD="$LLVM_BUILD/bin/ld.lld" \
  SED=/opt/homebrew/bin/gsed \
  -j"$(sysctl -n hw.ncpu)" \
  vmlinux Image
```

Optional: a flat binary identical in spirit to `arch/riscv/boot/Image`:

```bash
"$LLVM_BUILD/bin/llvm-objcopy" -O binary "$OUT/vmlinux" "$OUT/vmlinux.bin"
```

## Size comparison (example commands)

```bash
ls -lh "$HOME/linux/build-linx-fixed/vmlinux" \
       "$HOME/linux/build-linx-fixed/vmlinux.bin" \
       "$HOME/linux/build-riscv-virt/vmlinux" \
       "$HOME/linux/build-riscv-virt/arch/riscv/boot/Image"

"$HOME/llvm-project/build-linxisa-clang/bin/llvm-size" \
  "$HOME/linux/build-linx-fixed/vmlinux" \
  "$HOME/linux/build-riscv-virt/vmlinux"
```

## Size comparison (measured snapshot)

Measured from this workspace on **2026-02-07**:

- LinxISA `vmlinux`: `6,824,792` bytes
- LinxISA `vmlinux.bin`: `3,585,984` bytes
- RISC-V `vmlinux`: `7,101,720` bytes
- RISC-V `Image`: `3,527,716` bytes

See `~/linux/Documentation/linxisa/bringup-status.md` for the latest numbers.
