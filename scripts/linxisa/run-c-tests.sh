#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

LINXISA_DIR="${LINXISA_DIR:-$HOME/linxisa}"
LLVM_BUILD="${LLVM_BUILD:-$HOME/llvm-project/build-linxisa-clang}"
CLANG="${CLANG:-$LLVM_BUILD/bin/clang}"
QEMU_BUILD="${QEMU_BUILD:-$ROOT/build}"

SRC_DIR="$LINXISA_DIR/compiler/llvm/tests/c"

find_qemu() {
  local name="$1"
  local cand
  for cand in \
    "$QEMU_BUILD/$name" \
    "$QEMU_BUILD/${name}-unsigned" \
    "$ROOT/build/$name" \
    "$ROOT/build/${name}-unsigned" \
    "$ROOT/build-tci/$name" \
    "$ROOT/build-tci/${name}-unsigned" \
    "$ROOT/build-linx/$name" \
    "$ROOT/build-linx/${name}-unsigned"; do
    if [[ -x "$cand" ]]; then
      echo "$cand"
      return 0
    fi
  done
  return 1
}

QEMU_LINX64="$(find_qemu qemu-system-linx64 || true)"
QEMU_LINX32="$(find_qemu qemu-system-linx32 || true)"

if [[ ! -x "$CLANG" ]]; then
  echo "error: clang not found/executable: $CLANG" >&2
  exit 1
fi
if [[ ! -d "$SRC_DIR" ]]; then
  echo "error: LinxISA C tests not found: $SRC_DIR" >&2
  exit 1
fi
if [[ ! -x "$QEMU_LINX64" ]]; then
  echo "error: qemu-system-linx64 not found (set QEMU_BUILD=... or build it)." >&2
  exit 1
fi
if [[ -z "$QEMU_LINX32" ]]; then
  echo "warning: qemu-system-linx32 not found; skipping linx32 tests." >&2
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/linxisa-qemu-tests.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat >"$TMP/runner.c" <<'EOF'
#include <stdint.h>

#include "01_arith.c"
#include "02_control_flow.c"
#include "03_arrays.c"
#include "04_structs.c"
#include "05_switch.c"
#include "06_recursion.c"
#include "07_constants.c"
#include "08_loadstore.c"
#include "09_bitops.c"
#include "10_select.c"
#include "11_minmax.c"
#include "12_more_ops.c"
#include "13_br_ge.c"
#include "14_lui.c"
#include "15_i32_imms.c"
#include "16_andorw.c"
#include "17_indexed.c"
#include "18_setret_relax.c"
#include "19_shifted_add.c"

static int failures;

#define CHECK(expr) do { if (!(expr)) { failures++; } } while (0)

