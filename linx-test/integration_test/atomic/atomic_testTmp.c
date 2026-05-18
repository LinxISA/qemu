#include "atomic_testTmp.h"

int Atomic64AddTestCase(s64 val, s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_add(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atomic64.counter, ret);

    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_add_relaxed(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_add_return(val, &atomic64);
    if (ret64 != atomic64.counter || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :return val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicAddTestCase(int val, int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_fetch_add(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atomic.counter, ret);

    atomic.counter = preVal;
    ret = arch_atomic_fetch_add_relaxed(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_add_return(val, &atomic);
    if (ret != atomic.counter || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :return val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_add_return_relaxed(val, &atomic);
    if (ret != atomic.counter || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :return val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64SubTestCase(s64 val, s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_sub(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] - atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atatomic64omic.counter, ret);

    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_sub_relaxed(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] - atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_sub_return(val, &atomic64);
    if (ret64 != atomic64.counter || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :return val[%ld] - atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_sub_return_relaxed(val, &atomic64);
    if (ret64 != atomic64.counter || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :return val[%ld] - atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicSubTestCase(int val, int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_fetch_sub(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] - atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atomic.counter, ret);

    atomic.counter = preVal;
    ret = arch_atomic_fetch_sub_relaxed(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] - atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_sub_return(val, &atomic);
    if (ret != atomic.counter || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :return val[%ld] - atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_sub_return_relaxed(val, &atomic);
    if (ret != atomic.counter || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :return val[%ld] - atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64XorTestCase(s64 val, s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_xor(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] XOR atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atatomic64omic.counter, ret);

    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_xor_relaxed(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] XOR atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicXorTestCase(int val, int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_fetch_xor(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] XOR atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atomic.counter, ret);

    atomic.counter = preVal;
    ret = arch_atomic_fetch_xor_relaxed(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] XOR atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64OrTestCase(s64 val, s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_or(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] OR atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atatomic64omic.counter, ret);

    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_or_relaxed(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] OR atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicOrTestCase(int val, int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_fetch_or(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] OR atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atomic.counter, ret);

    atomic.counter = preVal;
    ret = arch_atomic_fetch_or_relaxed(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] OR atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64AndTestCase(s64 val, s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_and(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] AND atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic64[%ld] = atomic64[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atatomic64omic.counter, ret);

    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_and_relaxed(val, &atomic64);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] AND atomic64[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicAndTestCase(int val, int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_fetch_and(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] AND atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    // qemu_debug_printf("%s : val[%ld] + atomic[%ld] = atomic[%ld] : ret[%ld]------>PASS.", __func__, val, preVal, atomic.counter, ret);

    atomic.counter = preVal;
    ret = arch_atomic_fetch_and_relaxed(val, &atomic);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] AND atomic[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64AddUnlessTestCase(s64 val, s64 preVal, s64 lessVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_fetch_add_unless(&atomic64, val, lessVal);

    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] + atomic64[%ld] less[%ld] = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, lessVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicAddUnlessTestCase(int val, int preVal, int lessVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_fetch_add_unless(&atomic, val, lessVal);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld :fetch val[%ld] + atomic[%ld] less[%ld] = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, lessVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64DecIfPositiveTestCase(s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;
    s64 ra = preVal - 1;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_dec_if_positive(&atomic64);
    if (ret64 != ra || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : atomic64[%ld] - 1 = atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicDecIfPositiveTestCase(int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;
    int ra = preVal - 1;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_dec_if_positive(&atomic);
    if (ret != ra || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : atomic[%ld] - 1 = atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64CmpXchgTestCase(s64 old, s64 new, s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_cmpxchg(&atomic64, old, new);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_cmpxchg_relaxed(&atomic64, old, new);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_cmpxchg_acquire(&atomic64, old, new);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_cmpxchg_release(&atomic64, old, new);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicCmpXchgTestCase(int old, int new, int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_cmpxchg(&atomic, old, new);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_cmpxchg_relaxed(&atomic, old, new);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_cmpxchg_acquire(&atomic, old, new);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_cmpxchg_release(&atomic, old, new);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : old[%ld] new[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, old, new, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}

int Atomic64XchgTestCase(s64 val, s64 preVal, s64 wantRet)
{
    atomic64_t atomic64;
    s64 ret64;

    memset(&atomic64, 0, sizeof(atomic64_t));
    atomic64.counter = preVal;
    ret64 = arch_atomic64_xchg(&atomic64, val);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_xchg_relaxed(&atomic64, val);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_xchg_acquire(&atomic64, val);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }

    atomic64.counter = preVal;
    ret64 = arch_atomic64_xchg_release(&atomic64, val);
    if (ret64 != preVal || atomic64.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic64[%ld] => atomic64[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic64.counter, ret64);
        return -1;
    }
    return 0;
}

int AtomicXchgTestCase(int val, int preVal, int wantRet)
{
    atomic_t atomic;
    int ret;

    memset(&atomic, 0, sizeof(atomic_t));
    atomic.counter = preVal;
    ret = arch_atomic_xchg(&atomic, val);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_xchg_relaxed(&atomic, val);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_xchg_acquire(&atomic, val);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }

    atomic.counter = preVal;
    ret = arch_atomic_xchg_release(&atomic, val);
    if (ret != preVal || atomic.counter != wantRet) {
        qemu_debug_printf("%s %ld : val[%ld] atomic[%ld] => atomic[%ld] : ret[%ld]------>NG.", __func__, __LINE__, val, preVal, atomic.counter, ret);
        return -1;
    }
    return 0;
}