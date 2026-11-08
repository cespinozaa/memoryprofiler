// memprof.c: run a program, find its big mmap regions with eBPF, name the code
// that allocated each one, and sample which pages get written (and, with
// MEMPROF_READS=1, which pages get touched at all)
// sudo ./memprof OUT.csv INTERVAL_MS PROGRAM [ARGS...]
// sudo env MEMPROF_READS=1 ./memprof OUT.csv INTERVAL_MS PROGRAM [ARGS...]
// Writes OUT.csv (the samples) and OUT.csv.labels (site id -> source lines)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "memprof.h"
#include "memprof.skel.h"

#define PAGE_SZ       4096UL
#define BUCKET_PAGES  256UL            // 256 pages = 1 MB per heatmap row
#define BIT_PRESENT   (1ULL << 63)     // pagemap: page is in RAM
#define BIT_SOFTDIRTY (1ULL << 55)     // pagemap: written since last clear_refs
#define PFN_MASK      ((1ULL << 55) - 1)   // pagemap: bits 0-54 are the physical frame number
#define MIN_LEN       (1UL << 20)      // only track mmaps of at least 1 MB
#define MAX_REGIONS   64
#define NFR           6                // stack frames kept per allocation site
#define MAX_SITES     256
#define LOC_SZ        2048

struct region {
    unsigned long start, npages, nbuckets;
    int site;                          // which allocation site made it
    uint64_t *entries;                 // pagemap entries read at each sample
    unsigned int *written, *present;   // per-bucket page counts
    unsigned int *accessed;            // per-bucket count of pages touched (read mode only)
    int fresh;                         // 1 until its first sample has been thrown away
    int dead;                          // 1 once the region has been unmapped or can't be read
};

// An allocation site = the first NFR stack frames inside the target's binary.
// Their source lines are looked up only at exit, so the target is never slowed
// down by addr2line while it runs.
struct site {
    unsigned long long fr[NFR];
    int nfr;
};

static struct region regions[MAX_REGIONS];
static int nregions;
static struct site sites[MAX_SITES];
static int nsites;
static char answers[MAX_SITES * NFR][LOC_SZ];   // addr2line's answer for each looked-up address
static char exe[256];                           // the target's executable
static unsigned long exe_lo, exe_hi;            // where it is loaded
static volatile sig_atomic_t stop;
static int stacks_fd, pm = -1, clr = -1;
static int reads;                               // 1 when MEMPROF_READS is set: also track reads
static int idle_fd = -1;                        // /sys/kernel/mm/page_idle/bitmap
static long idle_errors;                        // failed reads or writes of that file
static long nsamples, sample_ms;                // how many sampling rounds, and how long they took
static FILE *csv;
static const char *csv_path;
static struct timespec t0;

static void on_sigint(int sig) { (void)sig; stop = 1; }

static long ms_since(const struct timespec *t)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

// pread can return less than asked, so keep going until we have it all
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

// writing 4 to clear_refs clears every soft-dirty bit of the process
static int reset_bits(int fd)
{
    return write(fd, "4", 1) == 1 ? 0 : -1;
}

// Find the target's own executable in /proc/PID/maps: its path, and the lowest
// and highest address of its mappings. Returns 0 on success.
static int find_exe(int pid, char *path_out, size_t sz, unsigned long *lo, unsigned long *hi)
{
    char path[64], line[512];
    ssize_t n;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    n = readlink(path, path_out, sz - 1);
    if (n < 0)
        return -1;
    path_out[n] = '\0';

    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    *lo = *hi = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long a, b;
        char name[256] = "";
        // line: start-end perms offset dev inode [name]; 3 results means it has a name
        int cnt = sscanf(line, "%lx-%lx %*s %*s %*s %*s %255s", &a, &b, name);

        if (cnt == 3 && strcmp(name, path_out) == 0) {
            if (!*lo)
                *lo = a;            // maps are in address order: first one is the load base
            *hi = b;
        }
    }
    fclose(f);
    return *lo ? 0 : -1;
}

