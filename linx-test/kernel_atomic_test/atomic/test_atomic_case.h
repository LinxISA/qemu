#include "test_typdef.h"

#ifndef __TEST_ATOMIC_CASE__
#define __TEST_ATOMIC_CASE__

#define LOOP_DEFAULT   100000
#define THREAD_2 2
#define THREAD_3 3
#define int64 s64

#define ATOMIC_TEST_FETCH_FUNC_CASE1(asm_type, prefix, ops1, ops2, lrtype, value1, value2, loop)                            \
static                                                                                                                      \
int atomic##asm_type##_##prefix##_##ops1##_##ops2##lrtype##_test(void* arg)                                                 \
{                                                                                                                           \
    thread_arg* current = (thread_arg*)arg;                                                                                 \
    int times = loop;                                                                                                       \
    int ret;                                                                                                                \
    while (times--) {                                                                                                       \
        if (current->pthread_index != 0) {                                                                                  \
            ret = arch_atomic##asm_type##_##prefix##_##ops1##lrtype(value1, &current->atomic->atomic##asm_type);            \
        } else {                                                                                                            \
            ret = arch_atomic##asm_type##_##prefix##_##ops2##lrtype(value2, &current->atomic->atomic##asm_type);            \
        }                                                                                                                   \
    }                                                                                                                       \
    return 0;                                                                                                               \
}                                                                                                                           \

#define ATOMIC_TEST_RETURN_FUNC_CASE1(asm_type, ops1, ops2, afterfix, lrtype, value1, value2, loop)                         \
static                                                                                                                      \
int atomic##asm_type##_##ops1##_##ops2##_##afterfix##lrtype##_test(void* arg)                                               \
{                                                                                                                           \
    thread_arg* current = (thread_arg*)arg;                                                                                 \
    int times = loop;                                                                                                       \
    int ret;                                                                                                                \
    while (times--) {                                                                                                       \
        if (current->pthread_index != 0) {                                                                                  \
            ret = arch_atomic##asm_type##_##ops1##_##afterfix##lrtype(value1, &current->atomic->atomic##asm_type);          \
        } else {                                                                                                            \
            ret = arch_atomic##asm_type##_##ops2##_##afterfix##lrtype(value2, &current->atomic->atomic##asm_type);          \
        }                                                                                                                   \
    }                                                                                                                       \
    return 0;                                                                                                               \
}                                                                                                                           \

#define ATOMIC_TEST_RETURN_FUNC_CASE2(asm_type, ops1, ops2, lrtype, value1, value2, loop)                                   \
static                                                                                                                      \
int atomic##asm_type##_##ops1##_##ops2##lrtype##_test(void* arg)                                                            \
{                                                                                                                           \
    thread_arg* current = (thread_arg*)arg;                                                                                 \
    int times = loop;                                                                                                       \
    int ret;                                                                                                                \
    while (times--) {                                                                                                       \
        if (current->pthread_index != 0) {                                                                                  \
            ret = arch_atomic##asm_type##_##ops1##lrtype(&current->atomic->atomic##asm_type, value1);                       \
        } else if (current->pthread_index == 1) {                                                                           \
            ret = arch_atomic##asm_type##_##ops2##lrtype(&current->atomic->atomic##asm_type, value2);                       \
        } else {                                                                                                            \
            int##asm_type tmp = arch_atomic##asm_type##_read(&(current->atomic->atomic##asm_type));                         \
            if ((tmp != value1) && (tmp != value2) && (tmp != 0)) {                                                         \
                    LOG_RAW("Invaild %ld[0x%lx] != %ld[0x%lx] | %ld[0x%lx]", tmp,                                           \
                                                                             tmp,                                           \
                                                                             value1,                                        \
                                                                             value1,                                        \
                                                                             value2,                                        \
                                                                             value2);                                       \
                    return -1;                                                                                              \
            }                                                                                                               \
        }                                                                                                                   \
    }                                                                                                                       \
    return 0;                                                                                                               \
}                                                                                                                           \

