from bkgen import *

g = Bkgen()

def div(t):
    q = abs(t[-2]) // abs(t[-1])
    return q if t[-2] * t[-1] >= 0 else -q

g.comment("0/random")
g.addRule(
    5, [
    0,
    RAND64,
    0
])

g.comment("both operand is random")
g.addRule(
    5, [
    RAND64,
    RAND(1, INT64_MAX),
    div
])
g.addRule(
    5, [
    RAND64,
    RAND(INT64_MIN, -1),
    div
])
g.addRule(
    5, [
    RAND64,
    RAND(1, 1<<36),
    div
])
g.addRule(
    5, [
    RAND64,
    RAND(-(1<<36), -1),
    div
])
g.addRule(
    5, [
    RAND64,
    RAND(1, 28),
    div
])
g.addRule(
    5, [
    RAND64,
    RAND(-28, -1),
    div
])

g.comment("both operand is max")
g.addRule(
    5, [
    UINT64_MAX,
    UINT64_MAX,
    div
])

g.comment("random/0")
g.addRule(
    3, [
    RAND64,
    0,
    -1
])
g.addRule(
    2, [
    0,
    0,
    -1
])

g.comment("result overflowed")
g.addRule(
    5, [
    INT64_MIN,
    -1,
    INT64_MIN
])

g.complete()
