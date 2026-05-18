from bkgen import *

def func(t):
    temp = lambda t: (t[-1] | t[-2]) & UINT32_MAX
    if temp(t)>>31 == 1:
        temp = temp(t) | (UINT32_MAX<<32)
    else:
        temp = temp(t) & UINT64_MAX
    return temp

g = Bkgen()

g.comment("random")

g.comment("both operands are random number")
g.addRule(
    10, [
    URAND64,
    URAND64,
    func
])
g.comment("one operand is 0")
g.addRule(
    5, [
    0,
    URAND64,
    func
])
g.addRule(
    5, [
    URAND64,
    0,
    func
])

g.comment("one operand is MAX")
g.addRule(
    5, [
    URAND64,
    UINT64_MAX,
    func
])

g.addRule(
    5, [
    UINT64_MAX,
    URAND64,
    func
])

g.comment("both operands are 0")
g.addRule(
    5, [
    0,
    0,
    func
])

g.complete()
