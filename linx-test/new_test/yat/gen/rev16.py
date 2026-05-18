
from bkgen import *

def rev16(t):
    r = 0
    a = t[-1]
    for i in range(4):
        r = r << 16
        b = a & 0xffff
        r = r | b
        a = a >> 16
    return r

g = Bkgen()

g.comment("random")
g.addRule(
    10, [
    URAND64,
    rev16
])

g.comment("0")
g.addRule(
    2, [
    0,
    rev16
])

g.comment("MAX")
g.addRule(
    2, [
    UINT64_MAX,
    rev16
])

g.complete()