static int run_all(void)
{
  /* 01_arith */
  CHECK(add_i32(5, -7) == -2);
  CHECK(sub_i32(5, -7) == 12);
  CHECK(mul_u32(12345u, 6789u) == 83810205u);
  CHECK(divrem_u32(1000u, 37u) == 28u);
#if __SIZEOF_LONG__ == 8
  CHECK(add_i64(1000000l, 7l) == 876551l);
  CHECK(mul_u64(123456789ul, 987654321ul) == 121932631112635269ul);
  CHECK(divrem_u64(1000ul, 37ul) == 28ul);
#endif

  /* 02_control_flow */
  CHECK(sum_upto(10l) == 53l);
  CHECK(classify_i32(-1) == -1);
  CHECK(classify_i32(0) == 0);
  CHECK(classify_i32(42) == 1);
  CHECK(popcount_u64(0xF0F0F0F0ul) == 16ul);
  CHECK(loop_mix(10) == -5);

  /* 03_arrays */
  {
    int a[4] = { 1, 2, 3, 4 };
    int b[4] = { 10, 20, 30, 40 };
    int c[5];
    int i;
    CHECK(dot_i32(a, b, 4) == 300l);
    fill_i32(c, 5, 7);
    for (i = 0; i < 5; i++) {
      CHECK(c[i] == (7 + i));
    }
    CHECK(local_array_sum(10) == 135l);
    CHECK(local_array_sum(100) == 1488l);
  }

  /* 04_structs */
  {
    struct S s;
    struct_write(&s, 7);
    CHECK(s.a == 7);
    CHECK(s.b == 8);
    CHECK(s.c == (long)7 * 1000l);
    CHECK(s.d == (signed char)(7 ^ 0x5a));
    CHECK(struct_sum(&s) == 7108l);
  }

  /* 05_switch */
  CHECK(switch_test(0) == 11);
  CHECK(switch_test(1) == -3);
  CHECK(switch_test(2) == 0);
  CHECK(switch_test(3) == 5);
  CHECK(switch_test(4) == 16);
  CHECK(switch_test(5) == 2);
  CHECK(switch_test(6) == 0x1232);
  CHECK(switch_test(7) == 14);
  CHECK(switch_test(-1) == 6);

  /* 06_recursion */
  CHECK(fib(10) == 55);
  CHECK(fact(10ul) == 3628800ul);

  /* 07_constants */
  CHECK(const_u32() == (unsigned long)0x89abcdefu);
  CHECK(const_u32_allones() == (unsigned long)0xffffffffu);
  CHECK(and_mask((unsigned long)0xffffffffu) == (unsigned long)0x12345678u);
  CHECK(add_mask((unsigned long)0xffffffffu) == (unsigned long)(0x12345678u + 100000u));

  /* 08_loadstore */
  {
    signed char s8 = -5;
    unsigned char u8 = 250;
    short s16 = -1234;
    unsigned short u16 = 56789;
    int i32 = -0x1234567;
    unsigned u32 = 0x89abcdefu;
#if __SIZEOF_LONG__ == 8
    unsigned long u64 = 0x1122334455667788ul;
#else
    unsigned long u64 = 0x55667788ul;
#endif

    CHECK(load_s8(&s8) == -5);
    CHECK(load_u8(&u8) == 250u);
    CHECK(load_s16(&s16) == -1234);
    CHECK(load_u16(&u16) == 56789u);
    CHECK(load_i32(&i32) == -0x1234567);
    CHECK(load_u32_zext(&u32) == (unsigned long)0x89abcdefu);
    CHECK(load_i64(&u64) == u64);

    store_i8(&u8, 42);
    CHECK(u8 == 42);
    store_i16(&u16, 0x1234);
    CHECK(u16 == 0x1234);
    store_i32(&u32, 0x76543210u);
    CHECK(u32 == 0x76543210u);
    store_i64(&u64, (unsigned long)0xA5A5A5A5u);
    CHECK(u64 == (unsigned long)0xA5A5A5A5u);
  }

  /* 09_bitops */
  CHECK(pack_bytes(1, 2, 3, 4) == 0x04030201ul);
#if __SIZEOF_LONG__ == 8
  CHECK(rotl_u64(0x0123456789abcdeful, 8) == 0x23456789abcdef01ul);
  CHECK(rotr_u64(0x0123456789abcdeful, 8) == 0xef0123456789abcdul);
#else
  /* Avoid undefined shifts on ILP32. */
  CHECK(rotl_u64(0x89abcdeful, 0) == 0x89abcdeful);
  CHECK(rotr_u64(0x89abcdeful, 0) == 0x89abcdeful);
#endif

  /* 10_select */
  CHECK(select_i32(1, 2, 10, 20) == 10);
  CHECK(select_i32(3, 2, 10, 20) == 20);
  CHECK(select_u32(1u, 2u, 10u, 20u) == 10u);
  CHECK(select_u32(3u, 2u, 10u, 20u) == 20u);

  /* 11_minmax */
  CHECK(max_i64(1l, 2l) == 2l);
  CHECK(min_i64(1l, 2l) == 1l);
  CHECK(max_u64(1ul, 2ul) == 2ul);
  CHECK(min_u64(1ul, 2ul) == 1ul);

  /* 12_more_ops */
  CHECK(ashr_i64(0x1234l, 7u) == (0x1234l >> 7));
  CHECK(ashr_i64_imm(0x1234l) == (0x1234l >> 7));
  CHECK(ashr_i32(0x1234, 7u) == (0x1234 >> 7));
  CHECK(lshr_u32_imm(0x80000000u) == 0x10000000u);
  CHECK(lshr_u32(0x80000000u, 3u) == 0x10000000u);
  CHECK(shl_i32(1, 4u) == 16);
  CHECK(div_i64(1000l, 37l) == (1000l / (37l | 1l)));
  CHECK(mod_i64(1000l, 37l) == (1000l % (37l | 1l)));
  CHECK(div_i32(1000, 37) == (1000 / (37 | 1)));
  CHECK(mod_i32(1000, 37) == (1000 % (37 | 1)));
  CHECK(mod_u32(1000u, 37u) == (1000u % (37u | 1u)));
  CHECK(mod_u64(1000ul, 37ul) == (1000ul % (37ul | 1ul)));
  CHECK(andiw_i32(0x12345678) == (0x12345678 & 0x7ff));
  CHECK(oriw_i32(0x12345678) == (0x12345678 | 0x155));
  CHECK(xoriw_i32(0x12345678) == (0x12345678 ^ 0x2aa));
  CHECK(xori_i64(0x1234ul) == (0x1234ul ^ 0x5a5ul));
  CHECK(cmp_ne_i32(1, 2) == 1);
  CHECK(cmp_ne_i32(2, 2) == 0);
  CHECK(cmp_ge_i32(2, 1) == 1);
  CHECK(cmp_ge_i32(1, 2) == 0);
  CHECK(cmp_geu_u32(2u, 1u) == 1u);
  CHECK(cmp_geu_u32(1u, 2u) == 0u);
  CHECK(count_down_ge(10) == 55);
  CHECK(count_down_geu(10u) == 55u);

  /* 13_br_ge (requires call relocations) */
  CHECK(ge_branch(5, 3) == 6);
  CHECK(ge_branch(1, 3) == 2);
  CHECK(geu_branch(5u, 3u) == 6u);
  CHECK(geu_branch(1u, 3u) == 2u);

  /* 14_lui */
  CHECK(aligned_i32() == 0x12345000);
  CHECK(neg_aligned_i32() == -4096);
  CHECK(aligned_i64() == 0x12345000ll);
  CHECK(neg_aligned_i64() == -4096ll);

  /* 15_i32_imms */
  CHECK(andiw_force(0x12345678) == (0x12345678 & 2047));
  CHECK(oriw_force(0x12345678) == (0x12345678 | 341));
  CHECK(xoriw_force(0x12345678) == (0x12345678 ^ 682));

  /* 16_andorw */
  CHECK(andw_force(0x12345678, 0xf0f0f0f0) == (0x12345678 & 0xf0f0f0f0));
  CHECK(orw_force(0x12345678, 0xf0f0f0f0) == (0x12345678 | 0xf0f0f0f0));

  /* 17_indexed */
  {
    signed char b[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int w[8] = { 10, 11, 12, 13, 14, 15, 16, 17 };
    long d[8] = { 100, 101, 102, 103, 104, 105, 106, 107 };

    CHECK(load_i8_idx(b, 6) == 6);
    store_i8_idx(b, 6, -9);
    CHECK(load_i8_idx(b, 6) == -9);

    CHECK(load_i32_idx(w, 4) == 14);
    store_i32_idx(w, 4, -33);
    CHECK(load_i32_idx(w, 4) == -33);

    CHECK(load_i64_idx(d, 3) == 103l);
    store_i64_idx(d, 3, 999l);
    CHECK(load_i64_idx(d, 3) == 999l);
  }

  /* 18_setret_relax (requires call relocation) */
  CHECK(caller_setret_relax(5) == 20106);

  /* 19_shifted_add */
  CHECK(add_sh3(5l, 3l) == 29l);
  CHECK(add_sh7(1l, 2l) == 257l);
  CHECK(addw_sh2(10, 7) == 38);
  CHECK(addw_sh1(10u, 7u) == 24u);

  return failures;
}

__attribute__((noinline))
int _start(void)
{
  int rc = run_all();
  if (rc > 255) {
    rc = 255;
  }
  return rc;
}
EOF

COMMON_FLAGS=(
  -O2
  -ffreestanding
  -fno-builtin
  -fno-stack-protector
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -fno-exceptions
  -fno-jump-tables
  -fno-inline
  -fno-inline-functions
  -fno-inline-functions-called-once
)

run_one() {
  local triple="$1"
  local qemu_bin="$2"
  local out_o="$TMP/runner-${triple}.o"

  echo "[cc] $triple"
  "$CLANG" -target "$triple" "${COMMON_FLAGS[@]}" -I "$SRC_DIR" -c "$TMP/runner.c" -o "$out_o"

  echo "[run] $qemu_bin -kernel $out_o"
  "$qemu_bin" -nographic -monitor none -machine virt -kernel "$out_o"
}

run_one linx64-unknown-elf "$QEMU_LINX64"
if [[ -n "$QEMU_LINX32" ]]; then
  run_one linx32-unknown-elf "$QEMU_LINX32"
fi

echo "ok"
