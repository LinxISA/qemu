from bkgen import *

def func(t):
    if t[-1] & 0x80000000 != 0:
        return ( t[-1] & 0xFFFFFFFF )+ 0XFFFFFFFF00000000
    else :
        return t[-1] & 0xFFFFFFFF
g = Bkgen()


g.addRule(
    50, [
    URAND64,
    func
])


g.complete()
