#include "test_atomic_case.h"
#include "pthread.h"

uint g_cpu, g_node;
atomic_test_t g_atomic;

static void* thread_handle(void* arg)
{
    int ret;
    syscall(SYS_getcpu, &g_cpu, &g_node, NULL);
    if (arg == NULL) {
        ret = -2;
        goto end;
    }

    thread_arg* current = arg;

    // LOG("PID %d running.\n", current->tipd);
    if (current->func != NULL) {
        ret = current->func(arg);
    }

end:
    current->ret = ret;
    return NULL;
}

static void do_test_case(testcase_t* test)
{
    thread_arg* arg = NULL;
    thread_arg* arg_tmp = NULL;

    arg = (thread_arg*)malloc(sizeof(thread_arg) * test->thread_index);
    if (arg == NULL) {
        BUG();
    }

    memset(&g_atomic, 0, sizeof(atomic_test_t));
    test->ret = 0;
    for (int index = 0; index < test->thread_index; index++) {
        arg_tmp = arg + index;
        arg_tmp->pthread_index = index;
        arg_tmp->atomic = &g_atomic;
        arg_tmp->func = test->func;
        // LOG("Index %d start to creat.\n", index);
        if (pthread_create(&(arg_tmp->tipd), NULL, thread_handle, (void*)arg_tmp)) {
            BUG();
        }
        // LOG("Index %d %d start to run.\n", index, arg_tmp->tipd);
    }

    for (int index = 0; index < test->thread_index; index++) {
        arg_tmp = arg + index;
        // LOG("Join Thread %d.\n", arg_tmp->tipd);
        if (pthread_join(arg_tmp->tipd, NULL)) {
            BUG();
        }
        // LOG("Wait %d exit: %d \n", arg_tmp->tipd, arg_tmp->ret);
        if (arg_tmp->ret != 0) {
            test->ret = -1;
            LOG("PID %d ret %d =========> NG", arg_tmp->tipd, arg_tmp->ret);
        }
    }

    if ((test->ret != 0) || ((test->want_val != NOT_USED) &&
        (((test->xlen == ATOMIC_TEST_WORD) && (g_atomic.atomic.counter != test->want_val)) ||
        ((test->xlen == ATOMIC_TEST_DWORD) && (g_atomic.atomic64.counter != test->want_val))))) {
        LOG("Test Case: %s ======> NG : Atomic Counter %ld[0x%lx] != %ld[0x%lx]\n", test->name,
            (test->xlen == ATOMIC_TEST_WORD) ? g_atomic.atomic.counter : g_atomic.atomic64.counter,
            (test->xlen == ATOMIC_TEST_WORD) ? g_atomic.atomic.counter : g_atomic.atomic64.counter, test->want_val, test->want_val);
        test->ret = -1;
    }
    // LOG("Test Case: %s ======> PASS.\n", test->name);
    free(arg);
    arg = NULL;
}

int main(int argc, int** argv[])
{
    int success_count = 0;

    syscall(SYS_getcpu, &g_cpu, &g_node, NULL);
    LOG("TestCase Total [%d] Load.\n", ARRAY_SIZE(g_test_case));
    for (int index = 0; index < ARRAY_SIZE(g_test_case); index++) {
        do_test_case(&g_test_case[index]);
    }
    LOG("==================Test Report=================\n");
    for (int index = 0; index < ARRAY_SIZE(g_test_case); index++) {
        LOG("%s\t======>\t%s.\n", g_test_case[index].name, (g_test_case[index].ret == 0) ? "PASS" : "NG");
        if (g_test_case[index].ret == 0) {
            success_count++;
        }
    }
    LOG("Total Pass %d/%d.\n", success_count, ARRAY_SIZE(g_test_case));
    LOG("==============================================\n");
    return 0;
}
