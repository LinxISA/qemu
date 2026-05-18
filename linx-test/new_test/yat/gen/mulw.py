from bkgen import *

g = Bkgen(commutative=True) #不可交换的话括号里的删掉

g.comment("one operand is 0")
g.addRule(
    4, [
    0,
    URAND64,
    lambda t: ((t[-1] * t[-2])&UINT32_MAX)\
    if(((1<<31)&(t[-1]*t[-2]))==0)\
    else(((t[-1] * t[-2])&UINT32_MAX)|0xffffffff00000000)
])

g.comment("both operand is random")
g.addRule(
    5, [
    URAND64,
    URAND64,
    lambda t: ((t[-1]*t[-2])&UINT32_MAX)\
    if(((1<<31)&((t[-1]*t[-2])&UINT32_MAX))==0)\
    else(((t[-1] * t[-2])&UINT32_MAX)|0xffffffff00000000)
])

g.comment("(-1) * random")
g.addRule(
    5, [
    UINT64_MAX,
    URAND64,
    lambda t: ((t[-1]*t[-2])&UINT32_MAX)\
    if(((1<<31)&((t[-1]*t[-2])&UINT32_MAX))==0)\
    else(((t[-1] * t[-2])&UINT32_MAX)|0xffffffff00000000)
])

g.complete()
