from bkgen import *

def fun(t):
    l = t[-2]
    r = t[-1]
    m = (r>>6)&0x3f
    n = (r&0x3f)+1
    if (m+n<=64):
        ret = ((l>>m)&((1<<n)-1))
    else:
        ret = (l>>m)
    return ret

def m_add_n_64(t):
    m = random.randint(0, 64)
    n = 64 - m
    return random.randint(0, 1 << 52 - 1) << 12 | m << 6 | n

g = Bkgen()

g.addRule(
    5, [
    URAND64,
    URAND64,
    fun
])

g.addRule(
    5, [
    URAND64,
    m_add_n_64,
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1)\
    << 12 | random.randint(0, 1 << 6 - 1) << 6 | 0,
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1)\
    << 12 | 0 | random.randint(0, 1 << 6 - 1),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1) << 12 | 0 | 0,
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1)\
    << 12 | random.randint(0, 1 << 6 - 1) << 6 | 63,
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1) << 12\
    | (63 << 6) | random.randint(0, 1 << 6 - 1),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1) << 12 | (63 << 6) | 63,
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1) << 12 | 0 | 63,
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: random.randint(0, 1 << 52 - 1) << 12 | (63 << 6) | 0,
    fun
])

g.addRule(
    1, [
    URAND64,
    0b11010011,
    fun
])

g.complete()
