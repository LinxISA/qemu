# LinxISA QEMU Emulator

## Scope
`emulator/qemu` is the LinxISA QEMU fork used for ISA/runtime validation, Linux boot, and strict trap/system behavior checks.

## Upstream
- Repository: `https://github.com/LinxISA/qemu`
- Merge-back target branch: `master`

## What This Submodule Owns
- Linx target implementation (`target/linx`)
- Linx decode/translate/execute paths and trap behavior
- QEMU binaries used by AVS/runtime gates (`qemu-system-linx64`)

## Canonical Build and Test Commands
Run from `/Users/zhoubot/linx-isa`.

```bash
cd /Users/zhoubot/linx-isa/emulator/qemu
./configure --target-list=linx64-softmmu
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

cd /Users/zhoubot/linx-isa/avs/qemu
LINX_DISABLE_TIMER_IRQ=0 \
CLANG=/Users/zhoubot/linx-isa/compiler/llvm/build-linxisa-clang/bin/clang \
LLD=/Users/zhoubot/linx-isa/compiler/llvm/build-linxisa-clang/bin/ld.lld \
QEMU=/Users/zhoubot/linx-isa/emulator/qemu/build/qemu-system-linx64 \
./run_tests.sh --all --timeout 10

LINX_DISABLE_TIMER_IRQ=0 \
CLANG=/Users/zhoubot/linx-isa/compiler/llvm/build-linxisa-clang/bin/clang \
LLD=/Users/zhoubot/linx-isa/compiler/llvm/build-linxisa-clang/bin/ld.lld \
QEMU=/Users/zhoubot/linx-isa/emulator/qemu/build/qemu-system-linx64 \
./check_system_strict.sh
```

## LinxISA Integration Touchpoints
- Runtime AVS in `/Users/zhoubot/linx-isa/avs/qemu`
- Linux boot and musl smoke flows (`kernel/linux`, `avs/qemu/run_musl_smoke.py`)
- SPEC/QEMU bring-up scripts under `/Users/zhoubot/linx-isa/tools/spec2017/`

## Related Docs
- `/Users/zhoubot/linx-isa/docs/project/navigation.md`
- `/Users/zhoubot/linx-isa/docs/bringup/`
- `/Users/zhoubot/linx-isa/avs/qemu/README.md`
