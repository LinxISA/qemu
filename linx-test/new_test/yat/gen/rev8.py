
from bkgen import *

def rev8(t):
    r = 0
    a = t[-1]
    for i in range(8):
        r = r << 8
        b = a & 0xff
        r = r | b
        a = a >> 8
    return r

g = Bkgen()

g.comment("random")
g.addRule(
    10, [
    URAND64,
    rev8
])

g.comment("0")
g.addRule(
    2, [
    0,
    rev8
])

g.comment("MAX")
g.addRule(
    2, [
    UINT64_MAX,
    rev8
])

g.complete()
