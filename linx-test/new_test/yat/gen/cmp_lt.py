from bkgen import *

def func(t):
    if t[-1] < t[-2]:
        return 1
    return 0
g = Bkgen()


g.addRule(
    20, [
    RAND(INT64_MIN, INT64_MAX),
    RAND("t[-1] + 1" , INT64_MAX) ,
    1
])

g.addRule(
    20, [
    RAND(INT64_MIN, -(1<<60)),
    RAND(0,100),
    1
])

g.addRule(
    20, [
    RAND(INT64_MIN, INT64_MAX),
    RAND(INT64_MIN, "t[-1] -1"),
    0
])

g.addRule(
    20, [
    RAND(INT64_MIN, INT64_MAX),
    "t[-1]",
    0
])


g.complete()
