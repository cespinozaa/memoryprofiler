// sampler.c: every N ms, count the pages a process wrote (soft-dirty) and
// the pages it has in RAM (present), for its biggest anonymous region

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#define PAGE_SZ       4096UL
#define BUCKET_PAGES  256UL            // 256 pages = 1 MB per heatmap row
#define BIT_PRESENT   (1ULL << 63)     // pagemap: page is in RAM
#define BIT_SOFTDIRTY (1ULL << 55)     // pagemap: written since last clear_refs

static volatile sig_atomic_t stop;
static void on_sigint(int sig) { (void)sig; stop = 1; }

// find the largest anonymous read-write mapping in /proc/PID/maps
static int find_region(int pid, unsigned long *start, unsigned long *len)
{
    char path[64], line[512];
    unsigned long best = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    f = fopen(path, "r");
    if (!f){
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        unsigned long lo, hi;
        char perms[8], name[256] = "";
        // line: start-end perms offset dev inode [name]
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &lo, &hi, perms, name);

        if (n < 3 || n == 4){           // n == 4 means it has a name (file, [heap], ...)
            continue;
        }
        if (perms[0] == 'r' && perms[1] == 'w' && hi - lo > best) {
            best = hi - lo;
            *start = lo;
            *len = hi - lo;
        }
    }
    fclose(f);
    return best ? 0 : -1;
}

// pread, keep going until we have it all
static int read_full(int fd, void *buf, size_t n, off_t off)
{
    size_t done = 0;

    while (done < n) {
        ssize_t r = pread(fd, (char *)buf + done, n - done, off + done);
        if (r <= 0)
            return -1;
        done += r;
    }
    return 0;
}

// writing 4 to clear_refs to clear every soft-dirty bit of the process
static int reset_bits(int fd)
{
    return write(fd, "4", 1) == 1 ? 0 : -1;
}

static long ms_since(const struct timespec *t0)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

int main(int argc, char **argv)
{
    char path[64];
    unsigned long start, len;
    struct timespec t0;
    int pid, interval_ms, pm, clr;

    if (argc < 2) {
        fprintf(stderr, "usage: %s PID [interval_ms]\n", argv[0]);
        return 1;
    }
    pid = atoi(argv[1]);
    interval_ms = argc > 2 ? atoi(argv[2]) : 100;

    if (find_region(pid, &start, &len)) {
        fprintf(stderr, "no anonymous rw region found for pid %d\n", pid);
        return 1;
    }
    unsigned long npages = len / PAGE_SZ;
    unsigned long nbuckets = (npages + BUCKET_PAGES - 1) / BUCKET_PAGES;
    uint64_t *entries = malloc(npages * sizeof(uint64_t));
    unsigned int *written = malloc(nbuckets * sizeof(unsigned int));
    unsigned int *present = malloc(nbuckets * sizeof(unsigned int));
    if (!entries || !written || !present) {
        perror("malloc");
        return 1;
    }

    snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
    pm = open(path, O_RDONLY);
    snprintf(path, sizeof(path), "/proc/%d/clear_refs", pid);
    clr = open(path, O_WRONLY);
    if (pm < 0 || clr < 0) {
        perror("open /proc files, did you use sudo?");
        return 1;
    }

    fprintf(stderr, "region 0x%lx, %lu MB, %lu pages, every %d ms\n",
            start, len >> 20, npages, interval_ms);
    printf("time_ms,region_start,bucket,written_pages,present_pages\n");

    signal(SIGINT, on_sigint);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    reset_bits(clr);                       // start the first interval from zero

    while (!stop) {
        usleep(interval_ms * 1000);

        // read first, then reset, so little time passes between the two
        if (read_full(pm, entries, npages * sizeof(uint64_t),
                      (off_t)(start / PAGE_SZ) * 8))
            break;                         // process exited or region is gone
        if (reset_bits(clr))
            break;
        long t = ms_since(&t0);

        memset(written, 0, nbuckets * sizeof(unsigned int));
        memset(present, 0, nbuckets * sizeof(unsigned int));
        for (unsigned long i = 0; i < npages; i++) {
            if (entries[i] & BIT_PRESENT)
                present[i / BUCKET_PAGES]++;
            if (entries[i] & BIT_SOFTDIRTY)
                written[i / BUCKET_PAGES]++;
        }
        for (unsigned long b = 0; b < nbuckets; b++)
            printf("%ld,0x%lx,%lu,%u,%u\n", t, start, b, written[b], present[b]);
        fflush(stdout);
    }
    return 0;
}