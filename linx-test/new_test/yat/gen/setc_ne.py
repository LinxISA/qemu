from bkgen import *
#include "block_type.h"
def func(t):
    if t[-1] == t[-2]:
        return 1
    return 16
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
    func
])



g.complete()
