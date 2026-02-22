#include <stdio.h>
#include <time.h>

static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int result = fib(35);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("%d\n", result);
    fprintf(stderr, "[Time: %.3f ms]\n", elapsed);
    return 0;
}
