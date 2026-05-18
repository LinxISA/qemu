#ifndef __DRIVER_H__
#define __DRIVER_H__

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    char const *name;
    void *instAddr;
    int dataLength;
    void *data;
} TestEvent;

#define NEW_EVENT(name) { #name, name, TEST_SIZE, data },
#define END_EVENT { 0, 0, 0, 0 }

#define reportSummary(op, ac, tot) \
    printf("[\033[33mSummary\033[0m]  %s: %d/%d accepted\n", op, ac, tot)

#define reportVerbose_5(op, s1, s2, e, r) \
    printf("[\033[32mAccepted\033[0m] \
    %s: 0x%-16lx, 0x%-16lx, 0x%-16lx, 0x%-16lx\n", op, s1, s2, e, r)
#define reportMismatch_5(op, s1, s2, e, r) \
    printf("[\033[31mMismatch\033[0m] \
    %s: 0x%-16lx, 0x%-16lx, 0x%-16lx, 0x%-16lx\n", op, s1, s2, e, r)
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


void testDriver_2_64(TestEvent *event)
{
typedef uint64_t (*Instruction)(uint64_t, uint64_t);
    Instruction inst = event->instAddr;
    uint64_t result, *ptr = event->data;
    int accepted = 0, i;
    for (i = 0; i < event->dataLength; ++i, ptr += 3) {
        result = inst(ptr[0], ptr[1]);
        if (result == ptr[2]) {
            ++accepted;
            reportVerbose(event->name, ptr[0], ptr[1], ptr[2], result);
        } else {
            reportMismatch(event->name, ptr[0], ptr[1], ptr[2], result);
        }
    }
    reportSummary(event->name, accepted, event->dataLength);
}

void testDriver_2_32(TestEvent *event)
{
typedef uint64_t (*Instruction)(uint64_t, uint64_t);
    Instruction inst = event->instAddr;
    uint64_t result, *ptr = event->data;
    int accepted = 0, i;
    for (i = 0; i < event->dataLength; ++i, ptr += 3) {
        result = inst(ptr[0], ptr[1]);
        if (result == ptr[2]) {
            ++accepted;
            reportVerbose(event->name, ptr[0], ptr[1], ptr[2], result);
        } else {
            reportMismatch(event->name, ptr[0], ptr[1], ptr[2], result);
        }
    }
    reportSummary(event->name, accepted, event->dataLength);
}

void testDriver_1_64(TestEvent *event)
{
typedef uint64_t (*Instruction)(uint64_t);
    Instruction inst = event->instAddr;
    uint64_t result, *ptr = event->data;
    int accepted = 0, i;
    for (i = 0; i < event->dataLength; ++i, ptr += 2) {
        result = inst(ptr[0]);
        if (result == ptr[1]) {
            ++accepted;
            reportVerbose(event->name, ptr[0], ptr[1], result);
        } else {
            reportMismatch(event->name, ptr[0], ptr[1], result);
        }
    }
    reportSummary(event->name, accepted, event->dataLength);
}

void testDriver_1_32(TestEvent *event)
{
typedef uint64_t (*Instruction)(uint64_t);
    Instruction inst = event->instAddr;
    uint64_t result, *ptr = event->data;
    int accepted = 0, i;
    for (i = 0; i < event->dataLength; ++i, ptr += 3) {
        result = inst(ptr[0]);
        if (result == ptr[1]) {
            ++accepted;
            reportVerbose(event->name, ptr[0], ptr[1], result);
        } else {
            reportMismatch(event->name, ptr[0], ptr[1], result);
        }
    }
    reportSummary(event->name, accepted, event->dataLength);
}

#endif /* __DRIVER_H__ */
