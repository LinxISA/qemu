from bkgen import *


g = Bkgen()


g.addRule(
    50, [
    URAND64,
    "t[-1]"
])


g.complete()
