from bkgen import *

def func(t):
    sign_bit = (t[-2]>>31)%2
    temp = ((t[-2] & UINT32_MAX) >> (t[-1]&0x1f))
    if sign_bit==1 and (t[-1]&0x1f)!=0:
        temp = (temp|(UINT64_MAX<<(64-t[-1]&0x1f)))&UINT64_MAX
    if sign_bit==1 and (t[-1]&0x1f)==0:
        temp = temp | (UINT32_MAX<<32)
    return temp


g = Bkgen()

g.comment("random")

g.comment("both operands are random number")
g.addRule(
    10, [
    URAND64,
    URAND64,
    func
])
g.comment("one operand is 0")
g.addRule(
    5, [
    0,
    URAND64,
    func
])
g.addRule(
    5, [
    URAND64,
    0,
    func
])

g.comment("one operand is MAX")
g.addRule(
    5, [
    URAND64,
    UINT64_MAX,
    func
])

g.addRule(
    5, [
    UINT64_MAX,
    URAND64,
    func
])

g.comment("both operands are 0")
g.addRule(
    5, [
    0,
    0,
    func
])

g.complete()
