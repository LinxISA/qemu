from bkgen import *

g = Bkgen()

sext32 = lambda x: x | (1 << 32) - 1 << 32 if x & 1 << 31 else x

g.comment("0/random")
g.addRule(
    5, [
    0,
    URAND64,
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])

g.comment("both operand is random")
g.addRule(
    3, [
    URAND64,
    RAND(1, UINT64_MAX),
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])
g.addRule(
    3, [
    URAND64,
    RAND(1, UINT64_MAX),
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])
g.addRule(
    3, [
    URAND64,
    lambda t: random.randint(1, 1<<10) | random.randint(0, UINT32_MAX) << 32,
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])
g.addRule(
    3, [
    URAND64,
    lambda t: random.randint(UINT32_MAX - (1<<10), UINT32_MAX)\
    | random.randint(0, UINT32_MAX) << 32,
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])
g.addRule(
    3, [
    URAND64,
    lambda t: random.randint(1, 10) | random.randint(0, UINT32_MAX) << 32,
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])
g.addRule(
    3, [
    URAND64,
    lambda t: random.randint(UINT32_MAX - 10, UINT32_MAX)\
    | random.randint(0, UINT32_MAX) << 32,
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])

g.comment("both operand is max")
g.addRule(
    5, [
    lambda t: UINT64_MAX ^ (random.randint(0, UINT32_MAX) << 32),
    lambda t: UINT64_MAX ^ (random.randint(0, UINT32_MAX) << 32),
    lambda t: sext32((t[-2]&UINT32_MAX)//(t[-1]&UINT32_MAX))
])

g.comment("random/0")
g.addRule(
    2, [
    URAND64,
    0,
    UINT64_MAX
])

g.complete()
