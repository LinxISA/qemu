from bkgen import *

def func(t):
    if t[-1] < 0x07:
        return 1
    return 0
g = Bkgen()


g.addRule(
    20, [
    RAND(INT64_MIN, 0x07),
    1
])



g.addRule(
    20, [
    RAND(0x07, INT64_MAX),
    0
])

g.addRule(
    20, [
    0x07,
    0
])


g.complete()
