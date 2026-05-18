from bkgen import *

def func(t):
    if t[-1] < 0x09:
        return 1
    return 0
g = Bkgen()




g.addRule(
    20, [
    RAND(INT64_MIN, -(1<<60)),
    0
])

g.addRule(
    5, [
    RAND(0, 0x08),
    1
])

g.addRule(
    20, [
    0x09,
    0
])


g.complete()
