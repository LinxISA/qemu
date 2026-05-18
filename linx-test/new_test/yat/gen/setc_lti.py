from bkgen import *


g = Bkgen()


g.addRule(
    20, [
    RAND(INT64_MIN, 0x01),
    16
])

g.addRule(
    20, [
    RAND(INT64_MIN, -(1<<60)),
    16
])


g.addRule(
    20, [
    1,
    1
])

g.addRule(
    20, [
    RAND(1, INT64_MAX),
    1
])


g.complete()
