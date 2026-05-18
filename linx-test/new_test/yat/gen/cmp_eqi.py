from bkgen import *

def cmp_eqi(t):
    if t[-1] == 0x07:
        return 1
    return 0
g = Bkgen()

g.comment("equal")
g.addRule(
    20, [
    0x07,
    1
])

g.comment("not_equal")
g.addRule(
    20, [
    URAND64,
    cmp_eqi
])



g.complete()