// Pick the first NFR stack frames that lie inside the target's own binary.
static int pick_frames(const unsigned long long *addrs, unsigned long long *fr)
{
    int i, n = 0;

    for (i = 0; i < MAX_STACK_DEPTH && addrs[i] && n < NFR; i++)
        if (addrs[i] >= exe_lo && addrs[i] < exe_hi)
            fr[n++] = addrs[i];
    return n;
}

// the id of the site with these frames, adding it if it is new (-1 if the table is full)
static int find_site(const unsigned long long *fr, int n)
{
    int i, j;

    for (i = 0; i < nsites; i++) {
        if (sites[i].nfr != n)
            continue;
        for (j = 0; j < n && sites[i].fr[j] == fr[j]; j++)
            ;
        if (j == n)
            return i;
    }
    if (nsites >= MAX_SITES)
        return -1;
    sites[nsites].nfr = n;
    memcpy(sites[nsites].fr, fr, n * sizeof(fr[0]));
    return nsites++;
}

// "function at /path/file.c:line" -> "file.c:line (function)". Returns 0 on success.
static int format_loc(char *text, char *out, size_t sz)
{
    char *at, *file;

    text += strspn(text, " ");
    if (strncmp(text, "(inlined by) ", 13) == 0)
        text += 13;
    at = strstr(text, " at ");
    if (!at)
        return -1;
    *at = '\0';                         // text is now just the function name
    file = at + 4;
    if (strrchr(file, '/'))
        file = strrchr(file, '/') + 1;  // keep only the file name
    file[strcspn(file, " ")] = '\0';    // drop a trailing "(discriminator N)"
    snprintf(out, sz, "%s (%s)", file, text);
    return 0;
}

// Look up every allocation site in ONE addr2line run (it reads the debug info
// once), then write the id -> label table next to the CSV.
static void write_labels(void)
{
    char path[600], line[LOC_SZ], loc[LOC_SZ], label[NFR * LOC_SZ + 16];
    unsigned long bias = (exe_lo == 0x400000) ? 0 : exe_lo;   // a non-PIE binary needs no offset
    int first[MAX_SITES];
    int nq = 0, cur = -1, s, j;
    size_t cap, len;
    char *cmd, *c;
    FILE *p, *f;

    if (!nsites)
        return;
    cap = 512 + (size_t)nsites * NFR * 24;
    cmd = malloc(cap);
    if (!cmd)
        return;
    len = snprintf(cmd, cap, "addr2line -e '%s' -a -f -C -i -p", exe);
    for (s = 0; s < nsites; s++) {
        first[s] = nq;
        for (j = 0; j < sites[s].nfr; j++, nq++)
            // a return address points just after the call, so step back one byte
            len += snprintf(cmd + len, cap - len, " 0x%llx", sites[s].fr[j] - bias - 1);
    }

    fprintf(stderr, "looking up %d allocation sites...\n", nsites);
    p = popen(cmd, "r");
    free(cmd);
    if (!p)
        return;
    while (fgets(line, sizeof(line), p)) {
        char *text = line;

        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "0x", 2) == 0) {      // the answer for the next address starts here
            cur++;
            text = strstr(line, ": ");
            text = text ? text + 2 : NULL;
        }
        if (cur >= 0 && cur < nq && text && *text)
            snprintf(answers[cur], LOC_SZ, "%s", text);   // keep the last line: the outermost function
    }
    pclose(p);

    snprintf(path, sizeof(path), "%s.labels", csv_path);
    f = fopen(path, "w");
    if (!f)
        return;
    for (s = 0; s < nsites; s++) {
        label[0] = '\0';
        for (j = 0; j < sites[s].nfr; j++) {
            if (format_loc(answers[first[s] + j], loc, sizeof(loc)))
                continue;
            if (label[0])
                strncat(label, " <- ", sizeof(label) - strlen(label) - 1);
            strncat(label, loc, sizeof(label) - strlen(label) - 1);
        }
        for (c = label; *c; c++)
            if (*c == ',')
                *c = ';';                       // keep the file one-field-per-comma
        fprintf(f, "S%d,%s\n", s, label);
    }
    fclose(f);
}

