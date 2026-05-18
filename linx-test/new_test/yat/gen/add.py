from bkgen import *

add = lambda t: (t[-1] + t[-2]) & UINT64_MAX

g = Bkgen(commutative=True)

g.comment("random")

g.addRule(
    10, [
    URAND64,
    URAND64,
    add
])
g.comment("one operand is 0")
g.addRule(
    5, [
    0,
    URAND64,
    add
])

g.comment("one operand is MAX")
g.addRule(
    5, [
    UINT64_MAX,
    URAND64,
    add
])

g.comment("0 + 0")
g.addRule(
    5, [
    0,
    0,
    add
])

g.comment("MAX + MAX")
g.addRule(
    5, [
    UINT64_MAX,
    UINT64_MAX,
    add
])

g.complete()
