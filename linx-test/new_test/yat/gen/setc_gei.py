from bkgen import *

def setc_gei(t):
    if t[-1] >= 1:
        return 16
    return 1
g = Bkgen()


g.addRule(
    20, [
    RAND(INT64_MIN, -1),
    1
])



g.addRule(
    40, [
    RAND(INT64_MIN, INT64_MAX),
    setc_gei
])



g.complete()
