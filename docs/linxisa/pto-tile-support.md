# PTO Tile support on LinxISA 0.58

The QEMU Linx target implements the Tile contract pinned by LinxISA 0.58.3.
The normative operation inventory is the LinxISA `engine_ops.json` projection;
this page describes QEMU behavior and does not define a second ISA taxonomy.

## Engine taxonomy

| Engine | Operations | Responsibility |
| --- | ---: | --- |
| VEC | 31 | Elementwise Tile operations only |
| SFU | 56 | Division, remainder, reductions, transforms, nonlinear functions, and other complex-hardware operations |
| TLSU | 10 | Tile memory access and data movement |
| CUBE | 12 | Matrix and matrix-vector operations |

`TEPL` is only the unchanged two-bit Mode plus five-bit Function encoding
carrier for VEC and SFU. It is not an execution-engine category. The semantic
aliases `BSTART.VEC` and `BSTART.SFU` select the same carrier without changing
any instruction bits.

### VEC operations

```text
TADD TSUB TMUL TAND TOR TXOR TSHL TSHR TMAX TMIN
TCMP TABS TNOT TNEG TRELU TSEL TCVT TFMA
TADDS TSUBS TMULS TANDS TORS TXORS TSHLS TSHRS
TMAXS TMINS TCMPS TSELS TEXPANDS
```

TFMA is active at logical selector `0x01c`. It consumes three Tile sources and
produces one destination. Floating-point execution is fused; integer execution
uses the architectural wrapping result.

### SFU operations

```text
TDIV TREM TDIVS TREMS TEXP TLOG TRECIP TSQRT TRSQRT
TROWSUM TROWMAX TROWMIN TROWPROD TROWARGMAX TROWARGMIN
TCOLSUM TCOLMAX TCOLMIN TCOLPROD TCOLARGMAX TCOLARGMIN
TROWEXPAND TROWEXPANDADD TROWEXPANDSUB TROWEXPANDMUL
TROWEXPANDDIV TROWEXPANDMAX TROWEXPANDMIN TROWEXPANDEXPDIF
TCOLEXPAND TCOLEXPANDADD TCOLEXPANDSUB TCOLEXPANDMUL
TCOLEXPANDDIV TCOLEXPANDMAX TCOLEXPANDMIN TCOLEXPANDEXPDIF
TCONCAT TEXTRACT TINSERT TIMG2COL TFILLPAD TCI TTRI
THISTOGRAM TQUANT TDEQUANT TSORT TMRGSORT TTRANS TGATHER
TSCATTER TPARTADD TPARTMUL TPARTMAX TPARTMIN
```

### TLSU operations

| Function | Operation |
| ---: | --- |
| 0 | TLOAD |
| 1 | TSTORE |
| 2 | TMOV |
| 3 | TPREFETCH |
| 4 | MGATHER |
| 5 | MSCATTER |
| 6 | MGATHER.MASK |
| 7 | MSCATTER.MASK |
| 8 | MGATHER.CAS |
| 13 | GMOV |

TLOAD and TSTORE retain the encoded scalar row stride. An omitted scalar input
uses the instruction-defined dense default; an explicitly encoded zero remains
a zero stride. TSTORE has no destination TSize field and obtains its transfer
extent from the bound source Tile.

The model also recognizes the compiler-private spill transport used by current
PTO kernels: an `S64/NORM` TSTORE may carry the raw bytes of a non-S64 Tile, and
an immediately matching `S64/NORM` TLOAD restores the saved dtype, logical
valid rectangle, physical shape, layout, and capacity. This shadow path is
strictly keyed by the same PE-local base, row count, row-byte count, and byte
stride; ordinary typed transfers continue to require the ASL descriptor and
dtype match. The shadow record is invalidated by overlapping successful
guest-memory writes, including ordinary scalar stores, and is deliberately
dropped across migration because it is not architectural state.

### CUBE operations

