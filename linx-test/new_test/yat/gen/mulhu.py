from bkgen import *

g = Bkgen(commutative=True)

g.comment("one operand is 0")
g.addRule(
    2, [
    0,
    URAND64,
    lambda t: (t[-1] * t[-2])>>64
])

g.comment("both operand is random32")
g.addRule(
    5, [
    URAND32,
    URAND32,
    lambda t: (t[-1] * t[-2])>>64
])

g.comment("random")
g.addRule(
    5, [
    URAND64,
    URAND64,
    lambda t: (t[-1] * t[-2])>>64
])

g.comment("min result")
g.addRule(
    3, [
    UINT32_MAX,
    UINT32_MAX,
    lambda t: (t[-1] * t[-2])>>64
])

g.comment("result is MAX")
t = 32
a = 1
while t > 0:
    a *= (1 << t) + 1
    b = (1 << t) - 1
    g.addData([a, b, 0])
    g.addData([b, a, 0])
    t //= 2

g.comment("result overflowed")
g.addRule(
    5, [
    UINT64_MAX,
    UINT64_MAX,
    lambda t: (t[-1] * t[-2])>>64
])

g.complete()
