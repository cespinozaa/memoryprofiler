// mmap_trace.bpf.c: runs inside the kernel, records every mmap of one process

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include "mmap_trace.h"

char LICENSE[] SEC("license") = "GPL";

const volatile unsigned int targ_pid = 0;   // user space sets this before load

// scratch: length saved at mmap entry, keyed by pid_tgid (process + thread)
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, u64);
    __type(value, u64);
} pending SEC(".maps");

// call stacks: bpf_get_stackid stores one here and returns its id
struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(max_entries, 1024);
    __uint(key_size, sizeof(u32));
    __uint(value_size, MAX_STACK_DEPTH * sizeof(u64));
} stacks SEC(".maps");

// one-way stream of events to user space
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

// mmap entry: we know the requested length (args[1]) but not the address yet
SEC("tp/syscalls/sys_enter_mmap")
int on_mmap_enter(struct trace_event_raw_sys_enter *ctx)
{
    u64 id = bpf_get_current_pid_tgid();
    u64 len = ctx->args[1];

    if ((id >> 32) != targ_pid){         // upper 32 bits = process id
        return 0;
    }
    bpf_map_update_elem(&pending, &id, &len, BPF_ANY);
    return 0;
}

// mmap exit: we know the returned address (ret), so emit one combined event
SEC("tp/syscalls/sys_exit_mmap")
int on_mmap_exit(struct trace_event_raw_sys_exit *ctx)
{
    u64 id = bpf_get_current_pid_tgid();
    struct alloc_event *e;
    u64 *len;

    if ((id >> 32) != targ_pid){
        return 0;
    }
    len = bpf_map_lookup_elem(&pending, &id);
    if (!len){
        return 0;
    }

    if (ctx->ret >= 0) {                // failed mmap returns a negative error
        e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
        if (e) {
            e->addr = ctx->ret;
            e->len = *len;
            e->pid = id >> 32;
            e->stack_id = bpf_get_stackid(ctx, &stacks, BPF_F_USER_STACK);
            bpf_ringbuf_submit(e, 0);
        }
    }
    bpf_map_delete_elem(&pending, &id);
    return 0;
}