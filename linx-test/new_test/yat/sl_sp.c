#include <stdio.h>
#include <stdint.h>
#include "c_prototype.h"
#define reportSummary(op, ac, tot) \
printf("[\033[33mSummary\033[0m]  %s: %d/%d accepted\n", op, ac, tot)

#define reportVerbose_5(op, s1, s2, e, r) \
    printf("[\033[32mAccepted\033[0m] \
    %s: %16lx, %16lx, %16lx, %16lx\n", op, s1, s2, e, r)
#define reportMismatch_5(op, s1, s2, e, r) \
    printf("[\033[31mMismatch\033[0m] \
    %s: %16lx, %16lx, %16lx, %16lx\n", op, s1, s2, e, r)
#define reportVerbose_4(op, s, e, r) \
printf("[\033[32mAccepted\033[0m] %s: %16lx, %16lx, %16lx\n", op, s, e, r)
#define reportMismatch_4(op, s, e, r) \
printf("[\033[31mMismatch\033[0m] %s: %16lx, %16lx, %16lx\n", op, s, e, r)

#define __getMacro(_0, _1, _2, _3, _4, h, ...) h

#define reportMismatch(...) __getMacro\
(__VA_ARGS__, reportMismatch_5, reportMismatch_4)(__VA_ARGS__)



#if TEST_VERBOSE
#define reportVerbose(...) __getMacro\
(__VA_ARGS__, reportVerbose_5, reportVerbose_4)(__VA_ARGS__)
#else /* TEST_VERBOSE */
#define reportVerbose(...) ((void *)0)
#endif /* TEST_VERBOSE */

int main()
{
    int temp = 0;
    int res = blk_sw_lw_sp(15);
    if (res == 1) {
        temp++;
        reportVerbose("blk_sw_lw_sp", 0, 0, 1, res);
    } else {
        reportMismatch("blk_sw_lw_sp", 0, 0, 1, res);
    }
    reportSummary("blk_sw_lw_sp", temp, 1);
    temp = 0;
    res = blk_sd_ld_sp(15);
    if (res == 1) {
        temp++;
        reportVerbose("blk_sw_lw_sp", 0, 0, 1, res);
    } else {
        reportMismatch("blk_sd_ld_sp", 0, 0, 1, res);
    }
    reportSummary("blk_sd_ld_sp", temp, 1);

    return 0;
}