// find a slot for a new region: reuse one whose region was unmapped
static struct region *new_slot(void)
{
    int i;

    for (i = 0; i < nregions; i++) {
        if (regions[i].dead) {
            free(regions[i].entries);
            free(regions[i].written);
            free(regions[i].present);
            free(regions[i].accessed);
            return &regions[i];
        }
    }
    return nregions < MAX_REGIONS ? &regions[nregions++] : NULL;
}

// a range was unmapped: stop sampling any tracked region that overlaps it
static void drop_regions(unsigned long long addr, unsigned long long len)
{
    int i;

    for (i = 0; i < nregions; i++) {
        struct region *r = &regions[i];

        if (!r->dead && addr < r->start + r->npages * PAGE_SZ && addr + len > r->start)
            r->dead = 1;
    }
}

// start tracking a newly mapped region, if its stack has frames in the target's binary
static void add_region(int pid, unsigned long long addr, unsigned long long len,
                       const unsigned long long *addrs)
{
    unsigned long long fr[NFR];
    char path[64];
    struct region *r;
    int n, site;

    if (!exe_hi && find_exe(pid, exe, sizeof(exe), &exe_lo, &exe_hi))
        return;
    n = pick_frames(addrs, fr);
    if (n == 0) {
        fprintf(stderr, "skipped mmap 0x%llx (%llu MB): no frame in the target binary\n",
                addr, len >> 20);
        return;
    }
    site = find_site(fr, n);
    if (site < 0) {
        fprintf(stderr, "skipped mmap 0x%llx: site table full\n", addr);
        return;
    }
    // open the /proc files only now: the target has exec'd by this point, and
    // files opened before exec would refer to its old address space
    if (pm < 0) {
        snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
        pm = open(path, O_RDONLY);
        snprintf(path, sizeof(path), "/proc/%d/clear_refs", pid);
        clr = open(path, O_WRONLY);
        if (pm < 0 || clr < 0) {
            perror("open /proc files");
            stop = 1;
            return;
        }
    }
    r = new_slot();
    if (!r) {
        fprintf(stderr, "skipped mmap 0x%llx: region table full\n", addr);
        return;
    }
    r->site = site;
    r->start = addr;
    r->npages = (len + PAGE_SZ - 1) / PAGE_SZ;
    r->nbuckets = (r->npages + BUCKET_PAGES - 1) / BUCKET_PAGES;
    r->entries = malloc(r->npages * sizeof(uint64_t));
    r->written = malloc(r->nbuckets * sizeof(unsigned int));
    r->present = malloc(r->nbuckets * sizeof(unsigned int));
    r->accessed = malloc(r->nbuckets * sizeof(unsigned int));
    if (!r->entries || !r->written || !r->present || !r->accessed) {
        perror("malloc");
        r->dead = 1;
        stop = 1;
        return;
    }
    r->fresh = 1;
    r->dead = 0;
    fprintf(stderr, "tracking 0x%lx, %llu MB (site S%d)\n", r->start, len >> 20, site);
}

// called once per event pulled from the ring buffer
static int on_event(void *ctx, void *data, size_t size)
{
    const struct alloc_event *e = data;
    unsigned long long addrs[MAX_STACK_DEPTH] = {};

    if (e->kind == EV_MUNMAP) {
        drop_regions(e->addr, e->len);
        return 0;
    }
    if (e->len < MIN_LEN)
        return 0;
    if (e->stack_id < 0 || bpf_map_lookup_elem(stacks_fd, &e->stack_id, addrs)) {
        fprintf(stderr, "lost mmap 0x%llx (%llu MB): could not get its call stack (id %d)\n",
                e->addr, e->len >> 20, e->stack_id);
        return 0;
    }
    add_region(e->pid, e->addr, e->len, addrs);
    return 0;
}

