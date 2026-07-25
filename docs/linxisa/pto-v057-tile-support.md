# LinxISA v0.57 PTO Tile support

This document records the executable PTO Tile subset in the Linx QEMU target.
The source catalog is the 111-operation v0.57 PTO map maintained by LinxISA:
97 TEPL operations, 6 TMA operations, and 8 CUBE operations.

Support has three distinct levels:

1. L1: the v0.57 selector or function has a unique decode identity.
2. L2: QEMU executes an operation that changes architectural Tile, ACC, or
   memory state.
3. L3: a LinxISA AVS case executes the operation and checks exact values or
   side effects.

A shared `BSTART.TEPL` decode does not imply support for every TEPL selector.
Selectors outside the executable whitelist raise an illegal-instruction
exception before QEMU changes the destination Tile.

## Summary

| Family | v0.57 operations | Explicit execution paths | Fail-closed |
| --- | ---: | ---: | ---: |
| TMA | 6 | 6 | 0 |
| CUBE | 8 | 5 | 3 |
| TEPL | 97 | 82 | 15 |
| Total | 111 | 93 | 18 |

The `93/111` count means that at least one defined QEMU profile has an
execution path. It is not a claim that every dtype, shape, layout, rounding
mode, exception, or target-specific profile is complete.

## TMA

The six workbook operations have execution paths:

```text
TLOAD TSTORE TMOV TPREFETCH MGATHER MSCATTER
```

`TPREFETCH` follows the v0.57 destination-free contract: it performs TLOAD-like
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

Function 8 `ACCCVT` exports the internal ACC to a normal Tile. It is required
by the execution pipeline but is not one of the eight workbook operations.

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

The executable whitelist contains these 82 operations:

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
TRESHAPE TTRANS TGATHER TSCATTER
TCI TTRI TFILLPAD TDEQUANT TEXTRACT TCONCAT TGATHERB
TPARTADD TPARTMUL TPARTMAX TPARTMIN
```

Important profile limits include:

- Common elementwise paths support 1-, 2-, and 4-byte elements. Implemented
  FP16 and BF16 arithmetic uses QEMU softfloat.
- `TCMP` and `TCMPS` produce a row-packed U32 predicate mask. `TSEL` and
  `TSELS` consume that mask.
- Reduction, expand, layout, and partial-operation paths implement the current
  row-major CPU profile; they do not claim every target fractal layout.
- `TFILLPAD` supports the documented Zero/Max/Min subset for the supported
  row-major Vec dtypes.
- `TRESHAPE` is an equal-byte-count bitwise reshape. `TCONCAT` supports the
  basic two-source column concatenation form.
- `TEXTRACT` supports the plain same-dtype bounded-window form.
- `TDEQUANT` supports S8/S16 source data, per-row FP32 scale and offset Tiles,
  and FP32 output using
  `dst[r,c] = (src[r,c] - offset[r]) * scale[r]`.

The remaining 15 TEPL operations are intentionally fail-closed:

| Operations | Missing contract or implementation |
| --- | --- |
| `TAXPY`, `TINSERT` | Require reading and preserving an existing destination, while the current TEPL output is a fresh allocation |
| `TQUANT` | INT8/MXFP8/MXFP4 profiles have different metadata, output counts, and packing contracts |
| `TIMG2COL` | Convolution window, repeat, padding, and configuration state are not fully encoded by the current header path |
| `TDEINTERLEAVE`, `TINTERLEAVE` | Require two Tile outputs; the current TEPL collector publishes one |
| `TSORT` | Sorted value/index compound or multi-output contract is not closed |
| `TMRGSORT` | Variable source list and block-length/executed-count profile are not closed |
| `THISTOGRAM` | Separate source type, destination type, and ByteId are not available in the canonical data-attribute decode |
| `TPARTARGMAX`, `TPARTARGMIN` | Require two outputs and six visible Tile operands |
| `TPUSH`, `TPOP`, `TALLOC`, `TFREE` | Pipe/control operations require a pipe-handle and side-effect ABI outside ordinary TEPL compute |

In particular, `TQUANT` selector `0x083` is decoded but rejected by the
executable-selector gate. The operation must remain rejected until the chosen
quantization profile uniquely defines the visible inputs, outputs, metadata,
and packed representation.

## Regression evidence

The executable regression lives in the LinxISA superproject rather than this
QEMU submodule:

```text
avs/qemu/tests/10_tile_tma.cpp
avs/qemu/tests/10_tile_cube.cpp
avs/qemu/tests/10_tile_cube_bias.S
avs/qemu/tests/10_tile_cube_gemv.S
avs/qemu/tests/10_tile_tepl.cpp
avs/qemu/tests/10_tile_tepl_*.S
```

The focused gate is:

```bash
QEMU=/path/to/qemu-system-linx64 \
  python3 avs/qemu/run_tests.py --suite tile --timeout 40
```

The Tile suite provides exact-value coverage for the implemented batches,
including packed comparisons and selections, signed narrow lanes, FP16/BF16
arithmetic, persistent rectangular shape metadata, reductions, expand
operations, fill padding, partial operations, reshape/concat/gather-by-byte,
sequence/triangular generation, extraction, dequantization, and TGEMV with
ACC accumulation. AVS ID `0x000A0026` checks exact `TMATMUL_BIAS` results and
the two-descriptor A/B/Bias operand order.

Negative expected-trap coverage is not currently a reliable L3 gate because
the standalone AVS illegal-instruction recovery harness can re-enter the test
instead of returning to the expected continuation. This limitation does not
change the fail-closed implementation policy.
