from bkgen import *

g = Bkgen()

def remw(t):
    a = t[-2] & UINT32_MAX
    b = t[-1] & UINT32_MAX
    s = 1
    if a & 1 << 31:
        a = (1 << 32) - a
        s = -1
    if b & 1 << 31:
        b = (1 << 32) - b
    q = a // b
    r = a - q * b
    return (1 << 64) - r if s == -1 else r

g.comment("0/random")
g.addRule(
    5, [
    0,
    URAND64,
    remw
])

g.comment("both operand is random")
g.addRule(
    5, [
    URAND64,
    URAND64,
    remw
])

g.comment("a %% a")
g.addRule(
    5, [
    URAND64,
    "t[-1]",
    0
])

g.comment("divisor is max")
g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, UINT32_MAX) << 32 | INT32_MAX,
    remw
])

g.comment("random/0")
g.addRule(
    5, [
    URAND64,
    0,
    lambda t: (t[-2]&UINT32_MAX)\
    if((((t[-2]&UINT32_MAX))&(1<<31))==0) \
    else(((t[-2]&UINT32_MAX))|0xffffffff00000000)

])

g.complete()
