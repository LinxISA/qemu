#ifndef _ATOMIC_TEST_TMP_H_
#define _ATOMIC_TEST_TMP_H_
#include "atomic.h"
#include <string.h>
#include <stdio.h>

extern void qemu_debug_str(int id, const char *str);
#define qemu_debug_snprintf(buf, size, fmt, ...) do {			\
	snprintf((buf), (size), "[%s@%s:%d] "fmt,			\
		__func__, __FILE__, __LINE__, ##__VA_ARGS__);		\
	qemu_debug_str(0, buf);						\
} while (0)

#define qemu_debug_nprintf(size, fmt, ...) do {				\
	char __buf[size];						\
	qemu_debug_snprintf(__buf, (size), fmt, ##__VA_ARGS__);		\
} while (0)

#define qemu_debug_printf(fmt, ...) do {				\
	qemu_debug_nprintf((512 + 2 * sizeof(fmt""#__VA_ARGS__)),	\
			fmt, ##__VA_ARGS__);				\
} while (0)

/*
    arch_atomic_xchg_relaxed
    arch_atomic_xchg_acquire
    arch_atomic_xchg_release
    arch_atomic_xchg

    arch_atomic64_xchg_relaxed
    arch_atomic64_xchg_acquire
    arch_atomic64_xchg_release
    arch_atomic64_xchg
*/
int Atomic64XchgTestCase(s64 val, s64 preVal, s64 wantRet);
int AtomicXchgTestCase(int val, int preVal, int wantRet);

/*
    arch_atomic_cmpxchg_relaxed
    arch_atomic_cmpxchg_acquire
    arch_atomic_cmpxchg_release
    arch_atomic_cmpxchg

    arch_atomic64_cmpxchg_relaxed
    arch_atomic64_cmpxchg_acquire
    arch_atomic64_cmpxchg_release
    arch_atomic64_cmpxchg
*/
int Atomic64CmpXchgTestCase(s64 old, s64 new, s64 preVal, s64 wantRet);
int AtomicCmpXchgTestCase(int old, int new, int preVal, int wantRet);

/*
    arch_atomic_dec_if_positive
    arch_atomic64_dec_if_positive
*/
int Atomic64DecIfPositiveTestCase(s64 preVal, s64 wantRet);
int AtomicDecIfPositiveTestCase(int preVal, int wantRet);

/*
    arch_atomic_fetch_add_unless
    arch_atomic64_fetch_add_unless
*/
int Atomic64AddUnlessTestCase(s64 val, s64 preVal, s64 lessVal, s64 wantRet);
int AtomicAddUnlessTestCase(int val, int preVal, int lessVal, int wantRet);

/*
    arch_atomic_fetch_and_relaxed
    arch_atomic_fetch_and
    arch_atomic64_fetch_and_relaxed
    arch_atomic64_fetch_and
*/
int Atomic64AndTestCase(s64 val, s64 preVal, s64 wantRet);
int AtomicAndTestCase(int val, int preVal, int wantRet);

/*
    arch_atomic_fetch_or_relaxed
    arch_atomic_fetch_or
    arch_atomic64_fetch_or_relaxed
    arch_atomic64_fetch_or
*/
int Atomic64OrTestCase(s64 val, s64 preVal, s64 wantRet);
int AtomicOrTestCase(int val, int preVal, int wantRet);

/*
    arch_atomic_fetch_xor_relaxed
    arch_atomic_fetch_xor
    arch_atomic64_fetch_xor_relaxed
    arch_atomic64_fetch_xor
*/
int Atomic64XorTestCase(s64 val, s64 preVal, s64 wantRet);
int AtomicXorTestCase(int val, int preVal, int wantRet);

/*
    arch_atomic64_fetch_sub
    arch_atomic64_fetch_sub_relaxed
    arch_atomic64_sub_return_relaxed
    arch_atomic64_sub_return

    arch_atomic_sub_return_relaxed
    arch_atomic_sub_return
    arch_atomic_fetch_sub_relaxed
    arch_atomic_fetch_sub
*/
int Atomic64SubTestCase(s64 val, s64 preVal, s64 wantRet);
int AtomicSubTestCase(int val, int preVal, int wantRet);

/*
    arch_atomic64_add_return_relaxed
    arch_atomic64_add_return
    arch_atomic64_fetch_add_relaxed
    arch_atomic64_fetch_add

    arch_atomic_add_return_relaxed
    arch_atomic_add_return
    arch_atomic_fetch_add_relaxed
    arch_atomic_fetch_add
*/
int Atomic64AddTestCase(s64 val, s64 preVal, s64 wantRet);
int AtomicAddTestCase(int val, int preVal, int wantRet);
#endif