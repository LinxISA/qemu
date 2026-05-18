#include <stdio.h>
#include <stdint.h>
#include "c_prototype.h"
#include <stdlib.h>
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

int compare(uint64_t *src, uint64_t *des, int n)
{
    if (!src || !des) {
        return 0;
    }
    int flag = 1;
    for (int i = 0; i < n; i++) {
        if (*src != *des) {
            flag = 0;
            break;
        }
        src++;
        des++;
    }
    return flag;
}

void test_mempush(void)
{
    uint64_t src[2] = {10, 20};
    uint64_t src16[16] = {0, 0, 0, 13, 13, 13, 13, 13, 15, 15,
    15, 15, 15, 15, 15, 15};
    uint64_t des[2], des2[2], des16[16];
    blk_mempush(src[0], src[1], des);
    if (compare(src, des, 2) == 1) {
        reportSummary("blk_mempush", 1, 1);
    }
    blk_mempush_cover(des2, src[1]);
    if (compare(src + 1, des2 + 1, 1) == 1) {
        reportSummary("blk_mempush_cover", 1, 1);
    }
    blk_mempush16(des16);
    if (compare(src16 + 3, des16 + 3, 13) == 1) {
        reportSummary("blk_mempush14", 1, 1);
    }
}

void test_mempop(void)
{
    uint64_t src[2] = {30, 40};
    uint64_t src16[16];
    uint64_t des[2], des2[2], des16[16];
    for (int i = 0; i <= 15; i++) {
        src16[i] = i;
    }
    blk_mempop(src);
    blk_mempp(des);
    if (compare(src, des, 2) == 1) {
        reportSummary("blk_mempop", 1, 1);
    }
    blk_mempop_cover(src);
    blk_mempp_cover(des2);
    if (compare(src + 1, des2 + 1, 1) == 1) {
        reportSummary("blk_mempop_cover", 1, 1);
    }
    blk_mempop14(src16);
    blk_mempp14(des16);
    if (compare(src16 + 1, des16 + 3, 13) == 1) {
        reportSummary("blk_mempop14", 1, 1);
    }
}

void test_fentry(void){
    /*
     * To test the function of reading six 64-bit
     * random numbers from the register and writing them to the memory;
     * The immediate number is the 12-bit signed maximum value;
     */
    volatile uint64_t temp = 0;
    const uint64_t right = 1;
    uint64_t source_fentry[16];
    uint64_t des_fentry[16];
    for (int i = 0; i < 6; i++) {
        source_fentry[i] = (rand() << 32) + rand();
    }
    uint64_t addr_fentry = des_fentry;
    addr_fentry += 48;
    blk_fentry(source_fentry[5], source_fentry[4],
        source_fentry[3], source_fentry[2], source_fentry[1],
        source_fentry[0], addr_fentry);
    uint64_t res = compare(source_fentry, des_fentry, 6);
    if (res == right) {
        reportVerbose("blk_fentry", 6, right, res);
        temp++;
    } else {
        reportMismatch("blk_fentry", 6, right, res);
    }
    /*
     * To test the function of reading seven 64-bit random numbers
     * rom the register and writing them to memory;
     * he immediate number is the 12-bit signed minimum value;
     * he destination address register is duplicate with the written register.
     */
    addr_fentry = des_fentry;
    addr_fentry += 56;
    source_fentry[6] = addr_fentry + 2048;
    blk_fentry_1(addr_fentry, source_fentry[5], source_fentry[4],
        source_fentry[3], source_fentry[2], source_fentry[1], source_fentry[0]);
    res = compare(source_fentry, des_fentry, 7);
    if (res == right) {
        reportVerbose("blk_fentry", 7, right, res);
        temp++;
    } else {
        reportMismatch("blk_fentry", 7, right, res);
    }
    /*
     * To test the function of reading sixteen 64-bit random numbers
     * from the memory and writing them to regist;
     */
    for (int i = 0; i < 13; i++) {
        source_fentry[i] = i;
    }
    addr_fentry = des_fentry;
    addr_fentry += 128;
    blk_fentry_2(addr_fentry);
    res = compare(source_fentry, des_fentry, 13);
    if (res == right) {
        reportVerbose("blk_fentry", 16, right, res);
        temp++;
    } else {
        reportMismatch("blk_fentry", 16, right, res);
    }
    reportSummary("blk_fentry", temp, 3);
}

