#include "driver.h"
#include "asm.h"

uint64_t data[] = {
#TEST_DATA
};

TestEvent testEvent[] = {
#TEST_EVENT
    END_EVENT
};

int main(int argc, char **argv)
{
    int i;
    for (i = 0; testEvent[i].name; ++i) {
        TEST_DRIVER(testEvent + i);
    }
    return 0;
}
