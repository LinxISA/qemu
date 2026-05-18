#include <stdio.h>
#include <stdint.h>
#include "c_prototype.h"

#define reportSummary(op, ac, tot) \
printf("[\033[33mSummary\033[0m]  %s: %d/%d accepted\n", op, ac, tot)

#define reportVerbose_5(op, s1, s2, e, r) \
    printf("[\033[32mAccepted\033[0m] \
    %s: 0x%-16lx, 0x%-16lx, 0x%-16lx, 0x%-16lx\n", op, s1, s2, e, r)
#define reportMismatch_5(op, s1, s2, e, r) \
    printf("[\033[31mMismatch\033[0m] %s: \
    0x%-16lx, 0x%-16lx, 0x%-16lx, 0x%-16lx\n", op, s1, s2, e, r)
#define reportVerbose_4(op, s, e, r) \
printf("[\033[32mAccepted\033[0m] \
%s: 0x%-16lx, 0x%-16lx, 0x%-16lx\n", op, s, e, r)
#define reportMismatch_4(op, s, e, r) \
printf("[\033[31mMismatch\033[0m] \
%s: 0x%-16lx, 0x%-16lx, 0x%-16lx\n", op, s, e, r)

#define __getMacro(_0, _1, _2, _3, _4, h, ...) h

#define reportMismatch(...) __getMacro\
(__VA_ARGS__, reportMismatch_5, reportMismatch_4)(__VA_ARGS__)

#if TEST_VERBOSE
#define reportVerbose(...) __getMacro\
(__VA_ARGS__, reportVerbose_5, reportVerbose_4)(__VA_ARGS__)
#else /* TEST_VERBOSE */
#define reportVerbose(...) ((void *)0)
#endif /* TEST_VERBOSE */

#define sext8(x) (((x) & 0x80) ? (x) | 0xffffffffffffff00 : (x))
#define sext16(x) (((x) & 0x8000) ? (x) | 0xffffffffffff0000 : (x))
#define sext32(x) (((x) & 0x80000000) ? (x) | 0xffffffff00000000 : (x))

#define ext8(x) (x)
#define ext16(x) (x)
#define ext32(x) (x)
#define ext64(x) (x)

#define BIND(a, b) #a " + " #b

#define _test(STORE, LOAD, EXT)                                    \
    do {                                                           \
        int temp = 0;                                              \
        uint64_t e, r;                                             \
        for (int i = 0; i < 32; i++) {                             \
            STORE(data + i, i * 8 + 7);                            \
        }                                                          \
        for (int j = 0; j < 32; j++) {                             \
            e = EXT(j * 8 + 7), r = LOAD(data + j);                \
            if (e == r) {                                          \
                temp++;                                            \
                reportVerbose(BIND(STORE, LOAD), data + j, e, r);  \
            } else {                                               \
                reportMismatch(BIND(STORE, LOAD), data + j, e, r); \
            }                                                      \
        }                                                          \
        reportSummary(BIND(STORE, LOAD), temp, 32);                \
    } while (0)

int main()
{
    uint64_t data[32];
    int temp = 0;
    _test(blk_sb_t, blk_lb_t, sext8);
    _test(blk_sb_t, blk_lb_r, sext8);
    _test(blk_sb_t, blk_lbu_t, ext8);
    _test(blk_sb_t, blk_lbu_r, ext8);

    _test(blk_sh_t, blk_lh_r, sext16);
    _test(blk_sh_t, blk_lh_t, sext16);
    _test(blk_sh_t, blk_lhu_r, ext16);
    _test(blk_sh_t, blk_lhu_t, ext16);

    _test(blk_sw_bd, blk_lw_r, sext32);
    _test(blk_sw_db, blk_lw_t, sext32);
    _test(blk_sw_rr0, blk_lwu_r, ext32);
    _test(blk_sw_rr1, blk_lwu_t, ext32);
    _test(blk_sw_t, blk_lw_r, sext32);
    _test(blk_sw_bd, blk_lw_t, sext32);
    _test(blk_sw_db, blk_lwu_r, ext32);
    _test(blk_sw_rr0, blk_lwu_t, ext32);
    _test(blk_sw_rr1, blk_lw_r, sext32);
    _test(blk_sw_t, blk_lw_t, sext32);
    _test(blk_sw_bd, blk_lwu_r, ext32);
    _test(blk_sw_db, blk_lwu_t, ext32);
    _test(blk_sw_rr0, blk_lw_r, sext32);
    _test(blk_sw_rr1, blk_lw_t, sext32);
    _test(blk_sw_t, blk_lwu_r, ext32);
    _test(blk_sw_bd, blk_lwu_t, ext32);
    _test(blk_sw_db, blk_lw_r, sext32);
    _test(blk_sw_rr0, blk_lw_t, sext32);
    _test(blk_sw_rr1, blk_lwu_r, ext32);
    _test(blk_sw_t, blk_lwu_t, ext32);

    _test(blk_sd_bd, blk_ld_t, ext64);
    _test(blk_sd_db, blk_ld_r, ext64);
    _test(blk_sd_rr0, blk_ld_t, ext64);
    _test(blk_sd_rr1, blk_ld_r, ext64);
    _test(blk_sd_t, blk_ld_t, ext64);
    _test(blk_sd_bd, blk_ld_r, ext64);
    _test(blk_sd_db, blk_ld_t, ext64);
    _test(blk_sd_rr0, blk_ld_r, ext64);
    _test(blk_sd_rr1, blk_ld_t, ext64);
    _test(blk_sd_t, blk_ld_r, ext64);

    return 0;
}
