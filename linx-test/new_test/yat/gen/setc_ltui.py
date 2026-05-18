from bkgen import *


g = Bkgen()


g.addRule(
    20, [
    0,
    16
])

g.addRule(
    20, [
    RAND(INT64_MIN, -(1<<60)),
    1
])

g.addRule(
    1, [
    1,
    1
])



g.complete()
