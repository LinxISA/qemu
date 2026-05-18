from bkgen import *


g = Bkgen()


g.addRule(
    20, [
    RAND(INT64_MIN, -1),
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
    16
])

g.addRule(
    20, [
    RAND(INT64_MIN, INT64_MAX),
    "t[-1]",
    16
])


g.complete()
