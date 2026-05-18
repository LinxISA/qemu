from bkgen import *

def setc_geui(t):
    if t[-1] >= 1:
        return 16
    return 1

g = Bkgen()


g.addRule(
    20, [
    RAND(0, UINT64_MAX),
    setc_geui
])

g.addRule(
    20, [
    RAND(INT64_MIN, -(1<<60)),
    16
])

g.addRule(
    20, [
    1,
    16
])




g.complete()
