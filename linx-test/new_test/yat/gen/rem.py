from bkgen import *

g = Bkgen()

def rem(t):
    a = abs(t[-2])
    b = abs(t[-1])
    q = a // b
    r = a - b * q
    return r if t[-2] >= 0 else (1 << 64) - r

g.addRule(
    5, [
    0,
    RAND64,
    0
])

g.addRule(
    10, [
    RAND64,
    RAND64,
    rem
])

g.addRule(
    1, [
    INT64_MAX,
    INT64_MAX,
    0
])

g.addRule(
    5, [
    RAND64,
    0,
    "t[-2]"
])

g.addRule(
    1, [
    INT64_MIN,
    -1,
    0
])

g.complete()