```text
TMATMUL TMATMUL.BIAS TMATMUL.ACC
TMATMULMX TMATMULMX.BIAS TMATMULMX.ACC
TGEMV TGEMV.BIAS TGEMV.ACC
TGEMVMX TGEMVMX.BIAS TGEMVMX.ACC
```

CUBE applies the same power-of-two row and column constraints as every other
Tile operation. Invalid function, dtype, shape, layout, or operand schemas fail
before architectural Tile, ACC, descriptor, or memory state is published.
The internal accumulator capacity is independent of the final D allocation;
publication converts the FP64/FP32/S64/U64 staging value to the architectural
FP64/FP32/S32/U32 result or the type selected by `B.FPATR`.

`B.FPATR.TransA` and `TransB` independently transpose the corresponding Shared
matrix primary. A transpose bit with a Local primary is rejected before source
snapshot or output effects. Shared schema checks apply to the stored transposed
shape, while computation observes the original logical M×K or K×N shape.

## Tile capacity and shape

`B.IOT` and `B.IOS` encode a four-bit `SizeCode`:

| SizeCode | B.IOT bytes per selected PE | B.IOS bytes per selected PE |
| ---: | ---: | ---: |
| 1 | 128 | 128 |
| 2 | 256 | 256 |
| 3 | 512 | 512 |
| 4 | 1024 | 1024 |
| 5 | 2048 | 2048 |
| 6 | 4096 | 4096 |
| 7 | 8192 | 8192 |
| 8 | 16384 | 16384 |
| 9 | 32768 | 32768 |
| 10 | 65536 | 65536 |
| 11 | reserved | 131072 |
| 12 | reserved | 262144 |

For ordinary layouts, rows are derived from
`SizeCodeBytes / (columns * element_size)`. CUBE layouts instead derive aligned
storage rows, columns, repeat counts, CELL count, and required bytes from the
logical M/N/K dimensions; valid dimensions need only be positive and fit the
selected M16/M32/N8 layout. QEMU keeps the wire SizeCode separate from its internal
`log2(bytes)-4` allocation code.

## Shared Tile registers and B.IOS

Each core owns one bank of 256 shared Tile registers, assembled as `S0` through
`S255`, visible to its four PEs. `B.IOS` uses an absolute SharedID and the former
`B.IOD` encoding slot. `B.IOD` and `C.B.IOS` are deleted and never decoded.

The three-bit `PEMode` decodes to `0000`, `1000`, `0100`, `0010`, `0001`,
`1100`, `1110`, or `1111`. Mode zero is a strict no-op. Each selected PE
receives its own SizeCode capacity and uses its own GPR
base and offset. A write allocates a new shared-register version and atomically
publishes its descriptor and payload. Reads do not modify descriptors. Initial
contents are undefined like an uninitialized register. QEMU enforces atomicity
but does not add cross-PE ordering; software must prevent conflicting accesses.

Each selected PE contributes the fixed quarter identified by its decoded mask
bit. `SizeCode` is the capacity per participating PE, while capacity accounting
charges `popcount(mask) * SizeCodeBytes`. The first successful update freezes
the allocation mask; later compatible updates may initialize a subset but may
not expand that mask.

## Fail-closed execution

QEMU validates the complete block schema before pinning sources, allocating an
output, mutating descriptors, accessing memory, or publishing results. Reserved
VEC/SFU selectors, TLSU functions, and CUBE functions raise illegal instruction.
Rejected dtype, shape, layout, or operand profiles leave visible state unchanged.

## Verification

Repository-local checks are named after the architecture surface they cover:

```bash
python3 -m unittest discover -s tests/linxisa -p 'test_*.py'
build/tests/unit/test-linx-tile-transaction
build/tests/unit/test-linx-tile-cube-numeric
```

The LinxISA superproject additionally checks QEMU decode metadata against the
canonical 0.58 catalog and runs the hosted AVS and cross-model lanes. A skipped,
missing, pending, or different-commit result is not release evidence.
