from bkgen import *
#include "block_type.h"
def func(t):
    if t[-1] == 0x07:
        return 0
    return 1
g = Bkgen()

g.comment("equal")
g.addRule(
    20, [
    0x07,
    0
])

g.comment("not_equal")
g.addRule(
    20, [
    URAND64,
    func
])



g.complete()
