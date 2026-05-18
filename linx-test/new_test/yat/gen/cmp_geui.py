from bkgen import *

def func(t):
    if t[-1] < 0x07:
        return 0
    return 1
g = Bkgen()




g.addRule(
    20, [
    RAND(INT64_MIN, -(1<<60)),
    1
])

g.addRule(
    5, [
    RAND(0, 0x06),
    0
])

g.addRule(
    20, [
    0x07,
    1
])


g.complete()
