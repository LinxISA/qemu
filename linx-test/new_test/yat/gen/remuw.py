from bkgen import *

g = Bkgen()

sext32 = lambda x: x & 0x7fffffff \
if (x & 0x80000000 == 0) else x | 0xffffffff00000000

g.comment("0/random")
g.addRule(
    5, [
    0,
    URAND64,
    lambda t: sext32((t[-2]&UINT32_MAX)%(t[-1]&UINT32_MAX))
])

g.comment("both operand is random")
g.addRule(
    2, [
    URAND64,
    RAND(0, 1 << 64),
    lambda t: sext32((t[-2]&UINT32_MAX)%(t[-1]&UINT32_MAX))
])

g.comment("both operand is max")
g.addRule(
    1, [
    UINT64_MAX,
    UINT64_MAX,
    lambda t: sext32((t[-2]&UINT32_MAX)%(t[-1]&UINT32_MAX))
])

g.comment("random/0")
g.addRule(
    2, [
    URAND64,
    0,
    lambda t: sext32(t[-2])
])

g.complete()
