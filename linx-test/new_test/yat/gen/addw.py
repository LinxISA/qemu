from bkgen import *

def add(t):
    temp = lambda t: (t[-1] + t[-2]) & UINT32_MAX
    if temp(t)>>31 == 1:
        temp = temp(t) | (UINT32_MAX<<32)
    else:
        temp = temp(t) & UINT64_MAX
    return temp

g = Bkgen(commutative=True)

g.comment("random")

g.comment("UINT64_MAX取反的值为:" )
g.comment(~UINT64_MAX)

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
