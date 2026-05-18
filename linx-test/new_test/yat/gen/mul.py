from bkgen import *

g = Bkgen(commutative=True)

g.comment("no overflow")
g.addRule(
    10, [
    lambda t: random.randint(1, 1 << random.randint(1, 63)),
    lambda t: random.randint(t[-1], UINT64_MAX) // t[-1],
    lambda t: (t[-1] * t[-2]) & UINT64_MAX
])

g.comment("one operand is 0")
g.addRule(
    2, [
    0,
    URAND32,
    lambda t: (t[-1] * t[-2]) & UINT64_MAX
])

g.comment("result is max")
t = 32
a = 1
while t > 0:
    a *= (1 << t) + 1
    b = (1 << t) - 1
    g.addData([a, b, UINT64_MAX])
    g.addData([b, a, UINT64_MAX])
    t //= 2

g.comment("result overflowed")
g.addRule(
    10, [
    RAND(UINT32_MAX, UINT64_MAX),
    RAND(UINT32_MAX, UINT64_MAX),
    lambda t: (t[-1] * t[-2]) & UINT64_MAX
])

g.comment("0 * 0")
g.addRule(
    3, [
    0,
    0,
    0
])

g.comment("MAX * MAX")
g.addRule(
    3, [
    UINT64_MAX,
    UINT64_MAX,
    lambda t: (t[-1] * t[-2]) & UINT64_MAX
])

g.complete()
