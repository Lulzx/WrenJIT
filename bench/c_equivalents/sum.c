#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    volatile long long sum = 0;
    for (int i = 0; i < 1000000; i++) {
        sum += i;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("%lld\n", sum);
    fprintf(stderr, "[Time: %.3f ms]\n", elapsed);
    return 0;
}