// Read tracking. /sys/kernel/mm/page_idle/bitmap has one bit per physical page frame
// (PFN): bit (PFN % 64) of the 8-byte word at offset (PFN / 64) * 8. Writing a 1 marks
// that page idle; the kernel clears the mark when the page is next accessed (read or
// write). So at each sample we read the marks (a cleared mark means "touched since the
// last sample") and mark every resident page idle again.
//
// The pagemap entries we already read give each present page's PFN. Pages that share
// an 8-byte word are handled with one read and one write.
static void arm_word(uint64_t word_index, uint64_t bits)
{
    if (bits && pwrite(idle_fd, &bits, 8, (off_t)word_index * 8) != 8)
        idle_errors++;
}

static void idle_pass(struct region *r, int count)
{
    uint64_t word = 0, arm = 0, cur = ~0ULL;     // the word loaded now, bits to re-mark, its index
    unsigned long p;

    for (p = 0; p < r->npages; p++) {
        uint64_t e = r->entries[p], pfn, w, bit;

        if (!(e & BIT_PRESENT))
            continue;
        pfn = e & PFN_MASK;
        if (!pfn)
            continue;                   // frame number hidden: not running as root
        w = pfn / 64;
        bit = 1ULL << (pfn % 64);
        if (w != cur) {
            if (cur != ~0ULL)
                arm_word(cur, arm);
            cur = w;
            arm = 0;
            if (pread(idle_fd, &word, 8, (off_t)w * 8) != 8) {
                word = ~0ULL;           // could not read: call it idle, not accessed
                idle_errors++;
            }
        }
        if (count && !(word & bit))     // mark cleared: the page was touched
            r->accessed[p / BUCKET_PAGES]++;
        arm |= bit;
    }
    if (cur != ~0ULL)
        arm_word(cur, arm);
}

// one sampling round: read every region's pagemap, reset the bits, then count
static void sample_all(long t)
{
    struct region *r;
    unsigned long p, b;
    int i;

    // read all regions first, then reset once, so little time passes between
    for (i = 0; i < nregions; i++) {
        r = &regions[i];
        if (!r->dead && read_full(pm, r->entries, r->npages * sizeof(uint64_t),
                                  (off_t)(r->start / PAGE_SZ) * 8))
            r->dead = 1;                // unmapped, or the process is gone
    }
    reset_bits(clr);

    if (reads) {                        // a new region's first pass only marks, it does not count
        for (i = 0; i < nregions; i++) {
            r = &regions[i];
            if (r->dead)
                continue;
            memset(r->accessed, 0, r->nbuckets * sizeof(unsigned int));
            idle_pass(r, !r->fresh);
        }
    }

    for (i = 0; i < nregions; i++) {
        r = &regions[i];
        if (r->dead)
            continue;
        if (r->fresh) {                 // new memory looks written to the kernel: skip this one
            r->fresh = 0;
            continue;
        }
        memset(r->written, 0, r->nbuckets * sizeof(unsigned int));
        memset(r->present, 0, r->nbuckets * sizeof(unsigned int));
        for (p = 0; p < r->npages; p++) {
            if (r->entries[p] & BIT_PRESENT)
                r->present[p / BUCKET_PAGES]++;
            if (r->entries[p] & BIT_SOFTDIRTY)
                r->written[p / BUCKET_PAGES]++;
        }
        for (b = 0; b < r->nbuckets; b++) {
            fprintf(csv, "%ld,0x%lx,S%d,%lu,%u,%u,%lu", t, r->start, r->site, b,
                    r->written[b], r->present[b], r->npages);
            if (reads)
                fprintf(csv, ",%u", r->accessed[b]);
            fputc('\n', csv);
        }
    }
    fflush(csv);
}

