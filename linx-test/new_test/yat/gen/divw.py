from bkgen import *

g = Bkgen()

def divw(t):
    a = t[-2] & UINT32_MAX
    b = t[-1] & UINT32_MAX
    s = 1
    if a & (1 << 31):
        a = (1 << 32) - a
        s *= -1
    if b & (1 << 31):
        b = (1 << 32) - b
        s *= -1
    q = a // b
    return q if s == 1 or q == 0 else (1 << 64) - q

g.comment("0/random")
g.addRule(
    5, [
    0,
    URAND64,
    0
])

g.comment("+/+")
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, 1<<10),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, 10),
    divw
])

g.comment("+/-")
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(INT32_MAX-(1<<10), INT32_MAX) | (1 << 31),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(INT32_MAX-10, INT32_MAX) | (1 << 31),
    divw
])

g.comment("-/+")
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, 1<<10),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, 10),
    divw
])

g.comment("-/-")
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(INT32_MAX-(1<<10), INT32_MAX) | (1 << 31),
    divw
])
g.addRule(
    3, [
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(1, INT32_MAX) | (1 << 31),
    lambda t: random.randint(0, UINT32_MAX)\
    << 32 | random.randint(INT32_MAX-10, INT32_MAX) | (1 << 31),
    divw
])

g.comment("both operand is max")
g.addRule(
    5, [
    lambda t: random.randint(0, UINT64_MAX) | (1 << 32) - 1,
    lambda t: random.randint(0, UINT64_MAX) | (1 << 32) - 1,
    1
])

g.comment("random/0")
g.addRule(
    2, [
    URAND64,
    0,
    -1
])

g.comment("0/0")
g.addRule(
    2, [
    0,
    0,
    -1
])

g.comment("overflow: max/(-1)")
g.addRule(
    5, [
    lambda t: (random.randint(0, UINT32_MAX) << 32) | 0x80000000,
    lambda t: (random.randint(0, UINT32_MAX) << 32) | 0xffffffff,
    0xffffffff80000000
])

g.complete()