#define ATOMIC_TEST_RETURN_FUNC_CASE3(asm_type, ops1, ops2, lrtype, value1, value2, loop)                                   \
static                                                                                                                      \
int atomic##asm_type##_##ops1##_##ops2##lrtype##_test(void* arg)                                                            \
{                                                                                                                           \
    thread_arg* current = (thread_arg*)arg;                                                                                 \
    int times = loop;                                                                                                       \
    int ret;                                                                                                                \
    while (times--) {                                                                                                       \
        if (current->pthread_index != 0) {                                                                                  \
            ret = arch_atomic##asm_type##_##ops1##lrtype(&current->atomic->atomic##asm_type, value2, value1);               \
        } else if (current->pthread_index == 1) {                                                                           \
            ret = arch_atomic##asm_type##_##ops2##lrtype(&current->atomic->atomic##asm_type, value1, value2);               \
        } else {                                                                                                            \
            int##asm_type tmp = arch_atomic##asm_type##_read(&(current->atomic->atomic##asm_type));                         \
            if ((tmp != value1) && (tmp != value2) && (tmp != 0)) {                                                         \
                    LOG_RAW("Invaild %ld[0x%lx] != %ld[0x%lx] | %ld[0x%lx]", tmp,                                           \
                                                                             tmp,                                           \
                                                                             value1,                                        \
                                                                             value1,                                        \
                                                                             value2,                                        \
                                                                             value2);                                       \
                    return -1;                                                                                              \
            }                                                                                                               \
        }                                                                                                                   \
    }                                                                                                                       \
    return 0;                                                                                                               \
}                                                                                                                           \

#define ATOMIC_TEST_RETURN_FUNC_CASE4(asm_type, ops1, ops2, value1, value2, loop)                                           \
static                                                                                                                      \
int atomic##asm_type##_##ops1##_##ops2##_test(void* arg)                                                                    \
{                                                                                                                           \
    thread_arg* current = (thread_arg*)arg;                                                                                 \
    int times = loop;                                                                                                       \
    int ret;                                                                                                                \
    while (times--) {                                                                                                       \
        if (current->pthread_index != 0) {                                                                                  \
            ret = arch_atomic##asm_type##_##ops1##_if_positive(&current->atomic->atomic##asm_type);                         \
        } else if (current->pthread_index == 1) {                                                                           \
            ret = arch_atomic##asm_type##_fetch_##ops2##_unless(&current->atomic->atomic##asm_type, value1, value2);        \
        } else {                                                                                                            \
            int##asm_type tmp = arch_atomic##asm_type##_read(&(current->atomic->atomic##asm_type));                         \
            if ((tmp < 0) || (tmp > value2) || ((tmp % value1) != 0)) {                                                     \
                    LOG_RAW("Invaild %ld[0x%lx] != 0 + %ld[0x%lx] < %ld[0x%lx]", tmp,                                       \
                                                                                 tmp,                                       \
                                                                                 value1,                                    \
                                                                                 value1,                                    \
                                                                                 value2,                                    \
                                                                                 value2);                                   \
                    return -1;                                                                                              \
            }                                                                                                               \
        }                                                                                                                   \
    }                                                                                                                       \
    return 0;                                                                                                               \
}                                                                                                                           \

#define ATOMIC_TEST_RETURN_FUNC_CASE5(asm_type, ops1, ops2, lrtype, value1, value2, loop)                                   \
static                                                                                                                      \
int atomic##asm_type##_##ops1##_##ops2##lrtype##_test(void* arg)                                                            \
{                                                                                                                           \
    thread_arg* current = (thread_arg*)arg;                                                                                 \
    int times = loop;                                                                                                       \
    int ret;                                                                                                                \
    while (times--) {                                                                                                       \
        if (current->pthread_index != 0) {                                                                                  \
            ret = arch_atomic##asm_type##_##ops1##lrtype(&current->atomic->atomic##asm_type, value1);                       \
        } else if (current->pthread_index == 1) {                                                                           \
            ret = arch_atomic##asm_type##_##ops2##lrtype(&current->atomic->atomic##asm_type, value1, value2);               \
        } else {                                                                                                            \
            int##asm_type tmp = arch_atomic##asm_type##_read(&(current->atomic->atomic##asm_type));                         \
            if ((tmp != value1) && (tmp != value2) && (tmp != 0)) {                                                         \
                    LOG_RAW("Invaild %ld[0x%lx] != %ld[0x%lx] | %ld[0x%lx]", tmp,                                           \
                                                                             tmp,                                           \
                                                                             value1,                                        \
                                                                             value1,                                        \
                                                                             value2,                                        \
                                                                             value2);                                       \
                    return -1;                                                                                              \
            }                                                                                                               \
        }                                                                                                                   \
    }                                                                                                                       \
    return 0;                                                                                                               \
}                                                                                                                           \

