from bkgen import *
#include "block_type.h"
def func(t):
    if t[-1] == 1:
        return 1
    return 16
g = Bkgen()

g.comment("equal")
g.addRule(
    20, [
    URAND64,
    func
])

g.comment("not_equal")
g.addRule(
    20, [
    1,
    1
])



g.complete()
