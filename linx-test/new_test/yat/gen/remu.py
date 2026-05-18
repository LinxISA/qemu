from bkgen import *

g = Bkgen()

g.comment("0/random")
g.addRule(
    5, [
    0,
    URAND64,
    lambda t: (t[-2] % t[-1])
])

g.comment("both operand is random")
g.addRule(
    2, [
    URAND64,
    URAND64,
    lambda t: (t[-2] % t[-1])
])

g.comment("both operand is max")
g.addRule(
    1, [
    UINT64_MAX,
    UINT64_MAX,
    lambda t: (t[-2] % t[-1])
])

g.comment("random/0")
g.addRule(
    2, [
    URAND64,
    0,
    "t[-2]"
])

g.complete()