ATOMIC_TEST_RETURN_FUNC_CASE4( , dec, add, 1, 10, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE4(64 , dec, add, 1, 0x0F00000000000000, LOOP_DEFAULT);

ATOMIC_TEST_RETURN_FUNC_CASE5( , xchg, cmpxchg, _relaxed, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE5( , xchg, cmpxchg, _acquire, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE5( , xchg, cmpxchg, _release, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE5( , xchg, cmpxchg, , 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE5(64 , xchg, cmpxchg, _relaxed, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE5(64 , xchg, cmpxchg, _acquire, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE5(64 , xchg, cmpxchg, _release, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE5(64 , xchg, cmpxchg, , 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);

ATOMIC_TEST_RETURN_FUNC_CASE3( , cmpxchg, cmpxchg, _relaxed, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE3( , cmpxchg, cmpxchg, _acquire, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE3( , cmpxchg, cmpxchg, _release, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE3( , cmpxchg, cmpxchg, , 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE3(64 , cmpxchg, cmpxchg, _relaxed, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE3(64 , cmpxchg, cmpxchg, _acquire, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE3(64 , cmpxchg, cmpxchg, _release, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE3(64 , cmpxchg, cmpxchg, , 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);

ATOMIC_TEST_RETURN_FUNC_CASE2( , xchg, xchg, _relaxed, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE2( , xchg, xchg, _acquire, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE2( , xchg, xchg, _release, 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE2( , xchg, xchg, , 0x5A, 0xA5, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE2(64 , xchg, xchg, _relaxed, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE2(64 , xchg, xchg, _acquire, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE2(64 , xchg, xchg, _release, 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);
ATOMIC_TEST_RETURN_FUNC_CASE2(64 , xchg, xchg, , 0x5A00000000000000, 0xA500000000000000, LOOP_DEFAULT);

ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, sub, sub, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, sub, add, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, add, add, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, sub, sub, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, sub, add, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, add, add, , 1, 1, LOOP_DEFAULT)

ATOMIC_TEST_RETURN_FUNC_CASE1( , sub, sub, return, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1( , sub, add, return, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1( , add, add, return, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1( , sub, sub, return, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1( , sub, add, return, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1( , add, add, return, , 1, 1, LOOP_DEFAULT)

ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, sub, sub, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, sub, add, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, add, add, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, sub, sub, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, sub, add, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, add, add, , 1, 1, LOOP_DEFAULT)

ATOMIC_TEST_RETURN_FUNC_CASE1(64 , sub, sub, return, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1(64 , sub, add, return, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1(64 , add, add, return, _relaxed, 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1(64 , sub, sub, return, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1(64 , sub, add, return, , 1, 1, LOOP_DEFAULT)
ATOMIC_TEST_RETURN_FUNC_CASE1(64 , add, add, return, , 1, 1, LOOP_DEFAULT)

ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, or, or, _relaxed, 0x5A00, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, or, xor, _relaxed, 0x5A00, 0x00, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, or, and, _relaxed, 0x5A, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, xor, xor, _relaxed, 0x5A, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, xor, and, _relaxed, 0x5A, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, and, and, _relaxed, 0x5A, 0xA5, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, or, or, , 0x5A00, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, or, xor, , 0x5A00, 0x00, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, or, and, , 0x5A, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, xor, xor, , 0x5A, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, xor, and, , 0x5A, 0x5A, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1( , fetch, and, and, , 0x5A, 0xA5, LOOP_DEFAULT)

ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, or, or, _relaxed, 0x5A00000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, or, xor, _relaxed, 0x5A00000000000000, 0x0000000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, or, and, _relaxed, 0x5A000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, xor, xor, _relaxed, 0x5A000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, xor, and, _relaxed, 0x5A000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, and, and, _relaxed, 0x5A000000000000, 0xA5000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, or, or, , 0x5A00000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, or, xor, , 0x5A00000000000000, 0x0000000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, or, and, , 0x5A000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, xor, xor, , 0x5A000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, xor, and, , 0x5A000000000000, 0x5A000000000000, LOOP_DEFAULT)
ATOMIC_TEST_FETCH_FUNC_CASE1(64 , fetch, and, and, , 0x5A000000000000, 0xA5000000000000, LOOP_DEFAULT)

testcase_t g_test_case[] = {
    // add_unless dec_if_positive
    {"Atomic-decadd-Relaxed-Case", THREAD_3, atomic_dec_add_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic64-decadd-Relaxed-Case", THREAD_3, atomic64_dec_add_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},

    // xchg cmpxchg
    {"Atomic-xchgcmpxchg-Relaxed-Case", THREAD_3, atomic_xchg_cmpxchg_relaxed_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-xchgcmpxchg-Acquire-Case", THREAD_3, atomic_xchg_cmpxchg_acquire_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-xchgcmpxchg-Release-Case", THREAD_3, atomic_xchg_cmpxchg_release_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-xchgcmpxchg-Case", THREAD_3, atomic_xchg_cmpxchg_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic64-xchgcmpxchg-Relaxed-Case", THREAD_3, atomic64_xchg_cmpxchg_relaxed_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-xchgcmpxchg-Acquire-Case", THREAD_3, atomic64_xchg_cmpxchg_acquire_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-xchgcmpxchg-Release-Case", THREAD_3, atomic64_xchg_cmpxchg_release_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-xchgcmpxchg-Case", THREAD_3, atomic64_xchg_cmpxchg_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},

    {"Atomic-cmpxchgcmpxchg-Relaxed-Case", THREAD_3, atomic_cmpxchg_cmpxchg_relaxed_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-cmpxchgcmpxchg-Acquire-Case", THREAD_3, atomic_cmpxchg_cmpxchg_acquire_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-cmpxchgcmpxchg-Release-Case", THREAD_3, atomic_cmpxchg_cmpxchg_release_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-cmpxchgcmpxchg-Case", THREAD_3, atomic_cmpxchg_cmpxchg_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic64-cmpxchgcmpxchg-Relaxed-Case", THREAD_3, atomic64_cmpxchg_cmpxchg_relaxed_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-cmpxchgcmpxchg-Acquire-Case", THREAD_3, atomic64_cmpxchg_cmpxchg_acquire_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-cmpxchgcmpxchg-Release-Case", THREAD_3, atomic64_cmpxchg_cmpxchg_release_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-cmpxchgcmpxchg-Case", THREAD_3, atomic64_cmpxchg_cmpxchg_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},

    {"Atomic-xchgxchg-Relaxed-Case", THREAD_3, atomic_xchg_xchg_relaxed_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-xchgxchg-Acquire-Case", THREAD_3, atomic_xchg_xchg_acquire_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-xchgxchg-Release-Case", THREAD_3, atomic_xchg_xchg_release_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-xchgxchg-Case", THREAD_3, atomic_xchg_xchg_test, NOT_USED, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic64-xchgxchg-Relaxed-Case", THREAD_3, atomic64_xchg_xchg_relaxed_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-xchgxchg-Acquire-Case", THREAD_3, atomic64_xchg_xchg_acquire_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-xchgxchg-Release-Case", THREAD_3, atomic64_xchg_xchg_release_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-xchgxchg-Case", THREAD_3, atomic64_xchg_xchg_test, NOT_USED, NOT_USED, ATOMIC_TEST_DWORD},

    // or xor and
    {"Atomic-OrOr-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_or_or_relaxed_test, 0x5A5A, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-OrXor-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_or_xor_relaxed_test, 0x5A00, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-OrAnd-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_or_and_relaxed_test, 0x5A, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-XorXor-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_xor_xor_relaxed_test, 0x00, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-XorAnd-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_xor_and_relaxed_test, 0x00, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-AndAnd-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_and_and_relaxed_test, 0x00, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic64-OrOr-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_or_or_relaxed_test, 0x5A5A000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-OrXor-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_or_xor_relaxed_test, 0x5A00000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-OrAnd-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_or_and_relaxed_test, 0x5A000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-XorXor-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_xor_xor_relaxed_test, 0x00000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-XorAnd-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_xor_and_relaxed_test, 0x00000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-AndAnd-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_and_and_relaxed_test, 0x00, NOT_USED, ATOMIC_TEST_DWORD},

    {"Atomic-OrOr-Fetch-Case", THREAD_2, atomic_fetch_or_or_test, 0x5A5A, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-OrXor-Fetch-Case", THREAD_2, atomic_fetch_or_xor_test, 0x5A00, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-OrAnd-Fetch-Case", THREAD_2, atomic_fetch_or_and_test, 0x5A, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-XorXor-Fetch-Case", THREAD_2, atomic_fetch_xor_xor_test, 0x00, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-XorAnd-Fetch-Case", THREAD_2, atomic_fetch_xor_and_test, 0x00, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-AndAnd-Fetch-Case", THREAD_2, atomic_fetch_and_and_test, 0x00, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic64-OrOr-Fetch-Case", THREAD_2, atomic64_fetch_or_or_test, 0x5A5A000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-OrXor-Fetch-Case", THREAD_2, atomic64_fetch_or_xor_test, 0x5A00000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-OrAnd-Fetch-Case", THREAD_2, atomic64_fetch_or_and_test, 0x5A000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-XorXor-Fetch-Case", THREAD_2, atomic64_fetch_xor_xor_test, 0x00000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-XorAnd-Fetch-Case", THREAD_2, atomic64_fetch_xor_and_test, 0x00000000000000, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-AndAnd-Fetch-Case", THREAD_2, atomic64_fetch_and_and_test, 0x00000000000000, NOT_USED, ATOMIC_TEST_DWORD},

    // Sub Add
    {"Atomic64-SubAdd-Return-Relaxed-Case", THREAD_2, atomic64_sub_add_return_relaxed_test, 0, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-Sub-Return-Relaxed-Case", THREAD_2, atomic64_sub_sub_return_relaxed_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-Add-Return-Relaxed-Case", THREAD_2, atomic64_add_add_return_relaxed_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},

    {"Atomic64-SubAdd-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_sub_add_relaxed_test, 0, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-Sub-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_sub_sub_relaxed_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic-Add-Fetch-Relaxed-Case", THREAD_2, atomic64_fetch_add_add_relaxed_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},

    {"Atomic64-SubAdd-Return-Case", THREAD_2, atomic64_sub_add_return_test, 0, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-Sub-Return-Case", THREAD_2, atomic64_sub_sub_return_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-Add-Return-Case", THREAD_2, atomic64_add_add_return_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},

    {"Atomic64-SubAdd-Fetch-Case", THREAD_2, atomic64_fetch_sub_add_test, 0, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-Sub-Fetch-Case", THREAD_2, atomic64_fetch_sub_sub_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},
    {"Atomic64-Add-Fetch-Case", THREAD_2, atomic64_fetch_add_add_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_DWORD},

    {"Atomic-SubAdd-Return-Relaxed-Case", THREAD_2, atomic_sub_add_return_relaxed_test, 0, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Sub-Return-Relaxed-Case", THREAD_2, atomic_sub_sub_return_relaxed_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Add-Return-Relaxed-Case", THREAD_2, atomic_add_add_return_relaxed_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic-SubAdd-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_sub_add_relaxed_test, 0, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Sub-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_sub_sub_relaxed_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Add-Fetch-Relaxed-Case", THREAD_2, atomic_fetch_add_add_relaxed_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic-SubAdd-Return-Case", THREAD_2, atomic_sub_add_return_test, 0, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Sub-Return-Case", THREAD_2, atomic_sub_sub_return_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Add-Return-Case", THREAD_2, atomic_add_add_return_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},

    {"Atomic-SubAdd-Fetch-Case", THREAD_2, atomic_fetch_sub_add_test, 0, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Sub-Fetch-Case", THREAD_2, atomic_fetch_sub_sub_test, 0 - THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},
    {"Atomic-Add-Fetch-Case", THREAD_2, atomic_fetch_add_add_test, THREAD_2 * LOOP_DEFAULT, NOT_USED, ATOMIC_TEST_WORD},
};

#endif