int main(int argc, char **argv)
{
    struct memprof_bpf *skel;
    struct ring_buffer *rb;
    int go[2], status, interval_ms;
    long next;
    pid_t child;

    if (argc < 4) {
        fprintf(stderr, "usage: %s OUT.csv INTERVAL_MS PROGRAM [ARGS...]\n", argv[0]);
        return 1;
    }
    csv_path = argv[1];
    interval_ms = atoi(argv[2]);
    if (interval_ms <= 0) {
        fprintf(stderr, "INTERVAL_MS must be positive\n");
        return 1;
    }
    csv = fopen(csv_path, "w");
    if (!csv) {
        perror(csv_path);
        return 1;
    }
    reads = getenv("MEMPROF_READS") && atoi(getenv("MEMPROF_READS")) != 0;
    if (reads) {
        idle_fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR);
        if (idle_fd < 0) {
            perror("open /sys/kernel/mm/page_idle/bitmap (needs root)");
            return 1;
        }
    }
    if (pipe(go)) {
        perror("pipe");
        return 1;
    }

    child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {                   // child: wait for the go-ahead, then become the target
        char c;

        close(go[1]);
        if (read(go[0], &c, 1) != 1)
            _exit(1);
        close(go[0]);
        // glibc raises its mmap threshold as a program frees big blocks, which moves
        // later allocations onto the heap where the tracer cannot see them. Setting
        // the threshold ourselves turns that adjustment off.
        setenv("MALLOC_MMAP_THRESHOLD_", "1048576", 1);
        unsetenv("MEMPROF_READS");      // the target does not need to see our switch
        execvp(argv[3], &argv[3]);
        perror("execvp");
        _exit(1);
    }
    close(go[0]);

    skel = memprof_bpf__open();
    if (!skel)
        return 1;
    skel->rodata->targ_pid = child;     // set between open and load
    if (memprof_bpf__load(skel) || memprof_bpf__attach(skel)) {
        fprintf(stderr, "load or attach failed, did you use sudo?\n");
        kill(child, SIGKILL);
        memprof_bpf__destroy(skel);
        return 1;
    }
    stacks_fd = bpf_map__fd(skel->maps.stacks);
    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "ring buffer failed\n");
        kill(child, SIGKILL);
        memprof_bpf__destroy(skel);
        return 1;
    }

    fprintf(csv, "time_ms,region_start,label,bucket,written_pages,present_pages,region_pages%s\n",
            reads ? ",accessed_pages" : "");
    signal(SIGINT, on_sigint);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    next = interval_ms;                 // when the next sample is due, in ms since start
    if (write(go[1], "x", 1) != 1)      // tracer is attached: let the target run
        perror("write");
    close(go[1]);

    while (!stop && waitpid(child, &status, WNOHANG) == 0) {
        long now = ms_since(&t0);

        if (now >= next) {
            if (pm >= 0) {              // nothing to sample until a region has appeared
                sample_all(now);
                nsamples++;
                sample_ms += ms_since(&t0) - now;
            }
            next += interval_ms;
            if (reads)                  // a slow pass must not cause a burst of back-to-back samples
                while (next <= ms_since(&t0))
                    next += interval_ms;
            continue;
        }
        // wait for events, but not past the next sample, and wake up at least every
        // 50 ms so we notice quickly when the target exits
        ring_buffer__poll(rb, next - now > 50 ? 50 : next - now);
    }

    if (stop)
        kill(child, SIGTERM);
    fclose(csv);
    if (nsamples)
        fprintf(stderr, "%ld samples, %.1f ms each on average\n",
                nsamples, (double)sample_ms / nsamples);
    if (idle_errors)
        fprintf(stderr, "warning: %ld idle-bitmap reads or writes failed\n", idle_errors);
    write_labels();                     // the target is done: now it is safe to be slow
    ring_buffer__free(rb);
    memprof_bpf__destroy(skel);
    return 0;
}