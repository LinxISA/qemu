#include "atomic_testTmp.h"

int main(int argc, char **argv)
{
    /*
        arg0: val
        arg1: atomic.counter
        arg2: after atomic.counter

        arg2 = arg1 + arg0
    */
    Atomic64AddTestCase(5, 0, 5);
    Atomic64AddTestCase(6, 0, 6);
    Atomic64AddTestCase(6, -1, 5);
    Atomic64AddTestCase(0x7000000000000000, 16, 0x7000000000000010);
    Atomic64AddTestCase(0x6000000000000000, 0, 0x6000000000000000);
    Atomic64AddTestCase(0x7FFFFFFFFFFFFFFF, 1, 0x8000000000000000);
    Atomic64AddTestCase(0xFFFFFFFFFFFFFFFF, 1, 0x0);

    AtomicAddTestCase(5, 0, 5);
    AtomicAddTestCase(6, 0, 6);
    AtomicAddTestCase(6, -1, 5);
    AtomicAddTestCase(0x7FFFFFFF, 1, 0x80000000);
    AtomicAddTestCase(0x7000000000000000, 16, 16);
    AtomicAddTestCase(0xFFFFFFFFFFFFFFFF, 1, 0);

    /*
        arg0: val
        arg1: atomic.counter
        arg2: after atomic.counter

        arg2 = arg1 - arg0
    */
    Atomic64SubTestCase(5, 0, -5);
    Atomic64SubTestCase(5, 5, 0);
    Atomic64SubTestCase(5, 15, 10);
    Atomic64SubTestCase(1, 0x7000000000000000, 0x6FFFFFFFFFFFFFFF);
    Atomic64SubTestCase(1, 0x8000000000000000, 0x7FFFFFFFFFFFFFFF);
    Atomic64SubTestCase(1, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE);
    Atomic64SubTestCase(1, 0x0000000000000000, 0xFFFFFFFFFFFFFFFF);

    AtomicSubTestCase(5, 0, -5);
    AtomicSubTestCase(5, 5, 0);
    AtomicSubTestCase(5, 15, 10);
    AtomicSubTestCase(1, 0x80000000, 0x7FFFFFFF);
    AtomicSubTestCase(1, 0xFFFFFFFF80000000, 0x7FFFFFFF);
    AtomicSubTestCase(1, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFE);
    AtomicSubTestCase(1, 0xFFFFFFFF00000001, 0x0);

    /*
        arg0: val
        arg1: atomic.counter
        arg2: after atomic.counter

        arg2 = arg0 XOR arg1
    */
    Atomic64XorTestCase(0xAA, 0x55, 0xFF);
    Atomic64XorTestCase(0xAA, 0x50, 0xFA);
    Atomic64XorTestCase(0xFFFFFFFFFFFFFFFF, 0x50, 0xFFFFFFFFFFFFFFAF);

    AtomicXorTestCase(0xAA, 0x55, 0xFF);
    AtomicXorTestCase(0xAA, 0x50, 0xFA);
    AtomicXorTestCase(0xAA, 0x50, 0xFA);
    AtomicXorTestCase(0xFFFFFFFFFFFFFFFF, 0x50, 0xFFFFFFAF);

    /*
        arg0: val
        arg1: atomic.counter
        arg2: after atomic.counter

        arg2 = arg0 OR arg1
    */
    Atomic64OrTestCase(0xAA, 0x55, 0xFF);
    Atomic64OrTestCase(0xAA, 0x50, 0xFA);
    Atomic64OrTestCase(0xFFFFFFFF00000000, 0x50, 0xFFFFFFFF00000050);

    AtomicOrTestCase(0xAA, 0x55, 0xFF);
    AtomicOrTestCase(0xAA, 0x50, 0xFA);
    AtomicOrTestCase(0xFFFFFFFF00000000, 0x50, 0x50);

    /*
        arg0: val
        arg1: atomic.counter
        arg2: after atomic.counter

        arg2 = arg0 AND arg1
    */
    Atomic64AndTestCase(0xAA, 0x55, 0x0);
    Atomic64AndTestCase(0xAA, 0x52, 0x2);
    Atomic64AndTestCase(0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF00000052, 0xFFFFFFFF00000052);

    AtomicAndTestCase(0xAA, 0x55, 0x0);
    AtomicAndTestCase(0xAA, 0x52, 0x2);
    AtomicAndTestCase(0xFFFFFFFFFFFFFFFF, 0x52, 0x52);

    /*
        arg0: val
        arg1: atomic.counter
        arg2: unless
        arg3: after atomic.counter

        if arg1 != arg2 then :
            arg1 = arg0 + arg1
    */
    Atomic64AddUnlessTestCase(5, 0, 0, 0);
    Atomic64AddUnlessTestCase(5, 0, 1, 5);
    Atomic64AddUnlessTestCase(0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF);
    Atomic64AddUnlessTestCase(0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0x1, 0xFFFFFFFFFFFFFFFE);
    Atomic64AddUnlessTestCase(0xFFFFFFFFFFFFFFFE, 0xFFFFFFFFFFFFFFFF, 0x1, 0xFFFFFFFFFFFFFFFD);

    AtomicAddUnlessTestCase(5, 0, 0, 0);
    AtomicAddUnlessTestCase(5, 0, 1, 5);
    AtomicAddUnlessTestCase(0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
    AtomicAddUnlessTestCase(0x1, 0xFFFFFFFFFFFFFFFF, 0x1, 0x0);
    AtomicAddUnlessTestCase(0xFFFFFFFFFFFFFFFE, 0xFFFFFFFFFFFFFFFF, 0x1, 0xFFFFFFFD);

    /*
        arg0: atomic.counter
        arg1: after atomic.counter

        if arg0 - 1 >= 0 then :
            arg0 = arg0 - 1
    */
    Atomic64DecIfPositiveTestCase(2, 1);
    Atomic64DecIfPositiveTestCase(1, 0);
    Atomic64DecIfPositiveTestCase(0, 0);
    Atomic64DecIfPositiveTestCase(-1, -1);

    AtomicDecIfPositiveTestCase(2, 1);
    AtomicDecIfPositiveTestCase(1, 0);
    AtomicDecIfPositiveTestCase(0, 0);
    AtomicDecIfPositiveTestCase(-1, -1);
    AtomicDecIfPositiveTestCase(0xFFFFFFFFFFFFFFFF, -1);
    AtomicDecIfPositiveTestCase(0xFFFFFFFF00000000, 0x0);
    AtomicDecIfPositiveTestCase(0xFFFFFFFF00000001, 0);
    AtomicDecIfPositiveTestCase(0xFFFFFFFF00000002, 1);

    /*
        arg0: old
        arg1: new
        arg2: atomic.counter
        arg3: after atomic.counter

        if arg2 == old then :
            arg3 = arg1
    */
    Atomic64CmpXchgTestCase(0, 1, 0, 1);
    Atomic64CmpXchgTestCase(0, 1, 2, 2);
    Atomic64CmpXchgTestCase(2, 1, 2, 1);
    Atomic64CmpXchgTestCase(0xFFFFFFFFFFFFFFFF, 1, 0xFFFFFFFFFFFFFFFF, 1);

    AtomicCmpXchgTestCase(0, 1, 0, 1);
    AtomicCmpXchgTestCase(0, 1, 2, 2);
    AtomicCmpXchgTestCase(2, 1, 2, 1);
    AtomicCmpXchgTestCase(0xFFFFFFFF00000001, 2, 1, 2);

    /*
        arg0: value
        arg1: atomic.counter
        arg2: after atomic.counter

        arg2 = arg0
    */
    Atomic64XchgTestCase(0, 1, 0);
    Atomic64XchgTestCase(5, 1, 5);
    Atomic64XchgTestCase(5, 2, 5);
    Atomic64XchgTestCase(0xFFFFFFFFFFFFFFFF, 2, 0xFFFFFFFFFFFFFFFF);

    AtomicXchgTestCase(0, 1, 0);
    AtomicXchgTestCase(5, 1, 5);
    AtomicXchgTestCase(5, 2, 5);
    AtomicXchgTestCase(0xFFFFFFFF00000001, 2, 1);
    return 0;
}
