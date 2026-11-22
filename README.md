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

## PTO 0.59.1 Message Buffer receive backend

`target/linx/msgb_receive.c` implements the released receive-side bank
transitions for queue admission, claim, data reads, release, ACK, ACKSTAT and
ACKID. Each bank owns its receive epoch and 48-bit claim sequence, so claims
from different endpoints allocate isolated tokens. Failed claims and releases
change only RX result diagnostics. Failed ACK attempts change only ACKSTAT and
ACKID diagnostics. Queue, snapshot, read mask, claim sequence and the last
successful token remain unchanged.

This backend has no guest-visible register connection yet. Transport admission
must supply a validated packet and active nonzero invocation. Later integration
owns endpoint selection, configuration, permission and GM checks, and the
`0x0830..0x084b` SSR helpers. BSE and BWE remain outside this receive slice.

## PTO 0.59.1 scoped Scalar candidate

Set `LINX_QEMU_SCOPED_SCALAR_RUNTIME=1` to execute the locked PTO 0.59.1
immediate, PC-relative and register-offset single accesses, including their
pre-index and post-index writeback forms, paired RAM accesses, and compressed
C.LWI, C.LDI, C.SWI, and C.SDI. The candidate accepts 177 base forms and all
three scope codes. Each memory helper receives
the carrier's scope together with the address, size and outer TPC. Paired
accesses check both addresses before effects and publish load destinations
after both reads. A pair targeting device memory raises a data access fault
before guest data effects.
Scoped Reg5 sources check T/U validity before the access. Compressed loads
push T, and compressed stores use the existing T#1 value without consuming it.
Successful carriers retire eight bytes. Faults and rejected carriers retain the
outer TPC and suppress destination and memory publication.

The candidate supports `-smp 1` and Core4 (`-smp 4`, one thread per PE).
Each scoped access completes synchronously through the serialized GM path.
The shared coherent RAM supplies an execution for scope-conforming accesses.
Private scope is local to a PE, and Core4 supplies a single Core domain for
IntraCore communication. The `linx_scoped_scalar_access` trace records scope and
PE identity. The Core4 smoke checks cross-PE publication and final memory on
single-threaded and multi-threaded TCG.

The runtime switch also admits the exact locked PTO 0.59.1 ELF identity on
these topologies. The loader rejects mixed identities and incorrect projection
hashes. The default v0.58 path remains active without the switch.

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

## PTO 0.59.1 HSCB worker geometry candidate

`BLOCKNUM` (`0x0050`) and `BLOCKID` (`0x0051`) are read-only. Their ambient
value is zero. An HSCB owner may bind one live ACR0 worker geometry with
`linx_hscb_geometry_bind` after LAUNCH admission and clear it with the exact
invocation through `linx_hscb_geometry_clear` after matching DONE or invocation
teardown. Other ACRs continue to observe zero. The binding is included in CPU
migration state.

The HSCB dispatcher owns calls at LAUNCH, DONE and teardown boundaries. Obsolete
aliases `0x0052` through `0x0055`, high-address aliases, and all writes to the
two read-only registers raise the existing QEMU illegal-instruction trap.

## LinxISA Integration Touchpoints
- Runtime AVS in `/Users/zhoubot/linx-isa/avs/qemu`
- Linux boot and musl smoke flows (`kernel/linux`, `avs/qemu/run_musl_smoke.py`)
- SPEC/QEMU bring-up scripts under `/Users/zhoubot/linx-isa/tools/spec2017/`

## Related Docs
- `/Users/zhoubot/linx-isa/docs/project/navigation.md`
- `/Users/zhoubot/linx-isa/docs/bringup/`
- `/Users/zhoubot/linx-isa/avs/qemu/README.md`
