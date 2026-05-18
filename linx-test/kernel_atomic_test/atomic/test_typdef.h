#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sched.h"
#include "sys/wait.h"
#include "sys/ipc.h"
#include "sys/shm.h"
#include "sys/syscall.h"
#include "atomic.h"

#ifndef __TEST_TYPDEF__
#define __TEST_TYPDEF__

#define BUG() do {\
    printf("Hart[%d][%d] PID[%d] %s %d : Oops Here.\n", g_cpu, g_node, getpid(), __func__, __LINE__); \
    while(1) {} \
} while(0)

#define LOG(fmt, ...) do {\
    printf("Hart[%d][%d] PID[%d] %s %d : "fmt"\n", g_cpu, g_node, getpid(), __func__, __LINE__, ##__VA_ARGS__); \
} while(0)

#define LOG_RAW(fmt, ...) do {\
    printf("PID[%d] %s %d : "fmt"\n", getpid(), __func__, __LINE__, ##__VA_ARGS__); \
} while(0)

#define NOT_USED    0xFFFFFFFF
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

typedef enum {
    ATOMIC_TEST_WORD = 0,
    ATOMIC_TEST_DWORD,
} test_xlen;

typedef union {
    atomic_t atomic;
    atomic64_t atomic64;
} atomic_test_t;

typedef struct {
    atomic_test_t* atomic;
    int pthread_index;
    pthread_t tipd;
    int ret;
    int (*func)(void*);
} thread_arg;

typedef struct {
    char* name;
    int thread_index;
    int (*func)(void*);
    s64 want_val;
    int ret;
    test_xlen xlen;
} testcase_t;

#endif
