from bkgen import *

ag_uxtd = lambda t: t[-2] + ((t[-1] & UINT32_MAX) << 3) & UINT64_MAX

g = Bkgen()

g.addRule(
    5, [
    URAND64,
    URAND64,
    ag_uxtd
])
g.addRule(
    5, [
    0,
    URAND64,
    ag_uxtd
])
g.addRule(
    5, [
    URAND64,
    0,
    ag_uxtd
])
g.addRule(
    5, [
    0,
    0,
    ag_uxtd
])
g.addRule(
    5, [
    URAND64,
    UINT64_MAX,
    ag_uxtd
])
g.addRule(
    5, [
    UINT64_MAX,
    URAND64,
    ag_uxtd
])
g.addRule(
    5, [
    UINT64_MAX,
    UINT64_MAX,
    ag_uxtd
])

g.complete()
