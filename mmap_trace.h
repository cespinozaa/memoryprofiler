// mmap_trace.h: shared by the kernel half and the user half

#ifndef MMAP_TRACE_H
#define MMAP_TRACE_H

#define MAX_STACK_DEPTH 32      // how many return addresses we keep per stack

struct alloc_event {
    unsigned long long addr;    // start of the new region
    unsigned long long len;     // size in bytes
    int stack_id;               // key into the stacks map (negative = no stack)
    unsigned int pid;
};

#endif