from bkgen import *

def fun(t):
    l = t[-2]
    r = t[-1]
    flag = (r&0xfff)<<52
    n = (r&0x3f)+1
    data = (l&((1<<n)-1))
    ret = data|flag
    return ret

g = Bkgen()

g.addRule(
    5, [
    URAND64,
    lambda t: ((random.randint(0, 1<<52 - 1) << 12)\
    | (random.randint(0,51) << 6) | random.randint(0, 1<<6 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: (0 | (random.randint(0,51) << 6) | random.randint(0, 1<<6 - 1)),
    fun
])


g.addRule(
    5, [
    URAND64,
    lambda t: ((random.randint(0, 1<<52 - 1) << 12)\
    | 0 | random.randint(0, 1<<6 - 1)),
    fun
])
g.addRule(
    5, [
    URAND64,
    lambda t: (random.randint(0, 1<<6 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: ((random.randint(0, 1<<52 - 1) << 12)\
    | (51 << 6) | random.randint(0, 1<<6 - 1)),
    fun
])

g.addRule(
    5, [
    URAND64,
    lambda t: (0 | (51 << 6) | random.randint(0, 1<<6 - 1)),
    fun
])

g.addRule(
    1, [
    URAND64,
    0b11010011,
    fun
])

g.complete()
