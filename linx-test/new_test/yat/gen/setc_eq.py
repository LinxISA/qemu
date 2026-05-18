from bkgen import *

def setc_eq(t):
    if t[-1] == t[-2]:
        return 16
    return 1
g = Bkgen(commutative=True)

g.comment("equal")
g.addRule(
    20, [
    URAND64,
    "t[-1]",
    16
])

g.comment("not_equal")
g.addRule(
    20, [
    URAND64,
    URAND64,
    setc_eq
])



g.complete()
