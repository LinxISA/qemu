from bkgen import *

sext = lambda x: (x | 0xffffffff00000000 if x & 0x80000000 else x) & UINT64_MAX
ag_sxtd = lambda t: t[-2] + (sext(t[-1] & UINT32_MAX) << 3) & UINT64_MAX

g = Bkgen()

g.addRule(
    5, [
    URAND64,
    URAND64,
    ag_sxtd
])
g.addRule(
    5, [
    0,
    URAND64,
    ag_sxtd
])
g.addRule(
    5, [
    URAND64,
    0,
    ag_sxtd
])
g.addRule(
    5, [
    0,
    0,
    ag_sxtd
])
g.addRule(
    5, [
    URAND64,
    UINT64_MAX,
    ag_sxtd
])
g.addRule(
    5, [
    UINT64_MAX,
    URAND64,
    ag_sxtd
])
g.addRule(
    5, [
    UINT64_MAX,
    UINT64_MAX,
    ag_sxtd
])

g.complete()
