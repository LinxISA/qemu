# DavinciOO PTO ISA v0.2 Tile support

This document records the executable Local Tile subset in Linx QEMU. The
normative baseline is DavinciOO `origin/codex/update-intrinsic-docs` at
`3b4fe5e6`; earlier v0.2 drafts are not compatibility profiles. Selector
collisions are decoded only with their latest identity.

Support has three distinct levels:

1. L1: the latest PTO ISA v0.2 selector or function has a unique identity.
2. L2: QEMU executes an operation that changes architectural Tile, ACC, or
   memory state.
3. L3: a LinxISA AVS case executes the operation and checks exact values or
   side effects.

A shared `BSTART.TEPL` decode does not imply support for every TEPL selector or
dtype profile. QEMU validates the selector/dtype tuple when binding `B.IOT`,
before source pinning, output reservation, or destination backing-store
clearing. Unsupported tuples raise an illegal-instruction exception without
changing the Tile queue or destination Tile.

## Summary

| Family | PTO ISA v0.2 target operations | Explicit execution paths | Fail-closed |
| --- | ---: | ---: | ---: |
| TMA | Local functions 0-7 | 8 | Shared functions 8-13 |
| CUBE | Local non-FIXP functions | 5 | MX, Shared, and FIXP variants |
| TEPL | 98 | 81 | 17 |

These counts mean that at least one Local QEMU profile has an execution path.
They do not claim complete dtype, shape, layout, SharedTile, Group MMA, or FIXP
coverage.

The current QEMU Tile state is a single-PE Local model. It executes v5
`B.IOT.PE_MASK=1111`, which is the unambiguous all-participant Local form.
Partial nonzero masks require per-PE identity and masked producer placeholders;
they are rejected until the Core4 model is integrated.

## TMA

The six workbook operations have execution paths:

```text
TLOAD TSTORE TMOV TPREFETCH MGATHER MSCATTER
```

`TPREFETCH` follows the latest destination-free contract: it performs TLOAD-like
address and shape validation without allocating or publishing a Tile output.
QEMU also implements supplemental masked and CAS gather/scatter functions that
are not included in the six-operation workbook count.

## CUBE

The following operations have execution paths:

| Operation | Function | Current QEMU profile |
| --- | ---: | --- |
| `TMATMUL` | 0 | S32 matrix multiply into the internal ACC |
| `TMATMUL_BIAS` | 1 | S32 matrix multiply plus one-row, per-column S32 bias |
| `TMATMUL_ACC` | 2 | Accumulate using the matching live source pair |
| `TGEMV` | 16 | Uses the M/N/K path with `N=1` |
| `TGEMV_ACC` | 18 | Accumulates into the existing TGEMV ACC |

Legacy Function 8 `ACCCVT` is removed in the latest profile and is rejected at
decode. A replacement ACC export contract is required before QEMU can expose
the internal ACC as a normal Tile.

`TMATMUL_BIAS` freezes exactly three sources in A, B, Bias order across one or
more `B.IOT` descriptors. The Bias Tile must be a one-row S32 Tile and is
broadcast by column. The current implementation intentionally rejects other
dtypes rather than treating the word-backed compatibility path as packed S8 or
floating-point CUBE execution.

These CUBE operations remain fail-closed:

- `TGEMV_BIAS`: the latest PTO definition uses `M=1`, while the existing QEMU
  TGEMV compatibility profile uses `N=1`; the direction contract must be
  reconciled before adding bias.
- `TMATMUL_MX` and `TGEMV_MX`: scale Tile roles, FP8/FP4 unpacking, and the
  target MX reconstruction profile are not implemented. QEMU does not ignore
  scale operands and report a normal matmul as MX support.

The current CUBE backing layout is a bounded, contiguous 8x8 CPU profile. It
must not be treated as complete fractal-layout or mixed-precision coverage.

## TEPL

The executable whitelist contains these 81 operations:

```text
TADD TSUB TMUL TDIV TMAX TMIN TAND TOR TXOR TSHL TSHR
TRELU TCVT TEXP TLOG TSQRT TRSQRT TRECIP TABS TNOT TNEG TREM
TADDS TSUBS TMULS TDIVS TMAXS TMINS TANDS TORS TXORS TSHLS TSHRS TREMS
TCMP TCMPS TSEL TSELS
TROWSUM TROWMAX TROWMIN TROWPROD TROWARGMAX TROWARGMIN
TCOLSUM TCOLMAX TCOLMIN TCOLPROD TCOLARGMAX TCOLARGMIN
TROWEXPAND TCOLEXPAND TEXPANDS
TROWEXPANDADD TROWEXPANDSUB TROWEXPANDMUL TROWEXPANDDIV
TROWEXPANDMAX TROWEXPANDMIN TROWEXPANDEXPDIF
TCOLEXPANDADD TCOLEXPANDSUB TCOLEXPANDMUL TCOLEXPANDDIV
TCOLEXPANDMAX TCOLEXPANDMIN TCOLEXPANDEXPDIF
TTRANS TGATHER TSCATTER
TCI TTRI TFILLPAD TDEQUANT TEXTRACT TCONCAT TGATHERB
TPARTADD TPARTMUL TPARTMAX TPARTMIN
```

