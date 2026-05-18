from bkgen import *

g = Bkgen() #不可交换的话括号里的删掉

sext128 = lambda x: x if (x & 1 << 64) else x | (UINT64_MAX << 64)

g.comment("one operand is 0")
g.addRule(
    4, [
    0,
    URAND64,
    0
])

g.addRule(
    4, [
    RAND64,
    0,
    0
])

g.comment("both operand is random")
g.addRule(
    5, [
    URAND64,
    URAND64,
    lambda t: (((t[-1]*t[-2])>>64)\
    if(((1<<63)&t[-2])==0)\
    else((t[-1]*(t[-2]|(UINT64_MAX << 64)))>>64))&UINT64_MAX
])

g.addRule(
    5, [
    -1,
    1000,
    lambda t: (((t[-1]*t[-2])>>64)\
    if(((1<<63)&t[-2])==0)\
    else((t[-1]*(t[-2]|(UINT64_MAX << 64)))>>64))&UINT64_MAX
])

g.comment("min result")
g.addRule(
    1, [
    INT64_MIN,
    URAND64,
    lambda t: (((t[-1]*t[-2])>>64)\
    if(((1<<63)&t[-2])==0)\
    else((t[-1]*(t[-2]|(UINT64_MAX << 64)))>>64))&UINT64_MAX
])

g.comment("result is MAX")
g.addRule(
    1, [
    INT64_MAX,
    UINT64_MAX,
    lambda t: (((t[-1]*t[-2])>>64)\
    if(((1<<63)&t[-2])==0)\
    else((t[-1]*(t[-2]|(UINT64_MAX << 64)))>>64))&UINT64_MAX
])

g.complete()
