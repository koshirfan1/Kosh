/* benchmarks/loop.c — same tight loop, 5,000,000 iterations. The "floor":
   a native-compiled reference point with no interpreter/VM overhead at all. */
#include <stdio.h>
int main(void) {
    double total = 0;
    long i = 0;
    while (i < 5000000) {
        total = total + i;
        i = i + 1;
    }
    printf("%.0f\n", total);
    return 0;
}
