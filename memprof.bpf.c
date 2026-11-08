// memprof.bpf.c: runs inside the kernel, records every mmap and munmap of one process

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include "memprof.h"

char LICENSE[] SEC("license") = "GPL";

const volatile unsigned int targ_pid = 0;   // user space sets this before load

// scratch: length saved at mmap entry, keyed by pid_tgid (process + thread)
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, u64);
    __type(value, u64);
} pending SEC(".maps");

// call stacks: bpf_get_stackid stores one here and returns its id. The table is
// indexed by a hash of the stack, so two different stacks can land on the same slot.
// With only 1024 slots that happened often enough to lose regions; 16384 slots makes
// it rare, and BPF_F_REUSE_STACKID below makes a collision overwrite instead of fail.
struct {
    __uint(type, BPF_MAP_TYPE_STACK_TRACE);
    __uint(max_entries, 16384);
    __uint(key_size, sizeof(u32));
    __uint(value_size, MAX_STACK_DEPTH * sizeof(u64));
} stacks SEC(".maps");

// one-way stream of events to user space (2 MB: a slow read-tracking sample round
// can leave many events waiting before user space gets to them)
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 2 * 1024 * 1024);
} events SEC(".maps");

// mmap entry: we know the requested length (args[1]) but not the address yet
SEC("tp/syscalls/sys_enter_mmap")
int on_mmap_enter(struct trace_event_raw_sys_enter *ctx)
{
    u64 id = bpf_get_current_pid_tgid();
    u64 len = ctx->args[1];

    if ((id >> 32) != targ_pid)         // upper 32 bits = process id
        return 0;
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

    if ((id >> 32) != targ_pid)
        return 0;
    len = bpf_map_lookup_elem(&pending, &id);
    if (!len)
        return 0;

    if (ctx->ret >= 0) {                // failed mmap returns a negative error
        e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
        if (e) {
            e->addr = ctx->ret;
            e->len = *len;
            e->pid = id >> 32;
            e->kind = EV_MMAP;
            e->stack_id = bpf_get_stackid(ctx, &stacks, BPF_F_USER_STACK | BPF_F_REUSE_STACKID);
            bpf_ringbuf_submit(e, 0);
        }
    }
    bpf_map_delete_elem(&pending, &id);
    return 0;
}

// munmap entry: a region is being given back, so tell user space to stop watching it
SEC("tp/syscalls/sys_enter_munmap")
int on_munmap_enter(struct trace_event_raw_sys_enter *ctx)
{
    u64 id = bpf_get_current_pid_tgid();
    struct alloc_event *e;

    if ((id >> 32) != targ_pid)
        return 0;
    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (e) {
        e->addr = ctx->args[0];
        e->len = ctx->args[1];
        e->pid = id >> 32;
        e->stack_id = -1;
        e->kind = EV_MUNMAP;
        bpf_ringbuf_submit(e, 0);
    }
    return 0;
}