void test_fexit(void)
{
    /*
     * To test the function of reading seven 64-bit random numbers
     * from the memory and writing them to register;
     * The immediate number is the 12-bit signed maximum value;
     */
    uint64_t temp = 0;
    const uint64_t right = 1;
    uint64_t source_fexit[16];
    uint64_t des_fexit[16];
    for (int i = 0; i < 7; i++) {
        source_fexit[i] = (rand() << 32) + rand();
    }
    uint64_t addr_fexit = source_fexit;
    addr_fexit -= 1984;
    uint64_t addr1_fexit = des_fexit;
    addr1_fexit += 56;
    blk_fexit(addr_fexit);
    blk_fexit_entry(addr1_fexit);
    source_fexit[6] += 8;
    uint64_t res = compare(source_fexit, des_fexit, 7);
    if (res == right) {
        reportVerbose("blk_fexit", 7, right, res);
        temp++;
    } else {
        reportMismatch("blk_fexit", 7, right, res);
    }
    /*
     * To test the function of reading seven 64-bit random numbers \
     * from the memory and writing them to register;
     * The immediate number is the 12-bit signed minimum value;
     */
    for (int i = 0; i < 7; i++) {
        source_fexit[i] = (rand() << 32) + rand();
    }
    addr_fexit = source_fexit;
    addr_fexit += 2104;
    addr1_fexit = des_fexit;
    addr1_fexit += 56;
    blk_fexit_1(addr_fexit);
    blk_fexit_1_entry(addr1_fexit);
    source_fexit[6] += 8;
    res = compare(source_fexit, des_fexit, 7);
    if (res == right) {
        reportVerbose("blk_fexit", 7, right, res);
        temp++;
    } else {
        reportMismatch("blk_fexit", 7, right, res);
    }
    reportSummary("blk_fexit", temp, 2);
}

void test_ftexit(void)
{
    /*
     * To test the function of reading seven 64-bit random \
     * numbers from the memory and writing them to register;
     * The immediate number is the 12-bit signed maximum value;
     */
    uint64_t temp = 0;
    const uint64_t right = 1;
    uint64_t source_ftexit[16];
    uint64_t des_ftexit[16];
    for (int i = 0; i < 7; i++) {
        source_ftexit[i] = (rand() << 32) + rand();
    }
    uint64_t addr_fexit = source_ftexit;
    uint64_t addr1_fexit = des_ftexit;
    addr1_fexit += 56;
    addr_fexit -= 1984;
    blk_ftexit(addr_fexit);
    blk_ftexit_fentry(addr1_fexit);
    uint64_t res = compare(source_ftexit, des_ftexit, 7);
    if (res == right) {
        reportVerbose("blk_ftexit", 2, right, res);
        temp++;
    } else {
        reportMismatch("blk_ftexit", 2, right, res);
    }
    /*
     * To test the function of reading seven 64-bit random numbers \
     * from the memory and writing them to register;
     * The immediate number is the 12-bit signed minimum value;
     */
    for (int i = 0; i < 7; i++) {
        source_ftexit[i] = (rand() << 32) + rand();
    }
    addr_fexit = source_ftexit;
    addr_fexit += 2104;
    addr1_fexit = des_ftexit;
    addr1_fexit += 56;
    blk_ftexit_1(addr_fexit);
    blk_ftexit_1_fentry(addr1_fexit);
    res = compare(source_ftexit, des_ftexit, 7);
    if (res == right) {
        reportVerbose("blk_ftexit", 2, right, res);
        temp++;
    } else {
        reportMismatch("blk_ftexit", 2, right, res);
    }
    reportSummary("blk_ftexit", temp, 2);
}

int main()
{
    test_mempush();
    test_mempop();
    test_fentry();
    test_fexit();
    test_ftexit();
    return 0;
}
