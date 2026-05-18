
from bkgen import *

def rev32(t):
    r = 0
    a = t[-1]
    for i in range(2):
        r = r << 32
        b = a & 0xffffffff
        r = r | b
        a = a >> 32
    return r

g = Bkgen()

g.comment("random")
g.addRule(
    10, [
    URAND64,
    rev32
])

g.comment("0")
g.addRule(
    2, [
    0,
    rev32
])

g.comment("MAX")
g.addRule(
    2, [
    UINT64_MAX,
    rev32
])

g.complete()
