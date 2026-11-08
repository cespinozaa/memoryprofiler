// memprof.h: shared by the kernel half and the user half

#ifndef MMAP_TRACE_H
#define MMAP_TRACE_H

#define MAX_STACK_DEPTH 32      // how many return addresses we keep per stack
#define EV_MMAP   0             // a new region was mapped
#define EV_MUNMAP 1             // a region was given back

struct alloc_event {
    unsigned long long addr;    // start of the region
    unsigned long long len;     // size in bytes
    int stack_id;               // key into the stacks map (negative = no stack)
    unsigned int pid;
    int kind;                   // EV_MMAP or EV_MUNMAP
};

#endif