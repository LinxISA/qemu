from bkgen import *


g = Bkgen()


g.addRule(
    20, [
    RAND(0, UINT64_MAX),
    RAND("t[-1] + 1" , UINT64_MAX) ,
    1
])

g.addRule(
    20, [
    RAND(INT64_MIN, -(1<<60)),
    RAND(0,100),
    16
])

g.addRule(
    20, [
    RAND(0, UINT64_MAX),
    RAND(0, "t[-1] -1"),
    16
])

g.addRule(
    20, [
    RAND(0, UINT64_MAX),
    "t[-1]",
    16
])


g.complete()
