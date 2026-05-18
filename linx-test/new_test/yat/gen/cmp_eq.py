from bkgen import *

def cmp_eq(t):
    if t[-1] == t[-2]:
        return 1
    return 0
g = Bkgen(commutative=True)

g.comment("equal")
g.addRule(
    20, [
    URAND64,
    "t[-1]",
    1
])

g.comment("not_equal")
g.addRule(
    20, [
    URAND64,
    URAND64,
    cmp_eq
])



g.complete()
