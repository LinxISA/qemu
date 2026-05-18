from bkgen import *

g = Bkgen()

g.comment("0/random")
g.addRule(
    5, [
    0,
    URAND64,
    lambda t: (t[-2] // t[-1])
])

g.comment("both operand is random")
g.addRule(
    3, [
    URAND64,
    lambda t: random.randint(1, UINT64_MAX) | 1 << 63,
    lambda t: (t[-2] // t[-1])
])
g.addRule(
    3, [
    URAND64,
    RAND(1, INT64_MAX),
    lambda t: (t[-2] // t[-1])
])
g.addRule(
    3, [
    URAND64,
    RAND(1, 1<<36),
    lambda t: (t[-2] // t[-1])
])
g.addRule(
    3, [
    URAND64,
    RAND(1, 36),
    lambda t: (t[-2] // t[-1])
])

g.comment("both operand is max")
g.addRule(
    5, [
    UINT64_MAX,
    UINT64_MAX,
    lambda t: (t[-2] // t[-1])
])

g.comment("random/0")
g.addRule(
    3, [
    URAND64,
    0,
    UINT64_MAX
])
g.addRule(
    2, [
    0,
    0,
    UINT64_MAX
])

g.complete()
