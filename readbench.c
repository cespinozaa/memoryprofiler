// readbench.c: known-answer test for read tracking
// three 128 MB arrays, all filled first so every page is in RAM:
//   a is written all the time, b is only read all the time, c is never touched again
// expected: "written" shows only a; "accessed" shows a and b; neither shows c
// /readbench [SECONDS]      (default 20)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SIZE (128UL << 20)
#define PAGE 4096UL

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    unsigned char *a = malloc(SIZE);
    unsigned char *b = malloc(SIZE);
    unsigned char *c = malloc(SIZE);
    volatile unsigned char *bv = b;     // volatile: the compiler must really read b
    unsigned long sum = 0, i;
    time_t end;

    if (!a || !b || !c)
        return 1;
    memset(a, 1, SIZE);
    memset(b, 2, SIZE);
    memset(c, 3, SIZE);

    end = time(NULL) + seconds;
    while (time(NULL) < end) {
        for (i = 0; i < SIZE; i += PAGE)
            a[i]++;                     // write one byte in every page of a
        for (i = 0; i < SIZE; i += PAGE)
            sum += bv[i];               // read one byte from every page of b
        usleep(10000);
    }
    printf("done %lu\n", sum + c[0]);   // c[0] read once, at the very end
    return 0;
}