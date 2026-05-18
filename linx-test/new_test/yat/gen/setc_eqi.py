from bkgen import *

def setc_eq(t):
    if t[-1] == 1:
        return 16
    return 1
g = Bkgen()

g.comment("equal")
g.addRule(
    20, [
    1,
    16
])

g.comment("not_equal")
g.addRule(
    20, [
    URAND64,
    setc_eq
])



g.complete()
