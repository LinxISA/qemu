int __attribute__ ((noinline)) test_add(int a, int b)
{
    return a + b;
}

int main()
{
    int a = 5, b = 4;
    volatile int c = 0;

    c = test_add(a, b);
    if (c == 9)
        return 3;
    else
        return 5;
}
