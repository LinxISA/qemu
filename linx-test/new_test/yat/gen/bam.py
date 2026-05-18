from bkgen import *

def fun(t):
    l = t[-2]
    r = t[-1]
    m = (r>>58)&0x3f
    n = ((r>>52)&0x3f)+1
    if m == 0:
        return (l & ((1 << 64 - n) - 1 << n)) | (r & (1 << n) - 1)
    if (m+n<64):
        high = l&(((1<<64-m-n)-1)<<(m+n))
    else:
        high = 0
    low = l&((1<<m)-1)
    mid = r&((1<<n)-1)

    ret = (high|(mid<<m)|low)&UINT64_MAX
    return ret

g = Bkgen()

g.addRule(
    5, [
    URAND64,
    lambda t: ((random.randint(0, 1<<6 - 1) << 58) \
    | (random.randint(0,51) << 52) | random.randint(0, 1<<52 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: (0 | (random.randint(0,51) << 52) | random.randint(0, 1<<52 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: ((random.randint(0, 1<<6 - 1) << 58) \
    | 0 | random.randint(0, 1<<52 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: (random.randint(0, 1<<52 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: ((random.randint(0, 1<<6 - 1) << 58) \
    | (51 << 52) | random.randint(0, 1<<52 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: (0 | (51 << 52) \
    | random.randint(0, 1<<52 - 1)),
    fun
])

g.comment("random")
g.addRule(
    1, [
    0xd3ff9b4a62e819b3,
    0x2ea43191bba3566e,
    fun
])

g.complete()
