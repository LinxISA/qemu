from bkgen import *

def mulh(t):
    a = t[-2]
    b = t[-1]
    s = 1
    if a < 0:
        a = -a
        s *= -1
    if b < 0:
        b = -b
        s *= -1
    r = a * b
    if s == -1:
        r = (1 << 128) - r
    return r >> 64

g = Bkgen(commutative=True)

g.comment("one operand is 0")
g.addRule(
    2, [
    0,
    RAND64,
    0
])

g.comment("random low32bit")
g.addRule(
    5, [
    RAND32,
    RAND32,
    lambda t: -1 if t[-2] < 0 and t[-1] > 0 or t[-1] < 0 and t[-2] > 0 else 0
])

g.comment("+ * +")
g.addRule(
    5, [
    RAND(1, INT64_MAX),
    RAND(1, INT64_MAX),
    lambda t: t[-2] * t[-1] >> 64 & UINT64_MAX
])

g.comment("+ * -")
g.addRule(
    5, [
    RAND(INT64_MIN, -1),
    RAND(1, INT64_MAX),
    mulh
])

g.comment("- * -")
g.addRule(
    5, [
    RAND(INT64_MIN, -1),
    RAND(INT64_MIN, -1),
    lambda t: t[-2] * t[-1] >> 64 & UINT64_MAX
])

g.comment("result is 0")
t = 32
a = 1
while t > 1:
    a *= (1 << t) + 1
    b = (1 << t) - 1
    g.addData([a, b, 0])
    g.addData([b, a, 0])
    t //= 2

g.comment("max * max")
g.addRule(
    5, [
    INT64_MAX,
    INT64_MAX,
    INT64_MAX * INT64_MAX >> 64
])

g.complete()
