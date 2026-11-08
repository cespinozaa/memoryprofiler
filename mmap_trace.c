// mmap_trace.c: run a program and print every large mmap it makes

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "mmap_trace.h"
#include "mmap_trace.skel.h"
#include <string.h>

#define MIN_LEN (1UL << 20)             // ignore mmaps under 1 MB (loader noise)

static volatile sig_atomic_t stop;
static int stacks_fd;

static void on_sigint(int sig) { (void)sig; stop = 1; }

// find the target's own executable in /proc/PID/maps: its path, the lowest
// and highest address of its mappings, returns 0 on success
static int find_exe(int pid, char *exe, size_t exe_sz, unsigned long *lo, unsigned long *hi)
{
    char path[64], line[512];
    ssize_t n;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    n = readlink(path, exe, exe_sz - 1);
    if (n < 0)
        return -1;
    exe[n] = '\0';

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

        if (cnt == 3 && strcmp(name, exe) == 0) {
            if (!*lo)
                *lo = a;            // maps are in address order: first one is the load base
            *hi = b;
        }
    }
    fclose(f);
    return *lo ? 0 : -1;
}

// label an allocation: take the first stack frame inside the target's own
// binary (the line in its code that led to the mmap) and run addr2line on it
static void print_label(int pid, const unsigned long long *addrs)
{
    char exe[256], cmd[512], out[512];
    unsigned long lo, hi, bias;
    FILE *p;
    int i;

    if (find_exe(pid, exe, sizeof(exe), &lo, &hi))
        return;
    bias = (lo == 0x400000) ? 0 : lo;   // a non-PIE binary is loaded at 0x400000 and needs no offset
    for (i = 0; i < MAX_STACK_DEPTH && addrs[i]; i++) {
        if (addrs[i] < lo || addrs[i] >= hi)
            continue;
        // return address points just after the call, so step back one byte
        snprintf(cmd, sizeof(cmd), "addr2line -e '%s' -f -C -p 0x%llx", exe, addrs[i] - bias - 1);
        p = popen(cmd, "r");
        if (p && fgets(out, sizeof(out), p))
            printf("    label: %s", out);
        if (p)
            pclose(p);
        return;
    }
}

// called once per event pulled from the ring buffer
static int on_event(void *ctx, void *data, size_t size)
{
    const struct alloc_event *e = data;
    unsigned long long addrs[MAX_STACK_DEPTH] = {};
    int i;

    if (e->len < MIN_LEN){
        return 0;
    }
    printf("mmap addr=0x%llx len=%llu bytes (%llu MB) stack_id=%d\n",
           e->addr, e->len, e->len >> 20, e->stack_id);
    if (e->stack_id < 0 || bpf_map_lookup_elem(stacks_fd, &e->stack_id, addrs)){
        return 0;
    }
    for (i = 0; i < MAX_STACK_DEPTH && addrs[i]; i++)
        printf("    0x%llx\n", addrs[i]);
    print_label(e->pid, addrs);
    return 0;
}

int main(int argc, char **argv)
{
    struct mmap_trace_bpf *skel;
    struct ring_buffer *rb;
    int go[2], status;
    pid_t child;

    if (argc < 2) {
        fprintf(stderr, "usage: %s PROGRAM [ARGS...]\n", argv[0]);
        return 1;
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
    if (child == 0) {                   // child: wait for the go ahead, then become the target
        char c;

        close(go[1]);
        if (read(go[0], &c, 1) != 1)
            _exit(1);
        close(go[0]);
        execvp(argv[1], &argv[1]);
        perror("execvp");
        _exit(1);
    }
    close(go[0]);

    skel = mmap_trace_bpf__open();
    if (!skel)
        return 1;
    skel->rodata->targ_pid = child;     // set between open and load
    if (mmap_trace_bpf__load(skel) || mmap_trace_bpf__attach(skel)) {
        fprintf(stderr, "load or attach failed, did you use sudo?\n");
        kill(child, SIGKILL);
        mmap_trace_bpf__destroy(skel);
        return 1;
    }
    stacks_fd = bpf_map__fd(skel->maps.stacks);
    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "ring buffer failed\n");
        kill(child, SIGKILL);
        mmap_trace_bpf__destroy(skel);
        return 1;
    }

    signal(SIGINT, on_sigint);
    if (write(go[1], "x", 1) != 1)      // tracer is attached: let the target run
        perror("write");
    close(go[1]);

    while (!stop && waitpid(child, &status, WNOHANG) == 0)
        ring_buffer__poll(rb, 100);     // wait up to 100 ms for events
    ring_buffer__consume(rb);           // drain whatever is still in the buffer

    if (stop)
        kill(child, SIGTERM);
    ring_buffer__free(rb);
    mmap_trace_bpf__destroy(skel);
    return 0;
}