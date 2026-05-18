from bkgen import *

def func(t):
    return  t[-1] & 0xFFFFFFFF
g = Bkgen()

g.comment("equal")
g.addRule(
    50, [
    URAND64,
    func
])


g.complete()