Important profile limits include:

- Common elementwise paths support 1-, 2-, and 4-byte elements. Implemented
  FP16 and BF16 arithmetic and TCVT encoding use QEMU softfloat. TCVT checks
  both the queued source dtype and destination dtype before source pinning or
  output reservation.
- `TEXP`, `TLOG`, `TSQRT`, `TRSQRT`, `TRECIP`, and `TABS` implement the
  documented FP32 and FP16 profiles. BF16 forms remain rejected.
- FP8/FPL8 profiles are not implemented by the generic arithmetic path and
  are rejected rather than being interpreted as integers of the same width.
- `TCMP` and `TCMPS` produce a row-packed U32 predicate mask. `TSEL` and
  `TSELS` consume that mask.
- Reduction, expand, layout, and partial-operation paths implement the current
  row-major CPU profile; they do not claim every target fractal layout.
- `TFILLPAD` supports the documented Zero/Max/Min subset for the supported
  row-major Vec dtypes.
- `TCONCAT` supports the basic two-source column concatenation form.
- `TEXTRACT` supports the plain same-dtype bounded-window form.
- `TDEQUANT` supports S8/S16 source data, per-row FP32 scale and offset Tiles,
  and FP32 output using
  `dst[r,c] = (src[r,c] - offset[r]) * scale[r]`.
- VMState version 15 adds the Tile shape metadata required to restore this
  execution model. A pre-v15 stream with nonempty Tile state is deliberately
  rejected because its missing shapes cannot be reconstructed; pre-v15 empty
  Tile state remains loadable.

The remaining 17 TEPL operations are intentionally fail-closed:

| Operations | Missing contract or implementation |
| --- | --- |
| `TPRELU`, `TADDC`, `TSUBC`, `TFMA`, `TADDSC`, `TSUBSC`, `TAXPY`, `TINSERT` | Operand counts or existing-destination contracts are not implemented |
| `TFMOD`, `TFMODS`, `TLRELU` | Latest arithmetic semantics do not have a QEMU execution path yet |
| `TQUANT` | INT8/MXFP8/MXFP4 profiles have different metadata, output counts, and packing contracts |
| `TIMG2COL` | Convolution window, repeat, padding, and configuration state are not fully encoded by the current header path |
| `TSORT32` | Sorted value/index compound or multi-output contract is not closed |
| `TMRGSORT` | Variable source list and block-length/executed-count profile are not closed |
| `THISTOGRAM`, `TRANDOM` | Byte selection and random key/counter profiles are not implemented |

In particular, `TQUANT` selector `0x6a` is rejected by the
executable-selector gate. The operation must remain rejected until the chosen
quantization profile uniquely defines the visible inputs, outputs, metadata,
and packed representation.

## Regression evidence

The executable regression lives in the LinxISA superproject rather than this
QEMU submodule:

```text
avs/qemu/tests/10_tile_tma.cpp
avs/qemu/tests/10_tile_cube.cpp
avs/qemu/tests/10_tile_cube_asm.S
avs/qemu/tests/10_tile_tepl.cpp
avs/qemu/tests/10_tile_tepl_asm.S
```

The intended focused L3 gate is:

```bash
QEMU=/path/to/qemu-system-linx64 \
  python3 avs/qemu/run_tests.py --suite tile --timeout 40
```

The existing Tile suite provides exact-value coverage for the earlier v0.2
draft,
including packed comparisons and selections, signed narrow lanes, FP16/BF16
arithmetic, persistent rectangular shape metadata, reductions, expand
operations, fill padding, partial operations, concat/gather-by-byte,
sequence/triangular generation, extraction, dequantization, and TGEMV with
ACC accumulation. AVS ID `0x000A0026` checks exact `TMATMUL_BIAS` results and
the two-descriptor A/B/Bias operand order. AVS IDs `0x000A0029` and
`0x000A002A` checked that FP16 `TEXP` and BF16 `TLOG` trapped, then resumed and stored
the surviving input queue head; all 1024 lanes must match the original source,
which proved that the rejected output was not reserved or published on that
draft. The suite still emits the obsolete `B.IOT` layout and must be migrated
before it can serve as L3 evidence for the latest baseline.

The QEMU-local gates for the latest encoding are:

```bash
python3 tests/linxisa/test_v02_tepl_contract.py
python3 tests/linxisa/test_v02_local_b_iot.py
ninja -C build qemu-system-linx64
```

The representative TEPL negative checks use the stable same-ACR trap-return
path inside the normal Tile suite. The separate cross-ACR standalone
expected-trap harness remains unsuitable as general negative L3 evidence
because that lane can re-enter its test entry.
