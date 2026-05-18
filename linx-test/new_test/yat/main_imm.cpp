#include <cstdio>

#include "number.h"

#define FUNC_DECL(op, imm) extern uint64_t INST_IMM(op, imm)(uint64_t);

#define reportSummary(op, ac, tot) \
printf("[\033[33mSummary\033[0m]  \
%s: %d/%d accepted\n", #op, ac, tot)

#define reportVerbose_5(op, s1, s2, e, r) \
    printf("[\033[32mAccepted\033[0m] \
    %s: 0x%-16lx, [IMM] 0x%-16lx, 0x%-16lx, \
    0x%-16lx\n", #op, uint64_t(s1), uint64_t(s2), e, r)
#define reportMismatch_5(op, s1, s2, e, r) \
    printf("[\033[31mMismatch\033[0m] \
    %s: 0x%-16lx, [IMM] 0x%-16lx, \
    0x%-16lx, 0x%-16lx\n", #op, \
    uint64_t(s1), uint64_t(s2), e, r)

#define __getMacro(_0, _1, _2, _3, _4, h, ...) h

#define reportMismatch(...) __getMacro\
(__VA_ARGS__, reportMismatch_5, reportMismatch_4)(__VA_ARGS__)

#if TEST_VERBOSE
#define reportVerbose(...) __getMacro\
(__VA_ARGS__, reportVerbose_5, reportVerbose_4)(__VA_ARGS__)
#else /* TEST_VERBOSE */
#define reportVerbose(...) ((void *)0)
#endif /* TEST_VERBOSE */

#define _test(op, cfun, arg, imm, nmm)                   \
    do {                                                 \
        res = INST_IMM(op, nmm)(arg);                    \
        expected = yat::Number<cfun(arg, imm)>::Value;   \
        ++total;                                         \
        if (expected == res) {                           \
            ++accepted;                                  \
            reportVerbose(op, arg, imm, expected, res);  \
        } else {                                         \
            reportMismatch(op, arg, imm, expected, res); \
        }                                                \
    } while (0)

#define _INST_IMM(op, imm) op##_##imm
#define INST_IMM(op, imm) _INST_IMM(op, imm)


extern "C" {
#include DEC_FILE
}

int main(int argc, char **argv)
{
    uint64_t res, expected, total = 0, accepted = 0;
#include ACT_FILE
    reportSummary(INST_NAME, accepted, total);
    return 0;
}
