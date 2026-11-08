// microbench.c: 1 GB array, but only the first 100 MB is ever written in the loop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define TOTAL (1UL << 30)
#define HOT (100UL << 20)

int main(int argc, char **argv){
    int init_all = argc > 1 && strcmp(argv[1], "init_all") == 0;
    int seconds = 30;

    volatile char *arr = malloc(TOTAL);
    if (!arr) {
        perror("malloc");
        return 1;
    }

    printf("pid %d, array at %p, size %lu MB\n", getpid(), (void *)arr, TOTAL >> 20);
    fflush(stdout);

    if (init_all){
        memset((void *)arr, 1, TOTAL);
    }

    time_t end = time(NULL) + seconds;
    while (time(NULL) < end) {
        for (size_t i = 0; i < HOT; i += 4096) {
            arr[i]++;
        }
        usleep(10000);
    }
    return 0;
}
